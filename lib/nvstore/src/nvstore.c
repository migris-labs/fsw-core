/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Non-volatile storage — see migris/fsw/nvstore/nvstore.h for the
 * contract, on-flash image format, and rationale.
 *
 * Algorithm: two sectors hold A/B copies of the same image (header +
 * payload + CRC). `load` validates both and picks the higher-seq valid
 * copy; `save` writes the new image to the OTHER (older) sector with
 * `seq + 1`. A power loss mid-`save` leaves the previous copy intact
 * in the other sector — the next `load` recovers to it.
 */

#include "migris/fsw/nvstore/nvstore.h"

#include "migris/fsw/pus/ccsds.h" /* migris_crc16_ccitt_false — generic CCITT-FALSE CRC. */

#include <stddef.h>
#include <stdint.h>

/* On-flash image header layout: 'M' 'N' 'V' '1' | version(2 BE) |
 * seq(4 BE) | payload_len(2 BE). 12 bytes total. */
static const uint8_t k_magic[4] = {'M', 'N', 'V', '1'};

/* Maximum backend write_block we will see in practice. STM32H7 is 32;
 * 64 leaves room for a hypothetical future device. Used to size the
 * on-stack image buffer in `save`. Anonymous enum because preprocessor
 * `#define` of a bare integral constant trips clang-tidy's
 * `cppcoreguidelines-macro-to-enum`. */
enum { NVSTORE_WRITE_BLOCK_MAX = 64 };

/* Maximum on-stack image buffer in `save` — header + payload + CRC +
 * one block of trailing padding. */
enum {
    NVSTORE_IMAGE_MAX =
        (int)(MIGRIS_NVSTORE_HEADER_SIZE + MIGRIS_NVSTORE_PAYLOAD_MAX + 2U) + NVSTORE_WRITE_BLOCK_MAX
};

static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFU);
}

/* True iff the four bytes at `p` are the magic 'M' 'N' 'V' '1'. */
static int magic_matches(const uint8_t* p) {
    return (p[0] == k_magic[0] && p[1] == k_magic[1] && p[2] == k_magic[2] && p[3] == k_magic[3])
               ? 1
               : 0;
}

/* The backend is fully usable iff every function pointer is set, both
 * sectors exist, write_block is at least 1 and divides nothing we
 * impose (alignment is the backend's affair on a per-write basis), and
 * the maximum image fits in a sector. */
static int backend_ok(const migris_nv_backend_t* b) {
    if (b == NULL || b->read == NULL || b->write == NULL || b->erase == NULL) {
        return 0;
    }
    if (b->sector_count < 2U || b->write_block == 0U || b->write_block > NVSTORE_WRITE_BLOCK_MAX) {
        return 0;
    }
    /* The largest image we may ever write must fit in a sector
     * (header + payload + CRC + padding to a write_block multiple). */
    const uint32_t image_max =
        MIGRIS_NVSTORE_HEADER_SIZE + MIGRIS_NVSTORE_PAYLOAD_MAX + 2U + b->write_block;
    if (b->sector_size < image_max) {
        return 0;
    }
    return 1;
}

void migris_nvstore_init(migris_nvstore_t* store, const migris_nv_backend_t* backend) {
    if (store == NULL) {
        return;
    }
    store->backend = backend;
    store->payload_len = 0U;
    store->loaded_seq = 0U;
    store->loaded_sector = 0U;
    store->loaded = 0;
}

/* Read one sector's image into `store->payload` and validate it.
 * On success, fills *out_seq and *out_payload_len. On any structural or
 * CRC failure, returns 0 with the payload buffer's contents unspecified
 * (the caller is expected to either retry the other sector or treat
 * the load as empty). */
static int nvstore_try_load_sector(migris_nvstore_t* store,
                                   uint32_t sector,
                                   uint32_t* out_seq,
                                   uint16_t* out_payload_len) {
    const migris_nv_backend_t* b = store->backend;
    uint8_t header[MIGRIS_NVSTORE_HEADER_SIZE];
    if (b->read(b->self, sector, 0U, header, sizeof(header)) != 0) {
        return 0;
    }
    if (magic_matches(header) == 0) {
        return 0;
    }
    const uint16_t version = read_u16_be(&header[4]);
    if (version != MIGRIS_NVSTORE_FORMAT_VERSION) {
        return 0;
    }
    const uint32_t seq = read_u32_be(&header[6]);
    const uint16_t payload_len = read_u16_be(&header[10]);
    if (payload_len > MIGRIS_NVSTORE_PAYLOAD_MAX) {
        return 0;
    }
    if (payload_len > 0U) {
        if (b->read(b->self, sector, MIGRIS_NVSTORE_HEADER_SIZE, store->payload, payload_len) !=
            0) {
            return 0;
        }
    }
    uint8_t crc_buf[2];
    if (b->read(
            b->self, sector, MIGRIS_NVSTORE_HEADER_SIZE + payload_len, crc_buf, sizeof(crc_buf)) !=
        0) {
        return 0;
    }
    /* CRC over header concatenated with payload — exactly the bytes
     * that precede the CRC on flash. `migris_crc16_ccitt_false` uses a
     * fixed 0xFFFF initial value (no streaming API), so the bytes are
     * staged into a single contiguous buffer for one CRC pass. */
    uint8_t combined[MIGRIS_NVSTORE_HEADER_SIZE + MIGRIS_NVSTORE_PAYLOAD_MAX];
    for (size_t i = 0U; i < MIGRIS_NVSTORE_HEADER_SIZE; ++i) {
        combined[i] = header[i];
    }
    for (size_t i = 0U; i < payload_len; ++i) {
        combined[MIGRIS_NVSTORE_HEADER_SIZE + i] = store->payload[i];
    }
    const uint16_t crc =
        migris_crc16_ccitt_false(combined, MIGRIS_NVSTORE_HEADER_SIZE + payload_len);
    const uint16_t crc_on_flash = read_u16_be(crc_buf);
    if (crc != crc_on_flash) {
        return 0;
    }
    *out_seq = seq;
    *out_payload_len = payload_len;
    return 1;
}

int migris_nvstore_load(migris_nvstore_t* store) {
    if (store == NULL) {
        return MIGRIS_NVSTORE_ERR_BAD_ARG;
    }
    if (backend_ok(store->backend) == 0) {
        return store->backend == NULL ? MIGRIS_NVSTORE_ERR_BAD_ARG : MIGRIS_NVSTORE_ERR_GEOMETRY;
    }
    /* Pass 1: header-only validate each sector, pick the highest-seq
     * candidate, then confirm with payload+CRC. If the highest-seq
     * candidate fails CRC, fall back to the other one. The store keeps
     * only two A/B sectors so a small fixed array suffices. */
    const migris_nv_backend_t* b = store->backend;
    int valid[2] = {0, 0};
    uint32_t seq[2] = {0U, 0U};
    for (uint32_t i = 0U; i < 2U && i < b->sector_count; ++i) {
        uint8_t header[MIGRIS_NVSTORE_HEADER_SIZE];
        if (b->read(b->self, i, 0U, header, sizeof(header)) != 0) {
            continue;
        }
        if (magic_matches(header) == 0) {
            continue;
        }
        if (read_u16_be(&header[4]) != MIGRIS_NVSTORE_FORMAT_VERSION) {
            continue;
        }
        if (read_u16_be(&header[10]) > MIGRIS_NVSTORE_PAYLOAD_MAX) {
            continue;
        }
        valid[i] = 1;
        seq[i] = read_u32_be(&header[6]);
    }

    /* Order of attempts: highest seq first; ties (impossible in
     * practice) break to lower index. */
    uint32_t first = 0U;
    uint32_t second = 1U;
    if (valid[1] != 0 && (valid[0] == 0 || seq[1] > seq[0])) {
        first = 1U;
        second = 0U;
    }

    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        const uint32_t sector = (pass == 0U) ? first : second;
        if (valid[sector] == 0) {
            continue;
        }
        uint32_t loaded_seq = 0U;
        uint16_t loaded_payload_len = 0U;
        if (nvstore_try_load_sector(store, sector, &loaded_seq, &loaded_payload_len) != 0) {
            store->payload_len = loaded_payload_len;
            store->loaded_seq = loaded_seq;
            store->loaded_sector = sector;
            store->loaded = 1;
            return MIGRIS_NVSTORE_OK;
        }
    }

    /* Either sector had a parseable header but its payload+CRC failed,
     * or neither sector had a parseable header. Leave the payload
     * empty so the caller starts fresh. */
    store->payload_len = 0U;
    store->loaded_seq = 0U;
    store->loaded_sector = 0U;
    store->loaded = 0;
    return MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE;
}

/* Locate the record with `record_type` in `store->payload`. On a
 * found match, *off and *total are set (off = offset of the type
 * byte, total = 3 + len). Returns 1 on found, 0 on not found, -1 on
 * structural corruption (a record's len walks past the payload). */
static int nvstore_locate(const migris_nvstore_t* store,
                          uint8_t record_type,
                          uint16_t* out_off,
                          uint16_t* out_total) {
    uint16_t off = 0U;
    while (off + MIGRIS_NVSTORE_RECORD_OVERHEAD <= store->payload_len) {
        const uint8_t type = store->payload[off];
        const uint16_t len = read_u16_be(&store->payload[off + 1U]);
        const uint32_t total = (uint32_t)MIGRIS_NVSTORE_RECORD_OVERHEAD + (uint32_t)len;
        if ((uint32_t)off + total > (uint32_t)store->payload_len) {
            return -1; /* corruption — record extends past payload */
        }
        if (type == record_type) {
            *out_off = off;
            *out_total = (uint16_t)total;
            return 1;
        }
        off = (uint16_t)(off + total);
    }
    return 0;
}

int migris_nvstore_get(const migris_nvstore_t* store,
                       uint8_t record_type,
                       const uint8_t** out_bytes,
                       uint16_t* out_len) {
    if (store == NULL || out_bytes == NULL || out_len == NULL) {
        return MIGRIS_NVSTORE_ERR_BAD_ARG;
    }
    uint16_t off = 0U;
    uint16_t total = 0U;
    const int found = nvstore_locate(store, record_type, &off, &total);
    if (found != 1) {
        return MIGRIS_NVSTORE_ERR_NOT_FOUND;
    }
    *out_bytes = &store->payload[off + MIGRIS_NVSTORE_RECORD_OVERHEAD];
    *out_len = (uint16_t)(total - MIGRIS_NVSTORE_RECORD_OVERHEAD);
    return MIGRIS_NVSTORE_OK;
}

int migris_nvstore_put(migris_nvstore_t* store,
                       uint8_t record_type,
                       const uint8_t* bytes,
                       uint16_t len) {
    if (store == NULL || (bytes == NULL && len > 0U)) {
        return MIGRIS_NVSTORE_ERR_BAD_ARG;
    }
    /* Drop any existing record with this type so put = replace. */
    uint16_t off = 0U;
    uint16_t total = 0U;
    if (nvstore_locate(store, record_type, &off, &total) == 1) {
        const uint16_t tail = (uint16_t)(store->payload_len - off - total);
        for (uint16_t i = 0U; i < tail; ++i) {
            store->payload[off + i] = store->payload[off + total + i];
        }
        store->payload_len = (uint16_t)(store->payload_len - total);
    }
    const uint32_t new_total =
        (uint32_t)store->payload_len + (uint32_t)MIGRIS_NVSTORE_RECORD_OVERHEAD + (uint32_t)len;
    if (new_total > MIGRIS_NVSTORE_PAYLOAD_MAX) {
        return MIGRIS_NVSTORE_ERR_FULL;
    }
    store->payload[store->payload_len] = record_type;
    write_u16_be(&store->payload[store->payload_len + 1U], len);
    for (uint16_t i = 0U; i < len; ++i) {
        store->payload[store->payload_len + MIGRIS_NVSTORE_RECORD_OVERHEAD + i] = bytes[i];
    }
    store->payload_len = (uint16_t)new_total;
    return MIGRIS_NVSTORE_OK;
}

int migris_nvstore_save(migris_nvstore_t* store) {
    if (store == NULL) {
        return MIGRIS_NVSTORE_ERR_BAD_ARG;
    }
    if (backend_ok(store->backend) == 0) {
        return store->backend == NULL ? MIGRIS_NVSTORE_ERR_BAD_ARG : MIGRIS_NVSTORE_ERR_GEOMETRY;
    }
    const migris_nv_backend_t* b = store->backend;
    /* Target the sector NOT holding the loaded image — power-safe
     * ping-pong. If nothing is loaded, target sector 0 (sector 1 stays
     * available as the "older" copy after the first save). */
    const uint32_t target = store->loaded ? ((store->loaded_sector + 1U) % b->sector_count) : 0U;
    const uint32_t new_seq = store->loaded_seq + 1U;

    /* Build the image in a contiguous on-stack buffer: header +
     * payload + CRC, padded to a write_block multiple with 0xFF. One
     * write call per save keeps backend alignment trivial. */
    uint8_t image[NVSTORE_IMAGE_MAX];
    /* Header. */
    for (size_t i = 0U; i < sizeof(k_magic); ++i) {
        image[i] = k_magic[i];
    }
    write_u16_be(&image[4], MIGRIS_NVSTORE_FORMAT_VERSION);
    write_u32_be(&image[6], new_seq);
    write_u16_be(&image[10], store->payload_len);
    /* Payload. */
    for (uint16_t i = 0U; i < store->payload_len; ++i) {
        image[MIGRIS_NVSTORE_HEADER_SIZE + i] = store->payload[i];
    }
    /* CRC over header + payload. */
    const uint16_t crc = migris_crc16_ccitt_false(
        image, (size_t)MIGRIS_NVSTORE_HEADER_SIZE + (size_t)store->payload_len);
    write_u16_be(&image[MIGRIS_NVSTORE_HEADER_SIZE + store->payload_len], crc);
    /* Pad to write_block. */
    const uint32_t raw_size =
        (uint32_t)MIGRIS_NVSTORE_HEADER_SIZE + (uint32_t)store->payload_len + 2U;
    const uint32_t padded_size =
        ((raw_size + b->write_block - 1U) / b->write_block) * b->write_block;
    for (uint32_t i = raw_size; i < padded_size; ++i) {
        image[i] = 0xFFU;
    }

    if (b->erase(b->self, target) != 0) {
        return MIGRIS_NVSTORE_ERR_BACKEND;
    }
    if (b->write(b->self, target, 0U, image, padded_size) != 0) {
        return MIGRIS_NVSTORE_ERR_BACKEND;
    }

    store->loaded_seq = new_seq;
    store->loaded_sector = target;
    store->loaded = 1;
    return MIGRIS_NVSTORE_OK;
}
