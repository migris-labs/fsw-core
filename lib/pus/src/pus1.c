/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-1 (Telecommand verification) — report encoder.
 *
 * Pure serialiser: the TC router decides acceptance / completion and
 * supplies the verified TC's request identity; this file only turns
 * that decision into a wire-format report. A success report carries
 * just the 4-byte request ID; a failure report appends a single
 * failure-code byte. Wire format pinned in docs/wire/pus-1.md.
 */

#include "migris/fsw/pus/pus1.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

/* Build a PUS-1 verification report into `out`. `subtype_success` /
 * `subtype_failure` and `msg_idx_success` / `msg_idx_failure` select
 * the acceptance ([1]/[2], counters 0/1) or completion ([7]/[8],
 * counters 2/3) flavour; everything else is identical between the
 * two stages, so the two public entry points are thin wrappers. */
static int build_report(migris_pus1_ctx_t* ctx,
                        uint16_t apid,
                        uint16_t* tm_seq_count,
                        uint32_t now_seconds,
                        const uint8_t* request_id,
                        uint16_t destination_id,
                        migris_pus1_failure_code_t fc,
                        uint8_t subtype_success,
                        uint8_t subtype_failure,
                        size_t msg_idx_success,
                        size_t msg_idx_failure,
                        uint8_t* out,
                        size_t out_cap) {
    if (request_id == NULL) {
        return MIGRIS_PUS1_ERR_BAD_ARG;
    }

    const int is_failure = (fc != MIGRIS_PUS1_FC_NONE) ? 1 : 0;
    const uint8_t subtype = is_failure ? subtype_failure : subtype_success;
    const size_t msg_idx = is_failure ? msg_idx_failure : msg_idx_success;
    const size_t packet_size = is_failure ? MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE
                                          : MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE;

    if (out_cap < packet_size) {
        return MIGRIS_PUS1_ERR_BUF_TOO_SMALL;
    }

    /* data_length = (data field bytes) − 1. The data field is the TM
     * secondary header + request ID + (failure only) the failure-code
     * byte + the 2-byte CRC. */
    const uint16_t data_field =
        (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + MIGRIS_PUS1_REQUEST_ID_SIZE +
                   (is_failure ? 1U : 0U) + 2U);

    const migris_ccsds_primary_header_t primary = {
        .version = 0U,
        .type = MIGRIS_CCSDS_PACKET_TYPE_TM,
        .sec_hdr_flag = 1U,
        .apid = apid,
        .seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        .seq_count = *tm_seq_count,
        .data_length = (uint16_t)(data_field - 1U),
    };
    if (migris_ccsds_primary_pack(&primary, out, out_cap) != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS1_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_VERIFICATION,
        .service_subtype = subtype,
        .msg_counter = ctx->msg_counter[msg_idx],
        .destination_id = destination_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS1_ERR_BUF_TOO_SMALL;
    }

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    for (size_t i = 0U; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        out[off++] = request_id[i];
    }
    if (is_failure) {
        out[off++] = (uint8_t)fc;
    }

    /* CRC-16-CCITT-FALSE over every byte before the trailing CRC. */
    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit state only once the whole packet is built: advance the
     * shared per-APID CCSDS sequence count (mod 2^14) and this
     * subtype's PUS message counter (mod 2^8). */
    *tm_seq_count = (uint16_t)((*tm_seq_count + 1U) & 0x3FFFU);
    ctx->msg_counter[msg_idx] = (uint8_t)(ctx->msg_counter[msg_idx] + 1U);

    return (int)packet_size;
}

int migris_pus1_build_acceptance(migris_pus1_ctx_t* ctx,
                                 uint16_t apid,
                                 uint16_t* tm_seq_count,
                                 uint32_t now_seconds,
                                 const uint8_t* request_id,
                                 uint16_t destination_id,
                                 migris_pus1_failure_code_t fc,
                                 uint8_t* out,
                                 size_t out_cap) {
    return build_report(ctx,
                         apid,
                         tm_seq_count,
                         now_seconds,
                         request_id,
                         destination_id,
                         fc,
                         MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS,
                         MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE,
                         0U,
                         1U,
                         out,
                         out_cap);
}

int migris_pus1_build_completion(migris_pus1_ctx_t* ctx,
                                 uint16_t apid,
                                 uint16_t* tm_seq_count,
                                 uint32_t now_seconds,
                                 const uint8_t* request_id,
                                 uint16_t destination_id,
                                 migris_pus1_failure_code_t fc,
                                 uint8_t* out,
                                 size_t out_cap) {
    return build_report(ctx,
                         apid,
                         tm_seq_count,
                         now_seconds,
                         request_id,
                         destination_id,
                         fc,
                         MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS,
                         MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE,
                         2U,
                         3U,
                         out,
                         out_cap);
}
