/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Large-data downlink session — the stateful half of PUS-13. Slice
 * fsw-12.
 *
 * A spacecraft sometimes has to downlink a unit of data larger than a
 * single CCSDS Space Packet — a schedule detail report, a window of
 * stored telemetry, a payload product. PUS-13 (lib/pus/pus13.{h,c})
 * is the stateless part-packet codec; this session is the state
 * machine that drives it: given a borrowed, caller-owned contiguous
 * data unit, it slices the unit into MIGRIS_PUS13_PART_SIZE-byte
 * chunks and emits one [13,1] / [13,2] / [13,3] part per call, so the
 * application's main loop can drip a transfer out one packet per
 * iteration — the same shape the PUS-15 retrieval drain uses.
 *
 * One transfer at a time: a session is IDLE, then ACTIVE from
 * migris_largedata_start until the last part is emitted, then IDLE
 * again. The data unit is *borrowed* — the caller owns the storage and
 * must keep it valid and unchanged for the lifetime of the transfer.
 * The session copies nothing: it is a cursor plus the PUS-13 message
 * counters, which persist across transfers so they stay monotonic
 * (re-initialise only at startup, never per transfer).
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. Byte-level
 * specification of the wire: docs/wire/pus-13.md.
 */

#ifndef MIGRIS_FSW_LARGEDATA_LARGEDATA_H_
#define MIGRIS_FSW_LARGEDATA_LARGEDATA_H_

#include "migris/fsw/pus/pus13.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Upper bound, in bytes, on a large data unit one session transfers.
 *  A sanity cap only — the session borrows the unit and allocates
 *  nothing — so a unit over this is rejected by
 *  ``migris_largedata_start``. Override with
 *  ``-DMIGRIS_LARGEDATA_UNIT_MAX=<n>``. */
#ifndef MIGRIS_LARGEDATA_UNIT_MAX
#    define MIGRIS_LARGEDATA_UNIT_MAX 4096U
#endif

/** Large-data session lifecycle state. */
typedef enum {
    MIGRIS_LARGEDATA_IDLE = 0,  /**< No transfer in progress. */
    MIGRIS_LARGEDATA_ACTIVE = 1 /**< A transfer is in progress; parts remain to emit. */
} migris_largedata_state_t;

/** Large-data session return / error codes. Same convention as the
 *  rest of the framework: 0 (or a positive byte count) is success, a
 *  negative value is one of these. */
typedef enum {
    MIGRIS_LARGEDATA_OK = 0,
    MIGRIS_LARGEDATA_ERR_BAD_ARG = -1,        /**< NULL pointer, or an empty data unit. */
    MIGRIS_LARGEDATA_ERR_BUSY = -2,           /**< A transfer is already in progress. */
    MIGRIS_LARGEDATA_ERR_UNIT_TOO_LARGE = -3, /**< Data unit over MIGRIS_LARGEDATA_UNIT_MAX. */
    MIGRIS_LARGEDATA_ERR_BUF_TOO_SMALL = -4   /**< Output buffer below the next part packet. */
} migris_largedata_status_t;

/** A large-data downlink session. Caller-owned; zero-initialise once,
 *  then call ``migris_largedata_init``. The ``unit`` pointer is
 *  borrowed — the caller owns the bytes and must keep them valid and
 *  unchanged until the transfer completes. */
typedef struct {
    migris_largedata_state_t state;
    const uint8_t* unit;      /**< Borrowed data unit being transferred. */
    size_t unit_len;          /**< Total length of the data unit, bytes. */
    size_t cursor;            /**< Bytes already emitted. */
    uint16_t transaction_id;  /**< Identifier carried in every part of this transfer. */
    uint16_t next_part;       /**< 0-based number of the next part to emit. */
    uint16_t total_parts;     /**< Total parts in this transfer. */
    migris_pus13_ctx_t pus13; /**< PUS-13 message counters — persist across transfers. */
} migris_largedata_session_t;

/** Reset ``session`` to IDLE with zeroed message counters. Call once
 *  at startup, before the first ``migris_largedata_start``. A
 *  zero-initialised ``migris_largedata_session_t`` is already IDLE;
 *  this is the explicit, self-documenting form. */
void migris_largedata_init(migris_largedata_session_t* session);

/** Non-zero iff a transfer is in progress. */
int migris_largedata_active(const migris_largedata_session_t* session);

/** Begin a transfer of ``unit`` (``unit_len`` bytes), tagging every
 *  part with ``transaction_id``. The unit is borrowed — the caller
 *  keeps ownership and must not change or free it until the transfer
 *  completes. Computes the part count and leaves the session ACTIVE.
 *  Returns ``MIGRIS_LARGEDATA_OK``, or a negative
 *  ``migris_largedata_status_t``: ``ERR_BAD_ARG`` (NULL, or
 *  ``unit_len`` 0), ``ERR_BUSY`` (a transfer is already in progress),
 *  or ``ERR_UNIT_TOO_LARGE`` (``unit_len`` over
 *  ``MIGRIS_LARGEDATA_UNIT_MAX``). */
int migris_largedata_start(migris_largedata_session_t* session,
                           uint16_t transaction_id,
                           const uint8_t* unit,
                           size_t unit_len);

/** Emit the next downlink part of the active transfer into ``out``.
 *  Returns the positive part-packet byte count if a part was emitted —
 *  and, when that was the last part, leaves the session IDLE; 0 if no
 *  transfer is in progress; a negative ``migris_largedata_status_t`` on
 *  a NULL argument (``ERR_BAD_ARG``) or an ``out_cap`` below the part
 *  packet (``ERR_BUF_TOO_SMALL``, the session left unchanged so the
 *  caller can retry with a larger buffer). ``tm_seq_count`` is the
 *  shared per-APID CCSDS TM sequence count; ``destination_id`` is 0
 *  for a spontaneous downlink. Call once per main-loop iteration. */
int migris_largedata_next_part(migris_largedata_session_t* session,
                               uint16_t apid,
                               uint16_t* tm_seq_count,
                               uint32_t now_seconds,
                               uint16_t destination_id,
                               uint8_t* out,
                               size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_LARGEDATA_LARGEDATA_H_
