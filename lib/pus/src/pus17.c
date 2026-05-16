/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-17 (Test) — connection test service.
 *
 * Leaf service: the TC router has already validated the CCSDS
 * primary header, packet length, CRC, and PUS-C version, and routed
 * by service type. This file only verifies the subtype and encodes
 * the PUS-17[2] response. Wire format pinned in docs/wire/pus-17.md.
 */

#include "migris/fsw/pus/pus17.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

int migris_pus17_execute(migris_pus17_ctx_t* ctx,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         uint8_t* tm,
                         size_t tm_cap) {
    if (tm_cap < MIGRIS_PUS17_TM_PACKET_SIZE) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }
    if (service_subtype != MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC) {
        return MIGRIS_PUS17_ERR_NOT_PUS17_TC;
    }

    const migris_ccsds_primary_header_t tm_primary = {
        .version = 0U,
        .type = MIGRIS_CCSDS_PACKET_TYPE_TM,
        .sec_hdr_flag = 1U,
        .apid = apid,
        .seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        .seq_count = *tm_seq_count,
        .data_length = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 2U - 1U),
    };
    if (migris_ccsds_primary_pack(&tm_primary, tm, tm_cap) != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t tm_sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_TEST,
        .service_subtype = MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM,
        .msg_counter = ctx->tm_msg_counter,
        .destination_id = tc_source_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&tm_sec,
                                     &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     tm_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS17_ERR_BUF_TOO_SMALL;
    }

    /* Compute and append CRC over everything we just wrote. */
    const size_t tm_crc_offset = MIGRIS_PUS17_TM_PACKET_SIZE - 2U;
    const uint16_t crc = migris_crc16_ccitt_false(tm, tm_crc_offset);
    tm[tm_crc_offset] = (uint8_t)(crc >> 8);
    tm[tm_crc_offset + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit state on success only: advance the shared per-APID
     * sequence count (mod 2^14) and this service's message counter
     * (mod 2^8). */
    *tm_seq_count = (uint16_t)((*tm_seq_count + 1U) & 0x3FFFU);
    ctx->tm_msg_counter = (uint8_t)(ctx->tm_msg_counter + 1U);

    return (int)MIGRIS_PUS17_TM_PACKET_SIZE;
}
