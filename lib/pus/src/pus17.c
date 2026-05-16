/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-17 (Test) — connection test service.
 *
 * Decode a PUS-17[1] TC, validate every field that the wire format
 * pins, then encode a PUS-17[2] TM response.
 *
 * Strictness policy: this is on-board command-handling code. We
 * reject anything that doesn't match the pinned wire format byte for
 * byte. Lenient decoders accumulate technical debt in flight; strict
 * decoders surface ground-side bugs early.
 */

#include "migris/fsw/pus/pus17.h"

#include <stddef.h>
#include <stdint.h>

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

static int validate_primary(const migris_ccsds_primary_header_t *hdr, uint16_t expected_apid) {
    if (hdr->version != 0U) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }
    if (hdr->type != MIGRIS_CCSDS_PACKET_TYPE_TC) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }
    if (hdr->sec_hdr_flag != 1U) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }
    if (hdr->apid != expected_apid) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }
    if (hdr->seq_flags != MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }
    return MIGRIS_PUS17_OK;
}

int migris_pus17_handle_are_you_alive(migris_pus17_ctx_t *ctx,
                                      uint32_t now_seconds,
                                      const uint8_t *tc, size_t tc_len,
                                      uint8_t *tm, size_t tm_cap) {
    if (tm_cap < MIGRIS_PUS17_TM_PACKET_SIZE) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }

    /* TC length check: primary (6) + TC sec hdr (5) + zero user data
     * + CRC (2) = 13 bytes exactly. We reject any other length;
     * extra bytes would mean either a different subtype with a user
     * data field, or a malformed packet. */
    if (tc_len != MIGRIS_PUS17_TC_PACKET_SIZE) {
        return MIGRIS_PUS17_ERR_TRUNCATED;
    }

    migris_ccsds_primary_header_t primary;
    if (migris_ccsds_primary_unpack(&primary, tc, tc_len) != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS17_ERR_BAD_PRIMARY;
    }

    const int v = validate_primary(&primary, ctx->apid);
    if (v != MIGRIS_PUS17_OK) {
        return v;
    }

    /* data_length is "(data field bytes) − 1". For a PUS-17[1] TC the
     * data field is exactly 7 bytes (5-byte sec hdr + 2-byte CRC). */
    if (primary.data_length != (MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + 2U - 1U)) {
        return MIGRIS_PUS17_ERR_TRUNCATED;
    }

    /* CRC check covers every byte before the trailing 2-byte CRC. */
    const size_t crc_offset = tc_len - 2U;
    const uint16_t computed = migris_crc16_ccitt_false(tc, crc_offset);
    const uint16_t on_wire = (uint16_t)(((uint16_t)tc[crc_offset] << 8)
                                        | (uint16_t)tc[crc_offset + 1U]);
    if (computed != on_wire) {
        return MIGRIS_PUS17_ERR_BAD_CRC;
    }

    /* Secondary header sits right after the primary. */
    migris_pus_tc_secondary_header_t tc_sec;
    if (migris_pus_tc_secondary_unpack(&tc_sec,
                                       &tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                       tc_len - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE)
        != 0) {
        return MIGRIS_PUS17_ERR_TRUNCATED;
    }
    if (tc_sec.pus_version != MIGRIS_PUS_VERSION_C) {
        return MIGRIS_PUS17_ERR_BAD_PUS_VERSION;
    }
    if (tc_sec.service_type != MIGRIS_PUS_SERVICE_TEST
        || tc_sec.service_subtype != MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC) {
        return MIGRIS_PUS17_ERR_NOT_PUS17_TC;
    }

    /* TC accepted. Build the PUS-17[2] response. */
    const migris_ccsds_primary_header_t tm_primary = {
        .version = 0U,
        .type = MIGRIS_CCSDS_PACKET_TYPE_TM,
        .sec_hdr_flag = 1U,
        .apid = ctx->apid,
        .seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        .seq_count = ctx->tm_seq_count,
        .data_length = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 2U - 1U),
    };

    int rc = migris_ccsds_primary_pack(&tm_primary, tm, tm_cap);
    if (rc != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t tm_sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_TEST,
        .service_subtype = MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM,
        .msg_counter = ctx->tm_msg_counter,
        .destination_id = tc_sec.source_id,
        .time_seconds = now_seconds,
    };

    rc = migris_pus_tm_secondary_pack(&tm_sec,
                                      &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                      tm_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE);
    if (rc != 0) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }

    /* Compute and append CRC over everything we just wrote. */
    const size_t tm_crc_offset = MIGRIS_PUS17_TM_PACKET_SIZE - 2U;
    const uint16_t crc = migris_crc16_ccitt_false(tm, tm_crc_offset);
    tm[tm_crc_offset] = (uint8_t)(crc >> 8);
    tm[tm_crc_offset + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit ctx state (mod 2^14 for seq_count, mod 2^8 for msg_counter). */
    ctx->tm_seq_count = (uint16_t)((ctx->tm_seq_count + 1U) & 0x3FFFU);
    ctx->tm_msg_counter = (uint8_t)(ctx->tm_msg_counter + 1U);

    return (int)MIGRIS_PUS17_TM_PACKET_SIZE;
}
