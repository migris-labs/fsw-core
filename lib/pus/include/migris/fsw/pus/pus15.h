/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-15 — On-board storage and retrieval. Slice fsw-11 ships a
 * pragmatic core subset of ECSS-E-ST-70-41C service 15, over one
 * predefined packet store:
 *
 *   * Subtype [1]  (TC) → enable storage in the packet store.
 *   * Subtype [2]  (TC) → disable storage in the packet store.
 *   * Subtype [9]  (TC) → start a by-time-period retrieval (downlink).
 *   * Subtype [11] (TC) → delete the packet store content up to a time.
 *   * Subtype [12] (TC) → report the packet store.
 *   * Subtype [13] (TM) → packet store report.
 *
 * The packet store (lib/pktstore/) captures every TM packet the FSW
 * emits, each tagged with its storage time, so a pass's telemetry can
 * be downlinked on the next contact. A [15,9] downlink does not emit
 * telemetry itself — it ARMS a retrieval over a [from, to] time
 * window; the application's main loop then drains it, re-emitting one
 * stored packet per iteration. fsw-12's PUS-13 will add a chunked
 * large-data-transfer path for the same retrieval.
 *
 * As of slice fsw-5 generic TC reception lives in the TC router,
 * which validates and routes a TC then calls ``migris_pus15_execute``
 * with the parsed subtype, source ID and application data. PUS-15
 * owns its subtype handling and the [13] response; the packet store
 * itself is ``lib/pktstore/``.
 *
 * Dynamic packet-store creation / deletion, storage-selection
 * management, and the catalogue report are out of this slice — one
 * predefined store covers the produce-on-orbit / downlink-next-pass
 * model (see CHANGELOG.md).
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. Byte-level
 * specification: docs/wire/pus-15.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS15_H_
#define MIGRIS_FSW_PUS_PUS15_H_

#include "migris/fsw/pktstore/pktstore.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_STORAGE 15U

#define MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE 1U  /**< TC, enable storage. */
#define MIGRIS_PUS15_SUBTYPE_DISABLE_STORAGE 2U /**< TC, disable storage. */
#define MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE 9U  /**< TC, start a by-time-period retrieval. */
#define MIGRIS_PUS15_SUBTYPE_DELETE_RANGE 11U   /**< TC, delete content up to a time. */
#define MIGRIS_PUS15_SUBTYPE_REPORT_REQUEST 12U /**< TC, report the packet store. */
#define MIGRIS_PUS15_SUBTYPE_STORE_REPORT 13U   /**< TM, the packet store report. */

/** Total wire size of a [15,13] packet store report: primary (6) + TM
 *  sec header (10) + source data (1-byte enabled flag + 2-byte packet
 *  count + 4-byte oldest time + 4-byte newest time = 11) + CRC (2). */
#define MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE 29U

/** PUS-15 handler return / error codes. Same convention as the rest
 *  of the codec: a positive value is the byte count written, 0 is
 *  success with no telemetry, a negative value is one of these. The
 *  TC router maps ``ERR_BAD_SUBTYPE`` to a PUS-1 UNKNOWN_SUBTYPE
 *  completion failure and every other negative code to EXEC_FAILURE. */
typedef enum {
    MIGRIS_PUS15_OK = 0,
    MIGRIS_PUS15_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < the report size. */
    MIGRIS_PUS15_ERR_BAD_ARG = -2,       /**< NULL ctx / store / app data / out. */
    MIGRIS_PUS15_ERR_MALFORMED = -3,     /**< Application-data length wrong, or window inverted. */
    MIGRIS_PUS15_ERR_RETRIEVAL_ACTIVE =
        -4,                           /**< Downlink / delete while a retrieval is in progress. */
    MIGRIS_PUS15_ERR_BAD_SUBTYPE = -5 /**< Service subtype not supported inbound. */
} migris_pus15_status_t;

/** Per-(service, subtype) message counter for the PUS-15 report
 *  subtype this slice emits. Index [0] = packet store report (13).
 *  Caller-owned; zero-initialised once at startup. */
typedef struct {
    uint8_t msg_counter[1];
} migris_pus15_ctx_t;

/** Execute an already-accepted, already-routed PUS-15 TC.
 *
 *  The caller (TC router) has validated CCSDS framing, length, CRC and
 *  PUS-C version and confirmed service type 15. ``app_data`` /
 *  ``app_len`` is the TC application data; ``store`` is the on-board
 *  packet store.
 *
 *   * [1]/[2] toggle storage; [11] deletes content up to a time; [9]
 *     arms a by-time-period retrieval (drained by the main loop) —
 *     all return ``MIGRIS_PUS15_OK`` with no telemetry.
 *   * [12] encodes one [13] packet store report into ``out`` and
 *     returns the positive report byte count.
 *
 *  Any failure leaves the store untouched, writes nothing to ``out``,
 *  and returns a negative ``migris_pus15_status_t``. ``tm_seq_count``
 *  is the shared per-APID CCSDS TM sequence count: read into the [13]
 *  report then advanced mod 2^14, and only when a report is emitted. */
int migris_pus15_execute(migris_pus15_ctx_t* ctx,
                         migris_pktstore_t* store,
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

#endif  // MIGRIS_FSW_PUS_PUS15_H_
