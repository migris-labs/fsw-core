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
#include "migris/fsw/pus/pus3.h"
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
    if (sec.service_type != MIGRIS_PUS_SERVICE_TEST &&
        sec.service_type != MIGRIS_PUS_SERVICE_HOUSEKEEPING) {
        out->fc = MIGRIS_PUS1_FC_UNKNOWN_SERVICE;
        return;
    }

    out->fc = MIGRIS_PUS1_FC_NONE; /* accepted */
}

/* Execute a routed PUS-3 TC. The only inbound subtype is [27]
 * "generate a one-shot housekeeping report", whose application data is
 * exactly one Structure ID. Writes the [25] report into `out` and
 * returns its byte count, or 0 with `*exec_fc` set to the PUS-1
 * completion-failure cause (bad subtype, malformed app data, or
 * unknown SID — all surface to ground as UNKNOWN_SUBTYPE since the
 * structure ID space is the addressable unit here).
 *
 * The PUS-5 message counters are reported as zero on this path: the
 * router does not own the PUS-5 context. Hoisting it in is the
 * deferred "FDIR raises events from inside the router" abstraction
 * (see pus5.h); the application's spontaneous report carries the live
 * PUS-5 counters. */
static int router_pus3_oneshot(migris_tc_router_ctx_t* ctx,
                               const migris_tc_accept_result_t* v,
                               uint32_t now_seconds,
                               const uint8_t* tc,
                               size_t tc_len,
                               uint8_t* out,
                               size_t out_cap,
                               migris_pus1_failure_code_t* exec_fc) {
    if (v->service_subtype != MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL) {
        *exec_fc = MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE;
        return 0;
    }

    /* accept() verified tc_len equals the declared total and is at
     * least MIGRIS_TC_ROUTER_MIN_TC (primary + TC sec + CRC), so
     * app_off + 2 <= tc_len and this subtraction cannot underflow. */
    const size_t app_off =
        MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE;
    const size_t app_len = tc_len - app_off - 2U;
    if (app_len != MIGRIS_PUS3_POLL_TC_APP_DATA_SIZE) {
        *exec_fc = MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE;
        return 0;
    }
    const migris_pus3_sid_t sid = (migris_pus3_sid_t)(((uint16_t)tc[app_off] << 8) |
                                                      (uint16_t)tc[app_off + 1U]);

    migris_pus3_hk_params_t p;
    for (size_t i = 0U; i < 4U; ++i) {
        p.pus1_msg_counter[i] = ctx->pus1.msg_counter[i];
        p.pus5_msg_counter[i] = 0U;
    }
    p.pus17_tm_msg_counter = ctx->pus17.tm_msg_counter;
    p.tc_accepted_count = ctx->tc_accepted_count;
    p.tc_rejected_count = ctx->tc_rejected_count;
    p.rx_ring_overflow_drops = ctx->rx_ring_overflow_drops;

    const int rc = migris_pus3_build_hk_report(&ctx->pus3,
                                               ctx->apid,
                                               &ctx->tm_seq_count,
                                               now_seconds,
                                               sid,
                                               &p,
                                               v->source_id,
                                               out,
                                               out_cap);
    if (rc > 0) {
        return rc;
    }
    if (rc == MIGRIS_PUS3_ERR_UNKNOWN_SID) {
        *exec_fc = MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE;
    } else {
        *exec_fc = MIGRIS_PUS1_FC_EXEC_FAILURE;
    }
    return 0;
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

    /* Count every addressed TC exactly once, by acceptance verdict.
     * `addressed == 0` already returned above, so noise / wrong-APID
     * packets never reach this. Both the length-error and the
     * other-failure paths below return, and the accepted path falls
     * through — each route is covered here exactly once. These two
     * counters are surfaced in the framework PUS-3 housekeeping
     * structure. */
    if (v.fc != MIGRIS_PUS1_FC_NONE) {
        ctx->tc_rejected_count++;
    } else {
        ctx->tc_accepted_count++;
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

    /* Route to the service handler by service type. migris_tc_accept
     * already rejected every type other than PUS-17 and PUS-3, so the
     * default arm is unreachable — it is kept only to make the switch
     * total. Subtype validity is an execution-stage concern: an
     * accepted TC with an unsupported subtype routes here and yields a
     * PUS-1[8] completion failure (UNKNOWN_SUBTYPE), never an
     * acceptance failure. */
    migris_pus1_failure_code_t exec_fc = MIGRIS_PUS1_FC_NONE;
    switch (v.service_type) {
    case MIGRIS_PUS_SERVICE_TEST: {
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
        break;
    }
    case MIGRIS_PUS_SERVICE_HOUSEKEEPING: {
        const int rc = router_pus3_oneshot(ctx,
                                           &v,
                                           now_seconds,
                                           tc,
                                           tc_len,
                                           &out[off],
                                           out_cap - off,
                                           &exec_fc);
        if (rc > 0) {
            off += (size_t)rc;
        }
        break;
    }
    default:
        exec_fc = MIGRIS_PUS1_FC_EXEC_FAILURE;
        break;
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
