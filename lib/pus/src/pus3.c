/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-3 (Housekeeping & diagnostic data reporting) — report encoder.
 *
 * Pure serialiser: the caller decides a report is due (a periodic
 * cadence point, or a [27] one-shot poll) and supplies the structure
 * ID plus a snapshot of the parameter set; this file only turns that
 * into a wire-format [25] report. Source data is the 2-byte big-endian
 * Structure ID followed by the fixed parameter block defined in
 * docs/wire/pus-3.md.
 */

#include "migris/fsw/pus/pus3.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

static size_t put_u32_be(uint8_t* out, size_t off, uint32_t v) {
    out[off] = (uint8_t)((v >> 24) & 0xFFU);
    out[off + 1U] = (uint8_t)((v >> 16) & 0xFFU);
    out[off + 2U] = (uint8_t)((v >> 8) & 0xFFU);
    out[off + 3U] = (uint8_t)(v & 0xFFU);
    return off + 4U;
}

int migris_pus3_build_hk_report(migris_pus3_ctx_t* ctx,
                                uint16_t apid,
                                uint16_t* tm_seq_count,
                                uint32_t now_seconds,
                                migris_pus3_sid_t sid,
                                const migris_pus3_hk_params_t* params,
                                uint16_t destination_id,
                                uint8_t* out,
                                size_t out_cap) {
    if (ctx == NULL || tm_seq_count == NULL || params == NULL || out == NULL) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }
    /* The only structure this slice defines. Checked before any state
     * advances so an unknown SID is a clean no-op (the router maps it
     * to a PUS-1 completion failure). */
    if (sid != MIGRIS_PUS3_SID_FRAMEWORK_DIAG) {
        return MIGRIS_PUS3_ERR_UNKNOWN_SID;
    }

    /* data_length = (data field bytes) − 1. The data field is the TM
     * secondary header + source data (SID + param block) + 2-byte CRC. */
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE +
                                           MIGRIS_PUS3_HK_SOURCE_DATA_SIZE + 2U);
    const size_t packet_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + (size_t)data_field;

    if (out_cap < packet_size) {
        return MIGRIS_PUS3_ERR_BUF_TOO_SMALL;
    }

    /* Captured before the advance: this exact count is written into
     * both the CCSDS primary header and the parameter block, so the
     * report carries the sequence number it itself consumed. */
    const uint16_t seq_snapshot = *tm_seq_count;

    const migris_ccsds_primary_header_t primary = {
        .version = 0U,
        .type = MIGRIS_CCSDS_PACKET_TYPE_TM,
        .sec_hdr_flag = 1U,
        .apid = apid,
        .seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        .seq_count = seq_snapshot,
        .data_length = (uint16_t)(data_field - 1U),
    };
    if (migris_ccsds_primary_pack(&primary, out, out_cap) != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS3_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
        .service_subtype = MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT,
        .msg_counter = ctx->msg_counter[0],
        .destination_id = destination_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS3_ERR_BUF_TOO_SMALL;
    }

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

    /* Source data — frozen layout, all big-endian, no padding
     * (docs/wire/pus-3.md). */
    out[off++] = (uint8_t)((sid >> 8) & 0xFFU);
    out[off++] = (uint8_t)(sid & 0xFFU);
    off = put_u32_be(out, off, now_seconds);
    out[off++] = (uint8_t)((seq_snapshot >> 8) & 0xFFU);
    out[off++] = (uint8_t)(seq_snapshot & 0xFFU);
    for (size_t i = 0U; i < 4U; ++i) {
        out[off++] = params->pus1_msg_counter[i];
    }
    for (size_t i = 0U; i < 4U; ++i) {
        out[off++] = params->pus5_msg_counter[i];
    }
    out[off++] = params->pus17_tm_msg_counter;
    off = put_u32_be(out, off, params->tc_accepted_count);
    off = put_u32_be(out, off, params->tc_rejected_count);
    off = put_u32_be(out, off, params->rx_ring_overflow_drops);

    /* CRC-16-CCITT-FALSE over every byte before the trailing CRC. */
    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit state only once the whole packet is built: advance the
     * shared per-APID CCSDS sequence count (mod 2^14) and this
     * service's PUS message counter (mod 2^8). */
    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[0] = (uint8_t)(ctx->msg_counter[0] + 1U);

    return (int)packet_size;
}
