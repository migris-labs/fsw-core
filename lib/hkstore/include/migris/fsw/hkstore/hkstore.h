/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board housekeeping-structure store — a bounded set of ground-
 * defined PUS-3 housekeeping structures. Slice fsw-15, the framework's
 * first dynamic (ground-commandable) telemetry-definition store.
 *
 * A "housekeeping structure" is a named, periodically reported group of
 * on-board parameters: a Structure ID (SID), the list of datapool
 * parameter IDs it samples, a reporting interval, and an enabled flag.
 * The PUS-3 service (lib/pus/pus3.{h,c}) is the ground-facing face that
 * creates, deletes, enables and disables structures here ([3,1]/[3,2]/
 * [3,5]/[3,6]); the application's main loop drives the emission tick
 * (migris_hkstore_due) and the PUS-3 codec turns a due structure into a
 * datapool-backed [3,25] housekeeping parameter report.
 *
 * This store deliberately does NOT depend on lib/datapool/: it holds
 * parameter IDs opaquely and never resolves them. A structure may name
 * a parameter the datapool does not define — that is caught at emission
 * time by the PUS-3 codec, which fails the whole report. Keeping the
 * two decoupled means a structure can be created before, or independent
 * of, the parameters it references.
 *
 * A created structure starts DISABLED — flight-safe: a freshly defined
 * structure does not autonomously add to the downlink until ground
 * enables it with a [3,5]. The predefined framework structure
 * FRAMEWORK_DIAG (SID 0x0001) is NOT held here — it is a frozen,
 * hard-coded layout in the PUS-3 codec; this store carries only the
 * mission structures ground defines, whose SIDs must be 0x0100 or above
 * (the 0x0001..0x00FF block is reserved for fsw-core framework
 * structures). migris_hkstore_create rejects a SID below that floor.
 *
 * RAM at runtime, but the persisted set (SID, parameter list,
 * interval and enabled flag of each defined structure) survives a
 * reboot through lib/nvstore/ — slice fsw-17 gives the store a
 * serialize / deserialize pair and a monotonic generation counter the
 * application polls to detect a mutation since the last save. The
 * ``last_emit_sec`` and ``in_use`` fields are deliberately NOT
 * persisted: ``in_use`` is an array-slot artefact (deserialize
 * compacts defined structures into the low slots), and persisting
 * ``last_emit_sec`` against a fresh FSW clock would make the (now -
 * last) due arithmetic underflow as uint32_t and the structure would
 * fire on every tick until the clock caught up. Capacity and the
 * per-structure parameter count are compile-time constants —
 * freestanding, no malloc. Freestanding C — no Zephyr, no stdlib.
 *
 * Byte-level specification of the PUS-3 wire: docs/wire/pus-3.md.
 */

#ifndef MIGRIS_FSW_HKSTORE_HKSTORE_H_
#define MIGRIS_FSW_HKSTORE_HKSTORE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the store, in housekeeping structures. Compile-time
 *  constant so the store is statically sized (freestanding, no malloc).
 *  Override with ``-DMIGRIS_HKSTORE_CAPACITY=<n>`` (the `tc_uart`
 *  sample wires this to Kconfig ``FSW_HKSTORE_CAPACITY``); the host
 *  library uses the default. */
#ifndef MIGRIS_HKSTORE_CAPACITY
#    define MIGRIS_HKSTORE_CAPACITY 8U
#endif

/** Largest parameter list, in parameters, one structure can name. A
 *  longer list is rejected ``MIGRIS_HKSTORE_ERR_TOO_MANY``. Override
 *  with ``-DMIGRIS_HKSTORE_MAX_PARAMS=<n>`` (Kconfig
 *  ``FSW_HKSTORE_MAX_PARAMS`` in the sample). It bounds the worst-case
 *  dynamic [3,25] packet: a structure of N four-byte parameters is a
 *  6 + 10 + (2 + 4·N) + 2 byte report. */
#ifndef MIGRIS_HKSTORE_MAX_PARAMS
#    define MIGRIS_HKSTORE_MAX_PARAMS 8U
#endif

/** Lowest Structure ID a ground-created structure may use. The block
 *  0x0001..0x00FF is reserved for fsw-core framework structures (the
 *  predefined FRAMEWORK_DIAG lives at 0x0001, hard-coded in the PUS-3
 *  codec); a structure created through this store must carry a mission
 *  SID at 0x0100 or above. ``migris_hkstore_create`` rejects a SID
 *  below this floor with ``MIGRIS_HKSTORE_ERR_BAD_ARG``. */
#define MIGRIS_HKSTORE_SID_MIN 0x0100U

/** One housekeeping structure: a Structure ID, the datapool parameter
 *  IDs it samples, a reporting interval and an enabled flag. Parameter
 *  IDs are held opaquely — this store never resolves them against the
 *  datapool. An ``interval_sec`` of 0 means the structure is never
 *  emitted periodically (it can still be polled with a TC[3,27]). */
typedef struct {
    uint16_t sid;                                  /**< Structure ID (>= 0x0100). */
    uint16_t param_ids[MIGRIS_HKSTORE_MAX_PARAMS]; /**< Datapool parameter IDs sampled. */
    size_t param_count;                            /**< Number of valid entries in param_ids. */
    uint32_t interval_sec;  /**< Reporting period, seconds (0 = poll-only). */
    uint32_t last_emit_sec; /**< FSW time of the last periodic emission. */
    int enabled;            /**< 0 = disabled (post-create default), 1 = enabled. */
    int in_use;             /**< 0 = free slot, 1 = a defined structure. */
} migris_hk_structure_t;

/** The housekeeping-structure store. Caller-owned. Zero-initialise
 *  once, then call ``migris_hkstore_init``. Slots are not compacted on
 *  delete, so a borrowed ``migris_hk_structure_t`` pointer stays valid
 *  across the creation or deletion of *other* structures.
 *
 *  ``generation`` is monotonically incremented on every successful
 *  call that mutates the persisted set — create, delete, set_enabled.
 *  The tc_uart sample polls it to detect a mutation since the last
 *  save without re-reading every slot. The field is RAM-only: it
 *  starts at 0 on every boot, is bumped by mutations, and is NOT
 *  included in the on-flash serialised image. */
typedef struct {
    migris_hk_structure_t structures[MIGRIS_HKSTORE_CAPACITY];
    size_t count;        /**< Number of defined (in-use) structures. */
    uint32_t generation; /**< Mutation counter; slice fsw-17. */
} migris_hkstore_t;

/** Housekeeping-store return / error codes. Same convention as the
 *  rest of the framework: 0 is success, negative is one of these. */
typedef enum {
    MIGRIS_HKSTORE_OK = 0,
    MIGRIS_HKSTORE_ERR_BAD_ARG = -1,      /**< NULL pointer, empty list, or SID below 0x0100. */
    MIGRIS_HKSTORE_ERR_FULL = -2,         /**< Store already at capacity. */
    MIGRIS_HKSTORE_ERR_TOO_MANY = -3,     /**< Parameter list over MIGRIS_HKSTORE_MAX_PARAMS. */
    MIGRIS_HKSTORE_ERR_DUPLICATE = -4,    /**< A structure with this SID already exists. */
    MIGRIS_HKSTORE_ERR_NOT_FOUND = -5,    /**< No structure with this SID. */
    MIGRIS_HKSTORE_ERR_TRUNCATED = -6,    /**< Deserialise: input shorter than declared image. */
    MIGRIS_HKSTORE_ERR_BUF_TOO_SMALL = -7 /**< Serialise: output buffer below the image size. */
} migris_hkstore_status_t;

/** Reset ``store`` to empty. A zero-initialised ``migris_hkstore_t`` is
 *  already valid; this is provided for explicitness at startup and to
 *  clear a store that has been used. */
void migris_hkstore_init(migris_hkstore_t* store);

/** Number of structures currently defined. */
size_t migris_hkstore_count(const migris_hkstore_t* store);

/** Create a housekeeping structure with Structure ID ``sid`` sampling
 *  the ``param_count`` datapool parameter IDs at ``param_ids``,
 *  reported every ``interval_sec`` seconds (0 = poll-only). The
 *  structure starts DISABLED — ground enables it with a [3,5]. Fails,
 *  with no state change, with ``MIGRIS_HKSTORE_ERR_BAD_ARG`` (NULL,
 *  ``param_count`` 0, or ``sid`` below ``MIGRIS_HKSTORE_SID_MIN``),
 *  ``_ERR_TOO_MANY`` (``param_count`` over the per-structure maximum),
 *  ``_ERR_DUPLICATE`` (``sid`` already defines a structure), or
 *  ``_ERR_FULL``. Returns ``MIGRIS_HKSTORE_OK`` on success. */
int migris_hkstore_create(migris_hkstore_t* store,
                          uint16_t sid,
                          const uint16_t* param_ids,
                          size_t param_count,
                          uint32_t interval_sec);

/** Delete the structure with Structure ID ``sid``. Returns
 *  ``MIGRIS_HKSTORE_OK``, ``MIGRIS_HKSTORE_ERR_NOT_FOUND``, or
 *  ``MIGRIS_HKSTORE_ERR_BAD_ARG`` (NULL store). */
int migris_hkstore_delete(migris_hkstore_t* store, uint16_t sid);

/** Set the enabled state of the structure with Structure ID ``sid``:
 *  non-zero ``enabled`` permits ``migris_hkstore_due`` to emit it,
 *  zero suspends periodic emission (the structure is retained).
 *  Returns ``MIGRIS_HKSTORE_OK``, ``MIGRIS_HKSTORE_ERR_NOT_FOUND``, or
 *  ``MIGRIS_HKSTORE_ERR_BAD_ARG`` (NULL store). */
int migris_hkstore_set_enabled(migris_hkstore_t* store, uint16_t sid, int enabled);

/** Find the structure with Structure ID ``sid``. Returns a borrowed
 *  pointer into ``store`` (valid until that structure is deleted), or
 *  NULL if no such structure exists / on a NULL argument. */
const migris_hk_structure_t* migris_hkstore_find(const migris_hkstore_t* store, uint16_t sid);

/** Select the next structure due for a periodic report. Among the
 *  enabled structures with a non-zero ``interval_sec`` whose interval
 *  has elapsed (``now_seconds - last_emit_sec >= interval_sec``), the
 *  one waiting longest (earliest ``last_emit_sec``) is chosen, its
 *  ``last_emit_sec`` is stamped to ``now_seconds``, and a borrowed
 *  pointer to it is returned. Returns NULL if nothing is due or on a
 *  NULL argument. Call once per main-loop iteration — one structure is
 *  released per call. */
const migris_hk_structure_t* migris_hkstore_due(migris_hkstore_t* store, uint32_t now_seconds);

/** Mutation counter — strictly monotonic, bumped on every successful
 *  ``migris_hkstore_create`` / ``_delete`` / ``_set_enabled``. NOT
 *  bumped by ``migris_hkstore_due`` — ``last_emit_sec`` is not
 *  persisted, so a due call does not change anything that lands on
 *  flash. Lets the application save the store to NVM when it changes
 *  without polling every slot. Returns 0 on a NULL store. Resets to
 *  0 on every ``migris_hkstore_init`` (the field is NOT persisted —
 *  a fresh boot starts at 0 even after a restored image). */
uint32_t migris_hkstore_generation(const migris_hkstore_t* store);

/** Serialise the in-use structures into ``out`` as a contiguous byte
 *  stream for the ``lib/nvstore/`` persistence layer:
 *
 *      count(2 BE) + { sid(2 BE), interval_sec(4 BE), enabled(1),
 *                      param_count(1), param_ids(2 BE) * N } * count
 *
 *  Only the in-use slots are written, packed back-to-back. The
 *  ``last_emit_sec`` and ``in_use`` fields are deliberately NOT
 *  serialised. Returns the positive byte count written, or a negative
 *  ``migris_hkstore_status_t`` (``_ERR_BUF_TOO_SMALL`` /
 *  ``_ERR_BAD_ARG``). The ``generation`` counter is RAM-only. */
int migris_hkstore_serialize(const migris_hkstore_t* store, uint8_t* out, size_t out_cap);

/** Restore in-use structures from a previously serialised image.
 *  Each restored structure is compacted into the low slots (the
 *  ``in_use`` flag is rebuilt; remaining slots are cleared). Every
 *  restored structure starts with ``last_emit_sec = 0`` — periodic
 *  emission re-arms from the current FSW clock rather than firing on
 *  every tick until ``now`` catches up to a stale stamp. The
 *  ``generation`` counter is NOT bumped (a restore is not a mutation).
 *  On any error the store is reset to empty (stateless failure).
 *  Returns ``MIGRIS_HKSTORE_OK`` on a complete decode, ``_ERR_BAD_ARG``
 *  on a NULL argument, ``_ERR_TRUNCATED`` if the image is short of the
 *  declared payload, ``_ERR_FULL`` if ``count`` exceeds
 *  ``MIGRIS_HKSTORE_CAPACITY``, ``_ERR_TOO_MANY`` if any
 *  ``param_count`` exceeds ``MIGRIS_HKSTORE_MAX_PARAMS``, or
 *  ``_ERR_BAD_ARG`` if any restored SID is below
 *  ``MIGRIS_HKSTORE_SID_MIN`` or duplicates one already restored. */
int migris_hkstore_deserialize(migris_hkstore_t* store, const uint8_t* in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_HKSTORE_HKSTORE_H_
