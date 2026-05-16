/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * TC reception, acceptance and routing — the Migris flight-software
 * framework's first on-board dispatcher (slice fsw-5).
 *
 * One inbound CCSDS Space Packet enters; zero or more TM packets
 * leave, written back-to-back into the caller's buffer:
 *
 *   1. Accept stage — validate the CCSDS primary header, packet
 *      length, error-control CRC, PUS-C version, and that the service
 *      type is routable on this application process. The verdict is a
 *      ``migris_pus1_failure_code_t``.
 *   2. If the triggering TC requested it (ACK_ACCEPTANCE), emit a
 *      PUS-1[1] (accepted) or PUS-1[2] (failed, with code). On an
 *      acceptance failure the TC is not routed and there is no
 *      completion report.
 *   3. Route an accepted TC to its service handler (PUS-17 today),
 *      which may emit its own TM.
 *   4. If the TC requested it (ACK_COMPLETION), emit a PUS-1[7]
 *      (success) or PUS-1[8] (failure, with code).
 *
 * The CCSDS TM sequence count is shared per-APID across every emitted
 * packet (CCSDS 133.0-B-2: one count space per APID per direction);
 * it lives here, not in the per-service contexts. Freestanding C.
 *
 * Byte-level specification: docs/wire/pus-1.md (and pus-17.md for the
 * shared primary header / framing / CRC).
 */

#ifndef MIGRIS_FSW_PUS_TC_ROUTER_H_
#define MIGRIS_FSW_PUS_TC_ROUTER_H_

#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus3.h"
#include "migris/fsw/pus/pus17.h"
#include "migris/fsw/pus/pus_tc.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Upper bound on the bytes one inbound TC can produce. The largest
 *  single-TC burst is a PUS-1[1] acceptance report (22) + the routed
 *  service response + a PUS-1[7] completion report (22). The biggest
 *  service response is a PUS-3[25] housekeeping parameter report (47,
 *  vs PUS-17[2]'s 18), so the worst case is 22 + 47 + 22 = 91, rounded
 *  up for headroom. The caller's output buffer must be at least this
 *  large; the router checks once up front so no individual report can
 *  run out of space mid-burst. */
#define MIGRIS_TC_ROUTER_MAX_TM 96U

/** TC router return / error codes. A non-negative return value from
 *  ``migris_tc_router_dispatch`` is the number of TM bytes written
 *  (0 means "nothing to send" — e.g. a TC not addressed to this AP,
 *  or an accepted TC that requested no verification). */
typedef enum {
    MIGRIS_TC_ROUTER_OK = 0,
    MIGRIS_TC_ROUTER_ERR_BUF_TOO_SMALL = -1 /**< out_cap < MIGRIS_TC_ROUTER_MAX_TM. */
} migris_tc_router_status_t;

/** Router state for one application process. Caller-owned,
 *  zero-initialised once at startup (then set ``apid``). Holds the
 *  shared per-APID CCSDS TM sequence count and each service's PUS
 *  message-counter sub-context. */
typedef struct {
    uint16_t apid;            /**< APID this AP receives on and emits with. */
    uint16_t tm_seq_count;    /**< Shared CCSDS TM sequence count (mod 2^14). */
    migris_pus1_ctx_t pus1;   /**< PUS-1 per-subtype message counters. */
    migris_pus17_ctx_t pus17; /**< PUS-17 message counter. */
    migris_pus3_ctx_t pus3;   /**< PUS-3 housekeeping report message counter. */
    /** TCs addressed to this AP that passed acceptance. Owned and
     *  advanced by ``migris_tc_router_dispatch``; reported in the
     *  framework PUS-3 housekeeping structure. */
    uint32_t tc_accepted_count;
    /** TCs addressed to this AP that failed acceptance (length / CRC /
     *  PUS version / unknown service). Owned and advanced by
     *  ``migris_tc_router_dispatch``. */
    uint32_t tc_rejected_count;
    /** UART RX-ring bytes dropped on overflow. *Not* owned by the
     *  router: the application snapshots its ISR-side counter into this
     *  field before each dispatch / spontaneous report; the router only
     *  reads it into a PUS-3[25] report. */
    uint32_t rx_ring_overflow_drops;
} migris_tc_router_ctx_t;

/** Result of the generic accept-stage validation. ``addressed`` is 0
 *  when the packet is not a well-formed TC for this AP (bad primary
 *  header, or another APID) — the router emits nothing in that case.
 *  When ``addressed`` is 1, ``fc`` is ``MIGRIS_PUS1_FC_NONE`` if the
 *  TC passed acceptance, otherwise the failure cause. The remaining
 *  fields are populated only when the TC secondary header was
 *  parseable (i.e. ``fc`` is not ``MIGRIS_PUS1_FC_LENGTH_ERROR``). */
typedef struct {
    int addressed;
    migris_pus1_failure_code_t fc;
    uint8_t ack_flags;
    uint16_t source_id;
    uint8_t service_type;
    uint8_t service_subtype;
} migris_tc_accept_result_t;

/** Run the generic accept-stage validation on ``tc`` (``tc_len``
 *  bytes) for application process ``expected_apid``. Pure function,
 *  no side effects; ``*out`` is fully written. Exposed so the
 *  acceptance contract can be unit-tested independently of report
 *  emission. */
void migris_tc_accept(const uint8_t* tc,
                      size_t tc_len,
                      uint16_t expected_apid,
                      migris_tc_accept_result_t* out);

/** Receive one CCSDS Space Packet, verify and route it, and write the
 *  resulting verification / service TM packets contiguously into
 *  ``out``. Returns the total byte count written (>= 0), or
 *  ``MIGRIS_TC_ROUTER_ERR_BUF_TOO_SMALL`` if ``out_cap`` is below
 *  ``MIGRIS_TC_ROUTER_MAX_TM``. Context counters advance only for
 *  packets actually emitted. */
int migris_tc_router_dispatch(migris_tc_router_ctx_t* ctx,
                              uint32_t now_seconds,
                              const uint8_t* tc,
                              size_t tc_len,
                              uint8_t* out,
                              size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_TC_ROUTER_H_
