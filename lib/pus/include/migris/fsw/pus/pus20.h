/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-20 — On-board parameter management. Slice fsw-9 ships the
 * complete standard service — three messages, the whole of
 * ECSS-E-ST-70-41C service 20:
 *
 *   * Subtype [1] (TC) → "Report parameter values". Application data
 *     is a 1-byte count followed by that many 2-byte parameter IDs.
 *   * Subtype [2] (TM) → "Parameter value report". Source data is a
 *     1-byte count followed by that many (2-byte ID, value) pairs;
 *     each value is encoded at the width its registered type dictates.
 *   * Subtype [3] (TC) → "Set parameter values". Application data is a
 *     1-byte count followed by that many (2-byte ID, value) pairs.
 *
 * There is no create / delete: the parameter set is fixed in the MIB
 * (the on-board datapool, lib/datapool/). PUS-20 is the ground-facing
 * face of that datapool; the parameter-definition-reporting and
 * vendor-extension subtypes are out of scope (see CHANGELOG.md).
 *
 * As of slice fsw-5 generic TC reception (CCSDS primary / length / CRC
 * / PUS-C version / APID checks) lives in the TC router, which
 * validates and routes a TC, then calls ``migris_pus20_execute`` with
 * the already-parsed subtype, source ID and application data. PUS-20
 * owns its subtype check, its application-data parsing, the datapool
 * access, and — for [1] — the [2] response.
 *
 * Set semantics are ALL-OR-NOTHING: a [20,3] is decoded and fully
 * validated (every ID exists, is read-write, and the value bytes
 * parse) before any datapool write occurs. Any failure leaves the
 * datapool untouched and the router maps it to a PUS-1[8] completion
 * failure. A [20,3] may name the same ID twice — that is permitted,
 * last-writer-wins (see docs/wire/pus-20.md).
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. Byte-level
 * specification: docs/wire/pus-20.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS20_H_
#define MIGRIS_FSW_PUS_PUS20_H_

#include "migris/fsw/datapool/datapool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER 20U

#define MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST 1U /**< TC, "report parameter values". */
#define MIGRIS_PUS20_SUBTYPE_VALUE_REPORT 2U   /**< TM, "parameter value report". */
#define MIGRIS_PUS20_SUBTYPE_SET_REQUEST 3U    /**< TC, "set parameter values". */

/** Upper bound on parameters one PUS-20 TC may address, and one [2]
 *  report may carry. Compile-time constant — it bounds the worst-case
 *  packet (and so the TC router's output buffer). Override with
 *  ``-DMIGRIS_PUS20_MAX_PARAMS_PER_TC=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_PUS20_MAX_PARAMS``). */
#ifndef MIGRIS_PUS20_MAX_PARAMS_PER_TC
#    define MIGRIS_PUS20_MAX_PARAMS_PER_TC 8U
#endif

/** Worst-case wire size of a [2] parameter value report: primary (6) +
 *  TM sec header (10) + 1-byte count + N pairs of (2-byte ID + 4-byte
 *  value, the widest type) + CRC (2). Equals 19 + 6*N. The TC router
 *  sizes its output buffer so this fits alongside the PUS-1 reports. */
#define MIGRIS_PUS20_TM_MAX_PACKET_SIZE \
    (6U + 10U + 1U + (MIGRIS_PUS20_MAX_PARAMS_PER_TC * 6U) + 2U)

/** PUS-20 handler return / error codes. Same convention as the rest of
 *  the codec: a positive value is the byte count written, 0 is success
 *  with no telemetry (a [20,3] set), a negative value is one of these.
 *  The TC router maps ``ERR_BAD_SUBTYPE`` to a PUS-1 UNKNOWN_SUBTYPE
 *  completion failure and every other negative code to EXEC_FAILURE. */
typedef enum {
    MIGRIS_PUS20_OK = 0,
    MIGRIS_PUS20_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < the report size. */
    MIGRIS_PUS20_ERR_BAD_ARG = -2,       /**< NULL ctx / datapool / app data / out. */
    MIGRIS_PUS20_ERR_MALFORMED = -3,     /**< Application-data length / count inconsistent. */
    MIGRIS_PUS20_ERR_TOO_MANY = -4,      /**< Count over MIGRIS_PUS20_MAX_PARAMS_PER_TC. */
    MIGRIS_PUS20_ERR_UNKNOWN_ID = -5,    /**< A referenced parameter ID is not in the pool. */
    MIGRIS_PUS20_ERR_READ_ONLY = -6,     /**< A [20,3] named a read-only parameter. */
    MIGRIS_PUS20_ERR_BAD_SUBTYPE = -7    /**< Service subtype is not [1] or [3]. */
} migris_pus20_status_t;

/** Per-(service, subtype) message counter for the PUS-20 report
 *  subtype this slice emits. Index [0] = parameter value report (2);
 *  the array reserves room for further report subtypes without an ABI
 *  change (same pattern as pus1.h / pus3.h / pus5.h). Caller-owned;
 *  zero-initialised once at startup. The CCSDS TM sequence count is
 *  *not* here — it is shared per-APID and lives in the TC router
 *  context. */
typedef struct {
    uint8_t msg_counter[1];
} migris_pus20_ctx_t;

/** Execute an already-accepted, already-routed PUS-20 TC.
 *
 *  The caller (TC router) has validated the CCSDS primary header,
 *  packet length, CRC and PUS-C version and confirmed the service
 *  type is 20. ``app_data`` / ``app_len`` is the TC application data
 *  (the bytes after the TC secondary header, before the CRC).
 *
 *   * Subtype [1]: decodes the requested parameter IDs, validates that
 *     every one is defined, and encodes one [20,2] parameter value
 *     report into ``out``. Returns the positive report byte count.
 *   * Subtype [3]: decodes the (ID, value) pairs, validates that every
 *     ID is defined and read-write and that the value bytes parse,
 *     then — only if all pass — applies every write to ``dp``. Emits
 *     no telemetry. Returns ``MIGRIS_PUS20_OK`` (0).
 *
 *  All-or-nothing: any validation failure leaves ``dp`` untouched,
 *  writes nothing to ``out``, and returns a negative
 *  ``migris_pus20_status_t``. ``tm_seq_count`` is the shared per-APID
 *  CCSDS TM sequence count: read into the [20,2] report then advanced
 *  mod 2^14, and only when a report is actually emitted. */
int migris_pus20_execute(migris_pus20_ctx_t* ctx,
                         migris_datapool_t* dp,
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

#endif  // MIGRIS_FSW_PUS_PUS20_H_
