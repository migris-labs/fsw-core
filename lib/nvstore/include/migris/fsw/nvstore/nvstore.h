/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Non-volatile storage — the flash-backed persistence layer that lets
 * configuration state (operator-tuned parameters, schedule entries,
 * housekeeping-structure definitions, …) survive a reboot. Slice
 * fsw-16, the framework's first persistence primitive.
 *
 * The store keeps a small in-RAM "payload" of typed records and writes
 * it as ONE complete image to flash. Two flash sectors are used as an
 * **A/B ping-pong** pair: each save erases the *older* sector and
 * writes the new image there with the next sequence number. A power
 * loss mid-write leaves one intact copy — `load` validates both and
 * picks the valid copy with the higher sequence number. This is the
 * standard power-safe NVM pattern; on STM32H7 it falls out of the
 * board's 256 KB storage partition (= exactly two 128 KB sectors).
 *
 * The actual flash I/O is behind a seam (`migris_nv_backend_t`),
 * modelled on the fsw-8 event-sink seam. The Zephyr sample supplies a
 * concrete backend over `flash_area_*`; the host unit tests supply a
 * RAM-backed backend — so the image format, A/B logic, CRC and
 * versioning are fully host-testable without flash hardware.
 *
 * A record is `{type:1, len:2 (big-endian), bytes:len}`. Each consumer
 * (the datapool, eventually the schedule, hkstore, mode, …) owns one
 * record type and provides its own pure `serialize` / `deserialize`.
 * `put` replaces an existing record of the same type, so the payload
 * keeps at most one record per type.
 *
 * The on-flash image is internal to fsw-core — it is not a cross-repo
 * wire contract — but it MUST stay readable across firmware versions:
 * `format_version` lets a new firmware refuse an image it cannot
 * decode, and unknown record types in a loaded payload are ignored on
 * `get` (so a structural rollback is non-fatal). Image-format spec:
 * `docs/nv-image-format.md`.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib.
 */

#ifndef MIGRIS_FSW_NVSTORE_NVSTORE_H_
#define MIGRIS_FSW_NVSTORE_NVSTORE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum bytes the in-RAM payload (the full set of records being
 *  saved/loaded) may hold. Compile-time constant so the store is
 *  statically sized (freestanding, no malloc). Override with
 *  ``-DMIGRIS_NVSTORE_PAYLOAD_MAX=<n>``. The default sizes for the
 *  current set of records — datapool (fsw-16) plus the schedule,
 *  hkstore and mode records added in fsw-17. Worst-case at the
 *  fsw-17 defaults is ≈ 1330 B (a full schedule of 16 max-sized TCs
 *  dominates); 1536 leaves comfortable headroom without straying
 *  outside the flash sector (128 KB on the STM32H7). */
#ifndef MIGRIS_NVSTORE_PAYLOAD_MAX
#    define MIGRIS_NVSTORE_PAYLOAD_MAX 1536U
#endif

/** Format-version of the on-flash image. A `load` of an image with a
 *  different version is rejected — a firmware downgrade or a
 *  layout-breaking change must bump this. */
#define MIGRIS_NVSTORE_FORMAT_VERSION 1U

/** Image header, on the wire, all big-endian:
 *      magic:4 'M''N''V''1', format_version:2, seq:4, payload_len:2
 *  total = 12 bytes. */
#define MIGRIS_NVSTORE_HEADER_SIZE 12U

/** Per-record overhead inside the payload: type:1 + len:2 BE. */
#define MIGRIS_NVSTORE_RECORD_OVERHEAD 3U

/** Record types. Each persistence consumer owns one type; the framework
 *  reserves the low block (1..0x7F) and lets a future mission claim the
 *  high block (0x80..0xFF) if it needs its own NV records. */
typedef enum {
    MIGRIS_NVSTORE_RECORD_DATAPOOL = 1, /**< slice fsw-16. */
    MIGRIS_NVSTORE_RECORD_SCHEDULE = 2, /**< slice fsw-17. */
    MIGRIS_NVSTORE_RECORD_HKSTORE = 3,  /**< slice fsw-17. */
    MIGRIS_NVSTORE_RECORD_MODE = 4,     /**< slice fsw-17. */
} migris_nvstore_record_type_t;

/** The flash-I/O seam. Caller-owned vtable + opaque self. All calls
 *  return 0 on success and a non-zero backend-specific code on failure
 *  (the nvstore only ever maps that to ``MIGRIS_NVSTORE_ERR_BACKEND``).
 *  All offsets are *inside* a sector — sector indexing is explicit,
 *  the backend exposes the geometry. */
typedef struct migris_nv_backend {
    /** Read ``len`` bytes from ``sector`` at byte offset ``off``. */
    int (*read)(void* self, uint32_t sector, uint32_t off, void* dst, size_t len);
    /** Write ``len`` bytes to ``sector`` at byte offset ``off``. Offset
     *  and length must respect ``write_block`` alignment. */
    int (*write)(void* self, uint32_t sector, uint32_t off, const void* src, size_t len);
    /** Erase the whole ``sector`` to its erased pattern. */
    int (*erase)(void* self, uint32_t sector);
    /** Geometry. Both A and B share ``sector_size``; the store needs at
     *  least two sectors. ``write_block`` is the minimum write alignment
     *  in bytes (32 on STM32H7, 1 for a RAM backend). */
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t write_block;
    /** Backend-private context, forwarded as the first arg of the
     *  function-pointer calls. */
    void* self;
} migris_nv_backend_t;

/** The store. Caller-owned; zero-initialise once then call
 *  ``migris_nvstore_init``. ``backend`` is borrowed (the caller keeps
 *  ownership). */
typedef struct {
    const migris_nv_backend_t* backend;
    uint8_t payload[MIGRIS_NVSTORE_PAYLOAD_MAX];
    uint16_t payload_len;
    uint32_t loaded_seq;    /**< Sequence number of the loaded image (0 if none). */
    uint32_t loaded_sector; /**< Sector index the loaded image lives in. */
    int loaded;             /**< 0 = no valid image on flash, 1 = loaded. */
} migris_nvstore_t;

/** Status / error codes. Same convention as the rest of the framework:
 *  0 (or a positive byte count) is success, negative is one of these. */
typedef enum {
    MIGRIS_NVSTORE_OK = 0,
    MIGRIS_NVSTORE_ERR_BAD_ARG = -1,        /**< NULL pointer or backend missing a function. */
    MIGRIS_NVSTORE_ERR_NOT_FOUND = -2,      /**< Record type is not in the loaded payload. */
    MIGRIS_NVSTORE_ERR_FULL = -3,           /**< Payload would exceed MIGRIS_NVSTORE_PAYLOAD_MAX. */
    MIGRIS_NVSTORE_ERR_BACKEND = -4,        /**< Backend read/write/erase failed. */
    MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE = -5, /**< Load found no valid image on either sector. */
    MIGRIS_NVSTORE_ERR_GEOMETRY = -6        /**< Backend has < 2 sectors or an image won't fit. */
} migris_nvstore_status_t;

/** Initialise ``store`` with ``backend``. The in-RAM payload starts
 *  empty; call ``migris_nvstore_load`` to restore it from flash. A
 *  zero-initialised ``migris_nvstore_t`` is already valid; this is
 *  provided for explicitness at startup. */
void migris_nvstore_init(migris_nvstore_t* store, const migris_nv_backend_t* backend);

/** Read both flash sectors, validate magic + format_version + CRC, and
 *  populate the in-RAM payload from the valid copy with the higher
 *  sequence number. Returns ``MIGRIS_NVSTORE_OK`` on success (a valid
 *  image was loaded), ``MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE`` if neither
 *  sector holds a valid image (the payload is left empty), or
 *  ``MIGRIS_NVSTORE_ERR_BACKEND`` / ``_ERR_BAD_ARG`` / ``_ERR_GEOMETRY``. */
int migris_nvstore_load(migris_nvstore_t* store);

/** Find the record with ``record_type`` in the loaded payload. On
 *  success ``*out_bytes`` points into the in-RAM payload (valid until
 *  the next mutating call) and ``*out_len`` is the record length.
 *  Returns ``MIGRIS_NVSTORE_OK`` / ``_ERR_NOT_FOUND`` / ``_ERR_BAD_ARG``. */
int migris_nvstore_get(const migris_nvstore_t* store,
                       uint8_t record_type,
                       const uint8_t** out_bytes,
                       uint16_t* out_len);

/** Insert or replace the record for ``record_type`` in the in-RAM
 *  payload with ``len`` bytes from ``bytes``. An existing record of
 *  the same type is removed first. Returns ``MIGRIS_NVSTORE_OK``,
 *  ``MIGRIS_NVSTORE_ERR_FULL`` if the resulting payload would exceed
 *  ``MIGRIS_NVSTORE_PAYLOAD_MAX``, or ``MIGRIS_NVSTORE_ERR_BAD_ARG``.
 *  The payload is only persisted by a subsequent ``migris_nvstore_save``. */
int migris_nvstore_put(migris_nvstore_t* store,
                       uint8_t record_type,
                       const uint8_t* bytes,
                       uint16_t len);

/** Persist the in-RAM payload to flash: erase the older (or empty)
 *  sector and write a fresh image to it with ``seq = loaded_seq + 1``.
 *  Updates ``loaded_seq`` and ``loaded_sector`` on success. The
 *  previous image stays in the other sector — so a power loss during
 *  this call leaves a valid older copy that the next ``load`` will
 *  pick. Returns ``MIGRIS_NVSTORE_OK`` or ``_ERR_BACKEND`` /
 *  ``_ERR_BAD_ARG`` / ``_ERR_GEOMETRY``. */
int migris_nvstore_save(migris_nvstore_t* store);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_NVSTORE_NVSTORE_H_
