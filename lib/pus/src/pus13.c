/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-13 (Large data transfer, downlink) — part-packet codec.
 *
 * Stateless: builds one [13,1] / [13,2] / [13,3] part packet per call.
 * The part subtype is derived from the part's position in the
 * transfer. Anything that does not match the pinned wire format byte
 * for byte is rejected. Wire format pinned in docs/wire/pus-13.md.
 */

#include "migris/fsw/pus/pus13.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

/* Derive the part subtype from the part's position in the transfer:
 * the final index is the last part, index 0 of a multi-part transfer
 * is the first part, anything else is intermediate. A single-part
 * transfer (total_parts == 1) is one last part. */
static uint8_t pus13_subtype_for(uint16_t part_number, uint16_t total_parts) {
    if (part_number == (uint16_t)(total_parts - 1U)) {
        return MIGRIS_PUS13_SUBTYPE_LAST_PART;
    }
    if (part_number == 0U) {
        return MIGRIS_PUS13_SUBTYPE_FIRST_PART;
    }
    return MIGRIS_PUS13_SUBTYPE_INTERMEDIATE_PART;
}

/* Write `value` big-endian into `out` at `off`; return the next offset. */
static size_t pus13_put_be16(uint8_t* out, size_t off, uint16_t value) {
    out[off] = (uint8_t)((value >> 8) & 0xFFU);
    out[off + 1U] = (uint8_t)(value & 0xFFU);
    return off + 2U;
}

int migris_pus13_build_part(migris_pus13_ctx_t* ctx,
                            uint16_t apid,
                            uint16_t* tm_seq_count,
                            uint32_t now_seconds,
                            uint16_t destination_id,
                            uint16_t transaction_id,
                            uint16_t part_number,
                            uint16_t total_parts,
                            const uint8_t* payload,
                            size_t payload_len,
                            uint8_t* out,
                            size_t out_cap) {
    if (ctx == NULL || tm_seq_count == NULL || payload == NULL || out == NULL) {
        return MIGRIS_PUS13_ERR_BAD_ARG;
    }
    if (total_parts == 0U || part_number >= total_parts) {
        return MIGRIS_PUS13_ERR_BAD_ARG;
    }
    if (payload_len == 0U || payload_len > MIGRIS_PUS13_PART_SIZE) {
        return MIGRIS_PUS13_ERR_BAD_ARG;
    }

    /* Source data is the 6-byte part header plus the part payload. */
    const size_t source_len = MIGRIS_PUS13_PART_HEADER_SIZE + payload_len;
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + source_len + 2U);
    const size_t packet_len = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + data_field;
    if (out_cap < packet_len) {
        return MIGRIS_PUS13_ERR_BUF_TOO_SMALL;
    }

    const uint8_t subtype = pus13_subtype_for(part_number, total_parts);
    const uint8_t counter_index = (uint8_t)(subtype - 1U);
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
        return MIGRIS_PUS13_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_LARGE_DATA,
        .service_subtype = subtype,
        .msg_counter = ctx->msg_counter[counter_index],
        .destination_id = destination_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS13_ERR_BUF_TOO_SMALL;
    }

    /* Part header, then the payload, into the source-data region. */
    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    off = pus13_put_be16(out, off, transaction_id);
    off = pus13_put_be16(out, off, part_number);
    off = pus13_put_be16(out, off, total_parts);
    for (size_t i = 0U; i < payload_len; ++i) {
        out[off + i] = payload[i];
    }
    off += payload_len;

    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);
    off += 2U;

    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[counter_index] = (uint8_t)(ctx->msg_counter[counter_index] + 1U);
    return (int)off;
}
