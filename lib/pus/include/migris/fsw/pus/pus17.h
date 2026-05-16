/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-17 — Test service. Implements one subtype today:
 *
 *   * Subtype [1] (TC) → "Perform an are-you-alive connection test".
 *   * Subtype [2] (TM) → "Are-you-alive connection test report".
 *
 * The other subtypes ([3] application-specific connection test and
 * [4] its report) are not in the Migris PUS baseline yet — see
 * workspace CLAUDE.md.
 *
 * As of slice fsw-5 this is a *leaf service*: generic TC reception
 * (CCSDS primary / length / CRC / PUS-C version / APID checks) lives
 * in the TC router, which validates and routes a TC, then calls
 * ``migris_pus17_execute`` with the already-parsed subtype and source
 * ID. PUS-17 only owns its own subtype check and its [2] response.
 *
 * Freestanding C: caller provides the output buffer, the current
 * time, the shared per-APID TM sequence count, and a caller-owned
 * context holding this service's PUS message counter.
 */

#ifndef MIGRIS_FSW_PUS_PUS17_H_
#define MIGRIS_FSW_PUS_PUS17_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_TEST 17U
#define MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC 1U
#define MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM 2U

/** Total wire size of a PUS-17[2] response packet:
 *  primary header (6) + TM sec header (10) + zero user data + CRC (2). */
#define MIGRIS_PUS17_TM_PACKET_SIZE 18U

/** Total wire size of a PUS-17[1] request packet:
 *  primary header (6) + TC sec header (5) + zero user data + CRC (2). */
#define MIGRIS_PUS17_TC_PACKET_SIZE 13U

/** State carried across PUS-17 invocations on the FSW side. The
 *  caller (the TC router, ultimately the sample app / mission FSW)
 *  owns the storage and zero-initialises it once at startup. The
 *  CCSDS TM sequence count is *not* here — it is shared per-APID
 *  across every service and lives in the TC router context. */
typedef struct {
    uint8_t tm_msg_counter; /**< Next PUS-17[2] Message Counter (mod 2^8). */
} migris_pus17_ctx_t;

/** PUS-17 handler return / error codes. */
typedef enum {
    MIGRIS_PUS17_OK = 0,
    MIGRIS_PUS17_ERR_NOT_PUS17_TC = -4, /**< Subtype is not (17, 1). */
    MIGRIS_PUS17_ERR_BUF_TOO_SMALL = -6 /**< Output buffer < MIGRIS_PUS17_TM_PACKET_SIZE. */
} migris_pus17_status_t;

/** Execute an already-accepted, already-routed PUS-17 TC.
 *
 *  The caller (TC router) has validated the CCSDS primary header,
 *  packet length, CRC, and PUS-C version, and has confirmed the
 *  service type is 17. It passes the parsed ``service_subtype`` and
 *  the TC's ``tc_source_id``. This function checks the subtype is the
 *  are-you-alive request (1) and, if so, builds the PUS-17[2]
 *  response into ``tm`` for application process ``apid``.
 *
 *  ``tm_seq_count`` is the shared per-APID CCSDS TM sequence count:
 *  read into the response, then advanced mod 2^14.
 *
 *  Returns ``MIGRIS_PUS17_TM_PACKET_SIZE`` (positive — bytes written)
 *  on success. Returns ``MIGRIS_PUS17_ERR_NOT_PUS17_TC`` for an
 *  unsupported subtype and ``MIGRIS_PUS17_ERR_BUF_TOO_SMALL`` for an
 *  undersized buffer; on either, ``ctx`` and ``*tm_seq_count`` are
 *  left unchanged. */
int migris_pus17_execute(migris_pus17_ctx_t* ctx,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         uint8_t* tm,
                         size_t tm_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS17_H_
