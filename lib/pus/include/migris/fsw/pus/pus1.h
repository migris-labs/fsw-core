/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-1 — Telecommand verification service. Slice fsw-5 ships the
 * acceptance and completion stages:
 *
 *   * Subtype [1] (TM) → "Successful acceptance verification report".
 *   * Subtype [2] (TM) → "Failed acceptance verification report".
 *   * Subtype [7] (TM) → "Successful completion of execution report".
 *   * Subtype [8] (TM) → "Failed completion of execution report".
 *
 * The start ([3]/[4]) and progress ([5]/[6]) stages are deliberately
 * not in this slice: the only command the framework executes today
 * (PUS-17 are-you-alive) is instantaneous, so start/progress reports
 * would be degenerate. They land when a long-running command exists
 * to exercise them — see workspace CLAUDE.md and CHANGELOG.md.
 *
 * Unlike PUS-17, PUS-1 has no inbound TC. A verification report is
 * emitted as a side effect of processing *another* service's TC,
 * gated by that TC's ack-flag bits (see pus_tc.h). This header is a
 * pure report *encoder*: the caller (the TC router) owns the
 * acceptance/execution decision and the request identity; here we
 * only serialise the report. Freestanding C — no Zephyr, no malloc,
 * no stdlib.
 *
 * Byte-level specification: docs/wire/pus-1.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS1_H_
#define MIGRIS_FSW_PUS_PUS1_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_VERIFICATION 1U

#define MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS 1U
#define MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE 2U
#define MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS 7U
#define MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE 8U

/** Request ID = the first 4 bytes of the verified TC (the CCSDS
 *  Packet ID + Packet Sequence Control, i.e. the TC primary header
 *  bytes [0..3]). This is exactly the TC identifier ECSS-E-ST-70-41C
 *  §8.1 puts in a verification report's source data. */
#define MIGRIS_PUS1_REQUEST_ID_SIZE 4U

/** Total wire size of a success report ([1]/[7]): primary (6) + TM
 *  sec header (10) + request ID (4) + CRC (2). */
#define MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE 22U

/** Total wire size of a failure report ([2]/[8]): the success layout
 *  plus a 1-byte failure code before the CRC. */
#define MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE 23U

/** Why a TC failed verification. Serialised as a single byte in the
 *  source data of a failure report ([2]/[8]); ``NONE`` is never put
 *  on the wire (a success report carries no failure code). The split
 *  between acceptance- and execution-stage causes is documented in
 *  docs/wire/pus-1.md. */
typedef enum {
    MIGRIS_PUS1_FC_NONE = 0,            /**< Verification passed — success report. */
    MIGRIS_PUS1_FC_BAD_PRIMARY = 1,     /**< CCSDS primary header malformed/inconsistent. */
    MIGRIS_PUS1_FC_ILLEGAL_APID = 2,    /**< APID is not this application process. */
    MIGRIS_PUS1_FC_LENGTH_ERROR = 3,    /**< Declared length / packet size mismatch. */
    MIGRIS_PUS1_FC_CRC_FAILURE = 4,     /**< Packet error-control CRC mismatch. */
    MIGRIS_PUS1_FC_BAD_PUS_VERSION = 5, /**< TC secondary header PUS version not C. */
    MIGRIS_PUS1_FC_UNKNOWN_SERVICE = 6, /**< Service type not routable on this AP. */
    MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE = 7, /**< Known service, unsupported subtype. */
    MIGRIS_PUS1_FC_EXEC_FAILURE = 8     /**< Routed handler ran but reported failure. */
} migris_pus1_failure_code_t;

/** PUS-1 encoder return / error codes. Same convention as the rest of
 *  the codec: a positive value is the byte count written, a negative
 *  value is one of these. */
typedef enum {
    MIGRIS_PUS1_OK = 0,
    MIGRIS_PUS1_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < required packet size. */
    MIGRIS_PUS1_ERR_BAD_ARG = -2        /**< NULL request ID or internal field range. */
} migris_pus1_status_t;

/** Per-(service, subtype) message counters for the four PUS-1 report
 *  subtypes this slice emits. Index order: [0]=acceptance success (1),
 *  [1]=acceptance failure (2), [2]=completion success (7),
 *  [3]=completion failure (8). Caller-owned; zero-initialised once at
 *  startup. The CCSDS TM sequence count is *not* here — it is shared
 *  per-APID across every service and lives in the TC router context. */
typedef struct {
    uint8_t msg_counter[4];
} migris_pus1_ctx_t;

/** Encode a PUS-1 *acceptance* verification report.
 *
 *  ``fc == MIGRIS_PUS1_FC_NONE`` builds a successful-acceptance
 *  report (subtype [1], no failure-code byte); any other ``fc``
 *  builds a failed-acceptance report (subtype [2], failure code
 *  appended). ``request_id`` must point to ``MIGRIS_PUS1_REQUEST_ID_SIZE``
 *  bytes (the verified TC's primary-header bytes [0..3]).
 *  ``destination_id`` echoes the verified TC's source ID, consistent
 *  with PUS-17. ``tm_seq_count`` is the shared per-APID CCSDS TM
 *  sequence count: read into the packet, then advanced mod 2^14.
 *
 *  Returns the positive byte count written
 *  (``MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE`` or
 *  ``MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE``) on success, or a negative
 *  ``migris_pus1_status_t``. Side effects (the shared sequence count
 *  and the relevant ``ctx`` message counter) advance on success only. */
int migris_pus1_build_acceptance(migris_pus1_ctx_t* ctx,
                                 uint16_t apid,
                                 uint16_t* tm_seq_count,
                                 uint32_t now_seconds,
                                 const uint8_t* request_id,
                                 uint16_t destination_id,
                                 migris_pus1_failure_code_t fc,
                                 uint8_t* out,
                                 size_t out_cap);

/** Encode a PUS-1 *completion* verification report. Same contract as
 *  ``migris_pus1_build_acceptance`` but emits subtype [7] (success)
 *  or [8] (failure). */
int migris_pus1_build_completion(migris_pus1_ctx_t* ctx,
                                 uint16_t apid,
                                 uint16_t* tm_seq_count,
                                 uint32_t now_seconds,
                                 const uint8_t* request_id,
                                 uint16_t destination_id,
                                 migris_pus1_failure_code_t fc,
                                 uint8_t* out,
                                 size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS1_H_
