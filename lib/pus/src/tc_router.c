/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * TC reception, acceptance and routing.
 *
 * Strictness policy (same as the leaf services): this is on-board
 * command handling. Anything that does not match the pinned wire
 * format byte for byte is rejected. A TC that is not addressed to
 * this application process produces no output at all; a malformed or
 * unsupported TC that *is* addressed here produces a PUS-1 failure
 * report iff the TC requested verification. Wire format pinned in
 * docs/wire/pus-1.md.
 */

#include "migris/fsw/pus/tc_router.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus17.h"
#include "migris/fsw/pus/pus_tc.h"

#include <stddef.h>
#include <stdint.h>

/* Smallest TC we can structurally parse: CCSDS primary (6) + PUS-C TC
 * secondary header (5) + 2-byte CRC. Anything shorter is a length
 * error by construction. */
#define MIGRIS_TC_ROUTER_MIN_TC \
    (MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + 2U)

void migris_tc_accept(const uint8_t* tc,
                      size_t tc_len,
                      uint16_t expected_apid,
                      migris_tc_accept_result_t* out) {
    out->addressed = 0;
    out->fc = MIGRIS_PUS1_FC_NONE;
    out->ack_flags = 0U;
    out->source_id = 0U;
    out->service_type = 0U;
    out->service_subtype = 0U;

    migris_ccsds_primary_header_t primary;
    if (migris_ccsds_primary_unpack(&primary, tc, tc_len) != MIGRIS_CCSDS_OK) {
        return; /* not even a primary header — ignore */
    }
    if (primary.version != 0U || primary.type != MIGRIS_CCSDS_PACKET_TYPE_TC ||
        primary.sec_hdr_flag != 1U || primary.seq_flags != MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED) {
        return; /* malformed framing — ignore (could be noise or a TM) */
    }
    if (primary.apid != expected_apid) {
        return; /* not for this application process — ignore */
    }

    out->addressed = 1;

    /* Length consistency. A TC addressed to us whose declared length
     * disagrees with the bytes received cannot be parsed reliably —
     * the ack flags themselves are not trustworthy — so the router
     * reports this unconditionally. */
    const size_t expected_total = migris_ccsds_packet_total_size(primary.data_length);
    if (tc_len < MIGRIS_TC_ROUTER_MIN_TC || tc_len != expected_total) {
        out->fc = MIGRIS_PUS1_FC_LENGTH_ERROR;
        return;
    }

    migris_pus_tc_secondary_header_t sec;
    if (migris_pus_tc_secondary_unpack(&sec,
                                       &tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                       tc_len - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        out->fc = MIGRIS_PUS1_FC_LENGTH_ERROR;
        return;
    }

    /* Identity is now known. Capture it before the CRC check so a
     * requested verification still surfaces a CRC-failed command. */
    out->ack_flags = sec.ack_flags;
    out->source_id = sec.source_id;
    out->service_type = sec.service_type;
    out->service_subtype = sec.service_subtype;

    const size_t crc_offset = tc_len - 2U;
    const uint16_t computed = migris_crc16_ccitt_false(tc, crc_offset);
    const uint16_t on_wire =
        (uint16_t)(((uint16_t)tc[crc_offset] << 8) | (uint16_t)tc[crc_offset + 1U]);
    if (computed != on_wire) {
        out->fc = MIGRIS_PUS1_FC_CRC_FAILURE;
        return;
    }
    if (sec.pus_version != MIGRIS_PUS_VERSION_C) {
        out->fc = MIGRIS_PUS1_FC_BAD_PUS_VERSION;
        return;
    }
    if (sec.service_type != MIGRIS_PUS_SERVICE_TEST) {
        out->fc = MIGRIS_PUS1_FC_UNKNOWN_SERVICE;
        return;
    }

    out->fc = MIGRIS_PUS1_FC_NONE; /* accepted */
}

int migris_tc_router_dispatch(migris_tc_router_ctx_t* ctx,
                              uint32_t now_seconds,
                              const uint8_t* tc,
                              size_t tc_len,
                              uint8_t* out,
                              size_t out_cap) {
    /* One up-front capacity check: with a worst-case-sized buffer no
     * individual report can run out of space, so each builder below
     * is guaranteed to fit. */
    if (out_cap < MIGRIS_TC_ROUTER_MAX_TM) {
        return MIGRIS_TC_ROUTER_ERR_BUF_TOO_SMALL;
    }

    migris_tc_accept_result_t v;
    migris_tc_accept(tc, tc_len, ctx->apid, &v);
    if (v.addressed == 0) {
        return 0; /* not for this AP — silent, per the strictness policy */
    }

    /* request_id = TC primary header bytes [0..3]. `addressed` implies
     * the primary header unpacked, so those bytes are present. */
    uint8_t request_id[MIGRIS_PUS1_REQUEST_ID_SIZE];
    for (size_t i = 0U; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        request_id[i] = tc[i];
    }

    size_t off = 0U;

    /* Length error: ack flags are not trustworthy, so a malformed
     * command addressed to this AP is always reported. No routing, no
     * completion. */
    if (v.fc == MIGRIS_PUS1_FC_LENGTH_ERROR) {
        const int n = migris_pus1_build_acceptance(&ctx->pus1,
                                                   ctx->apid,
                                                   &ctx->tm_seq_count,
                                                   now_seconds,
                                                   request_id,
                                                   0U,
                                                   MIGRIS_PUS1_FC_LENGTH_ERROR,
                                                   &out[off],
                                                   out_cap - off);
        if (n > 0) {
            off += (size_t)n;
        }
        return (int)off;
    }

    /* Other acceptance-stage failure (CRC / PUS version / unknown
     * service): report only if requested, then stop. */
    if (v.fc != MIGRIS_PUS1_FC_NONE) {
        if ((v.ack_flags & MIGRIS_PUS_TC_ACK_ACCEPTANCE) != 0U) {
            const int n = migris_pus1_build_acceptance(&ctx->pus1,
                                                       ctx->apid,
                                                       &ctx->tm_seq_count,
                                                       now_seconds,
                                                       request_id,
                                                       v.source_id,
                                                       v.fc,
                                                       &out[off],
                                                       out_cap - off);
            if (n > 0) {
                off += (size_t)n;
            }
        }
        return (int)off;
    }

    /* Accepted. */
    if ((v.ack_flags & MIGRIS_PUS_TC_ACK_ACCEPTANCE) != 0U) {
        const int n = migris_pus1_build_acceptance(&ctx->pus1,
                                                   ctx->apid,
                                                   &ctx->tm_seq_count,
                                                   now_seconds,
                                                   request_id,
                                                   v.source_id,
                                                   MIGRIS_PUS1_FC_NONE,
                                                   &out[off],
                                                   out_cap - off);
        if (n > 0) {
            off += (size_t)n;
        }
    }

    /* Route to the service handler. migris_tc_accept already rejected
     * any service type other than PUS-17, so this is the only arm. */
    migris_pus1_failure_code_t exec_fc = MIGRIS_PUS1_FC_NONE;
    const int rc = migris_pus17_execute(&ctx->pus17,
                                        ctx->apid,
                                        &ctx->tm_seq_count,
                                        now_seconds,
                                        v.service_subtype,
                                        v.source_id,
                                        &out[off],
                                        out_cap - off);
    if (rc > 0) {
        off += (size_t)rc;
    } else if (rc == MIGRIS_PUS17_ERR_NOT_PUS17_TC) {
        exec_fc = MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE;
    } else {
        exec_fc = MIGRIS_PUS1_FC_EXEC_FAILURE;
    }

    if ((v.ack_flags & MIGRIS_PUS_TC_ACK_COMPLETION) != 0U) {
        const int n = migris_pus1_build_completion(&ctx->pus1,
                                                   ctx->apid,
                                                   &ctx->tm_seq_count,
                                                   now_seconds,
                                                   request_id,
                                                   v.source_id,
                                                   exec_fc,
                                                   &out[off],
                                                   out_cap - off);
        if (n > 0) {
            off += (size_t)n;
        }
    }

    return (int)off;
}
