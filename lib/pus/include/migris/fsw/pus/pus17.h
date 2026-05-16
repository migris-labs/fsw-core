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
 * The handler is freestanding: caller provides input/output buffers
 * and the current time (so on-board code can pass the boot clock and
 * host tests can pass a fixed value). State is held in a
 * caller-owned context struct.
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
 *  caller (sample app, downstream mission FSW) owns the storage and
 *  zero-initialises it once at startup. */
typedef struct {
    uint16_t apid;          /**< APID this AP receives on and emits with. */
    uint16_t tm_seq_count;  /**< Next CCSDS Sequence Count for TM (mod 2^14). */
    uint8_t tm_msg_counter; /**< Next PUS-17[2] Message Counter (mod 2^8). */
} migris_pus17_ctx_t;

/** PUS-17 handler return / error codes. */
typedef enum {
    MIGRIS_PUS17_OK = 0,
    MIGRIS_PUS17_ERR_TRUNCATED = -1,         /**< TC shorter than declared. */
    MIGRIS_PUS17_ERR_BAD_PRIMARY = -2,       /**< Type, APID, or sec-hdr flag mismatch. */
    MIGRIS_PUS17_ERR_BAD_CRC = -3,           /**< CRC verification failed on the TC. */
    MIGRIS_PUS17_ERR_NOT_PUS17_TC = -4,      /**< Service/subtype is not (17, 1). */
    MIGRIS_PUS17_ERR_BAD_PUS_VERSION = -5,   /**< TC secondary header PUS version not C. */
    MIGRIS_PUS17_ERR_BUF_TOO_SMALL = -6      /**< Output buffer < MIGRIS_PUS17_TM_PACKET_SIZE. */
} migris_pus17_status_t;

/** Decode a PUS-17[1] TC from ``tc`` (``tc_len`` bytes), validate its
 *  primary header / secondary header / CRC, then build the matching
 *  PUS-17[2] TM into ``tm`` (``tm_cap`` bytes capacity).
 *
 *  On success returns ``MIGRIS_PUS17_TM_PACKET_SIZE`` (positive value
 *  — number of bytes written). On failure returns a negative
 *  ``migris_pus17_status_t``.
 *
 *  Side effects: bumps ``ctx->tm_seq_count`` and ``ctx->tm_msg_counter``
 *  on success only. A failed TC validation leaves the context state
 *  unchanged so the next valid TC produces consecutively-numbered TM. */
int migris_pus17_handle_are_you_alive(migris_pus17_ctx_t *ctx,
                                      uint32_t now_seconds,
                                      const uint8_t *tc, size_t tc_len,
                                      uint8_t *tm, size_t tm_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS17_H_
