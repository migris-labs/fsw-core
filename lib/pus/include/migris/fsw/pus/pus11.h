/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-11 — On-board (time-based) scheduling. Slice fsw-10 ships a
 * pragmatic core subset of ECSS-E-ST-70-41C service 11:
 *
 *   * Subtype [1]  (TC) → enable the schedule execution function.
 *   * Subtype [2]  (TC) → disable the schedule execution function.
 *   * Subtype [3]  (TC) → reset the schedule (delete all activities).
 *   * Subtype [4]  (TC) → insert activities into the schedule.
 *   * Subtype [5]  (TC) → delete activities, by request identifier.
 *   * Subtype [11] (TC) → summary-report activities, by request id.
 *   * Subtype [12] (TM) → time-based schedule summary report.
 *
 * Each scheduled activity is one telecommand plus an absolute CUC
 * release time; it is identified by a 4-byte REQUEST IDENTIFIER — the
 * first four bytes of that telecommand (the same identifier PUS-1
 * uses). At release time the application's main loop hands the stored
 * TC back to the TC router for normal dispatch.
 *
 * Insert ([4]) and delete ([5]) are ALL-OR-NOTHING: every item is
 * decoded and validated before any change to the schedule, so a
 * failure leaves it untouched. The summary report ([11]) is a query —
 * a requested identifier that is not scheduled is simply omitted from
 * the [12] report, not an error.
 *
 * The detail report (ECSS 11,9/11,10) is deliberately excluded: it
 * echoes each activity's telecommand verbatim, so the packet size is
 * unbounded — that is "large data" and pairs with PUS-13. The summary
 * report carries only release time + request identifier per activity
 * and is bounded. Time-shift, sub-schedules, groups and filter-based
 * selection are also out of this slice (see CHANGELOG.md).
 *
 * As of slice fsw-5 generic TC reception lives in the TC router,
 * which validates and routes a TC then calls ``migris_pus11_execute``
 * with the parsed subtype, source ID and application data. PUS-11
 * owns its subtype handling and the [12] response; the schedule store
 * itself is ``lib/schedule/``.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. Byte-level
 * specification: docs/wire/pus-11.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS11_H_
#define MIGRIS_FSW_PUS_PUS11_H_

#include "migris/fsw/schedule/schedule.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_SCHEDULING 11U

#define MIGRIS_PUS11_SUBTYPE_ENABLE 1U                  /**< TC, enable the schedule. */
#define MIGRIS_PUS11_SUBTYPE_DISABLE 2U                 /**< TC, disable the schedule. */
#define MIGRIS_PUS11_SUBTYPE_RESET 3U                   /**< TC, delete all activities. */
#define MIGRIS_PUS11_SUBTYPE_INSERT 4U                  /**< TC, insert activities. */
#define MIGRIS_PUS11_SUBTYPE_DELETE 5U                  /**< TC, delete by request id. */
#define MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT_REQUEST 11U /**< TC, summary-report by request id. */
#define MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT 12U         /**< TM, the summary report. */

/** Upper bound on activities one TC may insert / delete / report, and
 *  one [12] report may carry. Compile-time constant — it bounds the
 *  worst-case packet (and so the TC router's output buffer). Override
 *  with ``-DMIGRIS_PUS11_MAX_PER_TC=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_PUS11_MAX_PER_TC``). */
#ifndef MIGRIS_PUS11_MAX_PER_TC
#    define MIGRIS_PUS11_MAX_PER_TC 8U
#endif

/** Worst-case wire size of a [12] summary report: primary (6) + TM
 *  sec header (10) + 1-byte count + N entries of (4-byte release time
 *  + 4-byte request id) + CRC (2). Equals 19 + 8*N. */
#define MIGRIS_PUS11_TM_MAX_PACKET_SIZE (6U + 10U + 1U + (MIGRIS_PUS11_MAX_PER_TC * 8U) + 2U)

/** PUS-11 handler return / error codes. Same convention as the rest
 *  of the codec: a positive value is the byte count written, 0 is
 *  success with no telemetry, a negative value is one of these. The
 *  TC router maps ``ERR_BAD_SUBTYPE`` to a PUS-1 UNKNOWN_SUBTYPE
 *  completion failure and every other negative code to EXEC_FAILURE. */
typedef enum {
    MIGRIS_PUS11_OK = 0,
    MIGRIS_PUS11_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < the report size. */
    MIGRIS_PUS11_ERR_BAD_ARG = -2,       /**< NULL ctx / schedule / app data / out. */
    MIGRIS_PUS11_ERR_MALFORMED = -3,     /**< Application-data length / count inconsistent. */
    MIGRIS_PUS11_ERR_TOO_MANY = -4,      /**< Count over MIGRIS_PUS11_MAX_PER_TC. */
    MIGRIS_PUS11_ERR_NOT_FOUND = -5,     /**< A delete named an unscheduled activity. */
    MIGRIS_PUS11_ERR_DUPLICATE = -6,     /**< An insert's request id is already scheduled. */
    MIGRIS_PUS11_ERR_FULL = -7,          /**< Insert would exceed the schedule capacity. */
    MIGRIS_PUS11_ERR_TC_TOO_LARGE = -8,  /**< An embedded TC exceeds the per-activity size. */
    MIGRIS_PUS11_ERR_BAD_SUBTYPE = -9    /**< Service subtype not supported inbound. */
} migris_pus11_status_t;

/** Per-(service, subtype) message counter for the PUS-11 report
 *  subtype this slice emits. Index [0] = summary report (12); the
 *  array reserves room for further report subtypes without an ABI
 *  change (same pattern as pus1.h / pus3.h / pus20.h). Caller-owned;
 *  zero-initialised once at startup. */
typedef struct {
    uint8_t msg_counter[1];
} migris_pus11_ctx_t;

/** Execute an already-accepted, already-routed PUS-11 TC.
 *
 *  The caller (TC router) has validated CCSDS framing, length, CRC and
 *  PUS-C version and confirmed service type 11. ``app_data`` /
 *  ``app_len`` is the TC application data (after the TC secondary
 *  header, before the CRC); ``sched`` is the on-board schedule store.
 *
 *   * [1]/[2]/[3] act on the schedule (no application data) and return
 *     ``MIGRIS_PUS11_OK`` with no telemetry.
 *   * [4] inserts and [5] deletes activities, all-or-nothing; return
 *     ``MIGRIS_PUS11_OK`` with no telemetry.
 *   * [11] encodes one [12] summary report into ``out`` and returns
 *     the positive report byte count.
 *
 *  Any failure leaves the schedule untouched, writes nothing to
 *  ``out``, and returns a negative ``migris_pus11_status_t``.
 *  ``tm_seq_count`` is the shared per-APID CCSDS TM sequence count:
 *  read into the [12] report then advanced mod 2^14, and only when a
 *  report is actually emitted. */
int migris_pus11_execute(migris_pus11_ctx_t* ctx,
                         migris_schedule_t* sched,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         const uint8_t* app_data,
                         size_t app_len,
                         uint8_t* out,
                         size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS11_H_
