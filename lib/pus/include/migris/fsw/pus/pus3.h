/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-3 — Housekeeping & diagnostic data reporting. Subtypes shipped:
 *
 *   * Subtype [25] (TM) → "Housekeeping parameter report". Emitted
 *     spontaneously on a fixed period (the application's timing loop
 *     owns the cadence) and on demand in response to a [27] poll.
 *   * Subtype [27] (TC) → "Generate a one-shot housekeeping parameter
 *     report". Application data is exactly one Structure ID.
 *   * Subtypes [1]/[2] (TC) → create / delete a housekeeping structure
 *     (slice fsw-15).
 *   * Subtypes [5]/[6] (TC) → enable / disable a structure's periodic
 *     report (slice fsw-15).
 *
 * Slice fsw-7 shipped only [25]/[27] against a single hard-coded
 * framework structure (FRAMEWORK_DIAG); slice fsw-15 adds the
 * structure-management surface. A ground-created (dynamic) structure
 * selects a list of parameters from the on-board datapool
 * (lib/datapool/) and is held in the housekeeping-structure store
 * (lib/hkstore/); a [25] report for it serialises those parameters'
 * MIB-decoded values. FRAMEWORK_DIAG stays a frozen, hard-coded layout
 * — migris_pus3_build_hk_report is byte-for-byte unchanged.
 *
 * Still deferred (see CHANGELOG.md): the diagnostic-structure subtypes
 * [3]/[4] and the diagnostic report [26]; structure-definition
 * reporting [9]/[10]; super-commutated parameter groups [7]/[8].
 *
 * Like PUS-5, PUS-3 is partly *asynchronous*: the periodic report is
 * emitted spontaneously at the cadence point, not as a side effect of
 * an inbound TC. This header is a pure report *encoder* — the caller
 * decides when the period elapses, owns the parameter snapshot, and
 * owns the output buffer; here we only serialise one report. The exact
 * same encoder serves the spontaneous and the [27]-polled paths (they
 * differ only in destination ID, a caller argument) — one straight-line
 * serialiser, the proven pus1/pus5/pus17 shape. Freestanding C — no
 * Zephyr, no malloc, no stdlib.
 *
 * Structure IDs are a frozen cross-repo contract. The range
 * 0x0001..0x00FF is reserved for fsw-core *framework* housekeeping
 * structures; mission flight software (cry4-fsw) owns 0x0100 and above
 * (the exact mission-side scheme is pinned when cry4-fsw bootstraps).
 * This mirrors the PUS-5 event-ID block split and the pinned "PUS-128+
 * vendor assignments live in cry4 / cry4-fsw, not in fsw-core"
 * decision. Only the one framework structure this slice emits is
 * defined below (smallest viable surface, like pus1.h / pus5.h
 * deferring their unused subtypes).
 *
 * Byte-level specification: docs/wire/pus-3.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS3_H_
#define MIGRIS_FSW_PUS_PUS3_H_

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/hkstore/hkstore.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_HOUSEKEEPING 3U

#define MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT 25U /**< TM, the report itself. */
#define MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL 27U   /**< TC, "generate one shot". */

/** Structure-management subtypes (TC), slice fsw-15. Create / delete a
 *  housekeeping structure and enable / disable its periodic report.
 *  Application data: [1] = SID(2) + parameter-count(1) + parameter
 *  ID(2 each) + interval seconds(4); [2]/[5]/[6] = SID(2). */
#define MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE 1U
#define MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE 2U
#define MIGRIS_PUS3_SUBTYPE_ENABLE_STRUCTURE 5U
#define MIGRIS_PUS3_SUBTYPE_DISABLE_STRUCTURE 6U

/** Structure ID = a 2-byte big-endian identifier. It is the first
 *  field of every housekeeping parameter report's source data, and the
 *  entire application-data payload of a [27] one-shot-poll request. */
#define MIGRIS_PUS3_SID_SIZE 2U
typedef uint16_t migris_pus3_sid_t;

/** fsw-core framework housekeeping structure IDs (reserved block
 *  0x0001..0x00FF). Mission structures live at 0x0100+ and are not
 *  defined here. The one structure this slice emits reports the
 *  framework's own diagnostic state (uptime, the shared TM sequence
 *  count, per-service PUS message counters, and the TC router's
 *  accepted / rejected / RX-overflow counters). */
#define MIGRIS_PUS3_SID_FRAMEWORK_DIAG 0x0001U

/** Source data of the framework structure: SID (2) followed by a
 *  fixed 27-byte parameter block (see docs/wire/pus-3.md). The block
 *  is a frozen layout — widening or narrowing it is a breaking wire
 *  change; adding a new structure under a new SID is not. */
#define MIGRIS_PUS3_HK_SOURCE_DATA_SIZE 29U

/** Total wire size of a [25] report for the framework structure:
 *  primary (6) + TM sec header (10) + source data (29) + CRC (2). */
#define MIGRIS_PUS3_HK_TM_PACKET_SIZE 47U

/** A [27] one-shot-poll TC carries exactly one Structure ID as its
 *  application data (this slice has a single structure; a SID list is
 *  deferred with the datapool). */
#define MIGRIS_PUS3_POLL_TC_APP_DATA_SIZE 2U

/** PUS-3 encoder return / error codes. Same convention as the rest of
 *  the codec: a positive value is the byte count written, a negative
 *  value is one of these. */
typedef enum {
    MIGRIS_PUS3_OK = 0,
    MIGRIS_PUS3_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < required packet size. */
    MIGRIS_PUS3_ERR_BAD_ARG = -2,       /**< NULL argument, or malformed TC application data. */
    MIGRIS_PUS3_ERR_UNKNOWN_SID = -3,   /**< SID is not a defined structure. */
    MIGRIS_PUS3_ERR_UNKNOWN_PARAM = -4, /**< A structure names a parameter the datapool lacks. */
    MIGRIS_PUS3_ERR_EXEC_FAILED = -5    /**< A structure-management operation was rejected. */
} migris_pus3_status_t;

/** Per-(service, subtype) message counter for the PUS-3 report
 *  subtype this slice emits. Index [0] = housekeeping parameter
 *  report (25); the array reserves room for further report subtypes
 *  without an ABI change (same pattern as pus1.h / pus5.h).
 *  Caller-owned; zero-initialised once at startup. The CCSDS TM
 *  sequence count is *not* here — it is shared per-APID across every
 *  service and lives in the TC router context. */
typedef struct {
    uint8_t msg_counter[1];
} migris_pus3_ctx_t;

/** Immutable snapshot of the framework structure's parameter set, as
 *  observed at one instant by the caller. Kept here (not a
 *  ``tc_router.h`` type) on purpose: ``tc_router.h`` includes this
 *  header to embed ``migris_pus3_ctx_t``, so this header must not
 *  depend on ``tc_router.h``. The caller (the sample loop for the
 *  spontaneous report; the TC router for a [27] poll) fills this from
 *  the router context plus its ISR RX-overflow counter snapshot.
 *
 *  ``pus5_msg_counter`` reflects the framework PUS-5 counters. As of
 *  slice fsw-8 the TC router owns the PUS-5 context, so both the
 *  spontaneous report and a [27]-polled report carry the *live*
 *  values — the fsw-7 zero-on-the-polled-path asymmetry is resolved
 *  (see docs/wire/pus-3.md). */
typedef struct {
    uint8_t pus1_msg_counter[4];     /**< accept-ok / accept-fail / compl-ok / compl-fail. */
    uint8_t pus5_msg_counter[4];     /**< info / low / medium / high. */
    uint8_t pus17_tm_msg_counter;    /**< PUS-17[2] message counter. */
    uint32_t tc_accepted_count;      /**< TCs that passed acceptance. */
    uint32_t tc_rejected_count;      /**< Addressed TCs that failed acceptance. */
    uint32_t rx_ring_overflow_drops; /**< UART RX-ring bytes dropped (ISR). */
} migris_pus3_hk_params_t;

/** Encode one PUS-3[25] housekeeping parameter report into ``out``.
 *
 *  ``sid`` selects the structure. The only structure this slice
 *  defines is ``MIGRIS_PUS3_SID_FRAMEWORK_DIAG``; any other SID
 *  returns ``MIGRIS_PUS3_ERR_UNKNOWN_SID`` with no side effects.
 *  ``params`` is the caller-built parameter snapshot serialised into
 *  the report's source data. ``destination_id`` is ``0`` for the
 *  spontaneous periodic report (no triggering TC) and echoes the
 *  triggering TC's source ID for a [27] poll, consistent with
 *  PUS-1 / PUS-17. ``tm_seq_count`` is the shared per-APID CCSDS TM
 *  sequence count: its pre-increment value is both written into the
 *  CCSDS primary header *and* serialised into the parameter block (so
 *  the report tells the ground the count it itself consumed), then
 *  advanced mod 2^14.
 *
 *  Returns ``MIGRIS_PUS3_HK_TM_PACKET_SIZE`` (positive — bytes
 *  written) on success, or a negative ``migris_pus3_status_t``. Side
 *  effects (the shared sequence count and ``ctx->msg_counter[0]``)
 *  advance on success only. */
int migris_pus3_build_hk_report(migris_pus3_ctx_t* ctx,
                                uint16_t apid,
                                uint16_t* tm_seq_count,
                                uint32_t now_seconds,
                                migris_pus3_sid_t sid,
                                const migris_pus3_hk_params_t* params,
                                uint16_t destination_id,
                                uint8_t* out,
                                size_t out_cap);

/** Execute a PUS-3 housekeeping structure-management telecommand
 *  against ``store``. ``service_subtype`` is one of
 *  ``MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE`` ([3,1]),
 *  ``..._DELETE_STRUCTURE`` ([3,2]), ``..._ENABLE_STRUCTURE`` ([3,5])
 *  or ``..._DISABLE_STRUCTURE`` ([3,6]); ``app_data`` (``app_len``
 *  bytes) is the telecommand application data — for a create, SID(2) +
 *  parameter-count(1) + parameter ID(2 each) + interval seconds(4); for
 *  the others, SID(2). The decode is all-or-nothing.
 *
 *  These telecommands produce no telemetry of their own — the TC router
 *  emits only the PUS-1 verification — so this returns a status, not a
 *  byte count: ``MIGRIS_PUS3_OK`` on success, ``MIGRIS_PUS3_ERR_BAD_ARG``
 *  on a NULL argument / malformed application data / a non-management
 *  subtype, or ``MIGRIS_PUS3_ERR_EXEC_FAILED`` if the store rejected the
 *  operation (unknown or duplicate SID, store full, parameter list too
 *  long, framework-range SID). */
int migris_pus3_execute(migris_hkstore_t* store,
                        uint8_t service_subtype,
                        const uint8_t* app_data,
                        size_t app_len);

/** Encode one PUS-3[25] housekeeping parameter report for a dynamic
 *  (ground-created) ``structure`` into ``out``.
 *
 *  The report's source data is the 2-byte big-endian Structure ID
 *  followed by the MIB-decoded big-endian value of every datapool
 *  parameter the structure names, in order — each value at the on-wire
 *  width its datapool type dictates, not self-describing (ground
 *  decodes from the structure definition and the MIB). A parameter the
 *  ``datapool`` does not define fails the *whole* report with
 *  ``MIGRIS_PUS3_ERR_UNKNOWN_PARAM`` — no partial packet, deterministic
 *  length. ``destination_id`` is ``0`` for a spontaneous periodic
 *  report and the triggering TC's source ID for a [27] poll, exactly as
 *  for ``migris_pus3_build_hk_report``. ``ctx->msg_counter[0]`` (the
 *  [25] message counter) and the shared ``tm_seq_count`` advance on
 *  success only.
 *
 *  Returns the positive packet byte count on success, or a negative
 *  ``migris_pus3_status_t`` (``_ERR_BAD_ARG`` / ``_ERR_UNKNOWN_PARAM``
 *  / ``_ERR_BUF_TOO_SMALL``). FRAMEWORK_DIAG is *not* served here — it
 *  is the frozen layout of ``migris_pus3_build_hk_report``. */
int migris_pus3_build_dynamic_hk_report(migris_pus3_ctx_t* ctx,
                                        const migris_datapool_t* datapool,
                                        const migris_hk_structure_t* structure,
                                        uint16_t apid,
                                        uint16_t* tm_seq_count,
                                        uint32_t now_seconds,
                                        uint16_t destination_id,
                                        uint8_t* out,
                                        size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS3_H_
