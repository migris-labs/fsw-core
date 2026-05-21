/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-15 (On-board storage and retrieval) — service handler.
 *
 * Strictness policy (same as the leaf services): on-board command
 * handling. Anything that does not match the pinned wire format byte
 * for byte is rejected. A [15,9] downlink does not emit telemetry — it
 * arms a retrieval the application's main loop drains. Wire format
 * pinned in docs/wire/pus-15.md.
 */

#include "migris/fsw/pus/pus15.h"

#include "migris/fsw/pktstore/pktstore.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

/* Read a 4-byte big-endian unsigned integer from `buf` at `off`. */
static uint32_t pus15_be32(const uint8_t* buf, size_t off) {
    return ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1U] << 16) |
           ((uint32_t)buf[off + 2U] << 8) | (uint32_t)buf[off + 3U];
}

/* Write `value` big-endian into `out` at `off`; return the next offset. */
static size_t pus15_put_be32(uint8_t* out, size_t off, uint32_t value) {
    out[off] = (uint8_t)((value >> 24) & 0xFFU);
    out[off + 1U] = (uint8_t)((value >> 16) & 0xFFU);
    out[off + 2U] = (uint8_t)((value >> 8) & 0xFFU);
    out[off + 3U] = (uint8_t)(value & 0xFFU);
    return off + 4U;
}

/* Build one [15,13] packet store report: storage enabled flag, packet
 * count, and the oldest / newest storage time held. */
static int pus15_build_store_report(migris_pus15_ctx_t* ctx,
                                    const migris_pktstore_t* store,
                                    uint16_t apid,
                                    uint16_t* tm_seq_count,
                                    uint32_t now_seconds,
                                    uint16_t tc_source_id,
                                    uint8_t* out,
                                    size_t out_cap) {
    if (out_cap < MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE) {
        return MIGRIS_PUS15_ERR_BUF_TOO_SMALL;
    }

    /* Source data is the fixed 11-byte summary block. */
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 11U + 2U);
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
        return MIGRIS_PUS15_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_STORAGE,
        .service_subtype = MIGRIS_PUS15_SUBTYPE_STORE_REPORT,
        .msg_counter = ctx->msg_counter[0],
        .destination_id = tc_source_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS15_ERR_BUF_TOO_SMALL;
    }

    uint32_t oldest = 0U;
    uint32_t newest = 0U;
    (void)migris_pktstore_span(store, &oldest, &newest); /* leaves 0,0 when empty */
    const size_t packet_count = migris_pktstore_count(store);

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    out[off] = (uint8_t)((migris_pktstore_is_enabled(store) != 0) ? 1U : 0U);
    off += 1U;
    out[off] = (uint8_t)((packet_count >> 8) & 0xFFU);
    out[off + 1U] = (uint8_t)(packet_count & 0xFFU);
    off += 2U;
    off = pus15_put_be32(out, off, oldest);
    off = pus15_put_be32(out, off, newest);

    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);
    off += 2U;

    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[0] = (uint8_t)(ctx->msg_counter[0] + 1U);
    return (int)off;
}

/* Arm a [15,9] by-time-period retrieval: application data is the
 * 4-byte from-time and 4-byte to-time. The retrieval emits no
 * telemetry here — the main loop drains it. */
static int pus15_downlink(migris_pktstore_t* store, const uint8_t* app, size_t app_len) {
    if (app_len != 8U) {
        return MIGRIS_PUS15_ERR_MALFORMED;
    }
    const int rc = migris_pktstore_arm_retrieval(store, pus15_be32(app, 0U), pus15_be32(app, 4U));
    if (rc == MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE) {
        return MIGRIS_PUS15_ERR_RETRIEVAL_ACTIVE;
    }
    if (rc != MIGRIS_PKTSTORE_OK) {
        return MIGRIS_PUS15_ERR_MALFORMED; /* inverted window */
    }
    return MIGRIS_PUS15_OK;
}

/* Execute a [15,11] delete: application data is the 4-byte time up to
 * and including which stored packets are removed. */
static int pus15_delete(migris_pktstore_t* store, const uint8_t* app, size_t app_len) {
    if (app_len != 4U) {
        return MIGRIS_PUS15_ERR_MALFORMED;
    }
    const int rc = migris_pktstore_delete_up_to(store, pus15_be32(app, 0U));
    if (rc == MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE) {
        return MIGRIS_PUS15_ERR_RETRIEVAL_ACTIVE;
    }
    if (rc < 0) {
        return MIGRIS_PUS15_ERR_BAD_ARG;
    }
    return MIGRIS_PUS15_OK;
}

int migris_pus15_execute(migris_pus15_ctx_t* ctx,
                         migris_pktstore_t* store,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         const uint8_t* app_data,
                         size_t app_len,
                         uint8_t* out,
                         size_t out_cap) {
    /* enable / disable / report carry no application data, so app_data
     * may legitimately be NULL when app_len is 0. */
    if (ctx == NULL || store == NULL || tm_seq_count == NULL || out == NULL ||
        (app_data == NULL && app_len > 0U)) {
        return MIGRIS_PUS15_ERR_BAD_ARG;
    }
    switch (service_subtype) {
    case MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE:
        if (app_len != 0U) {
            return MIGRIS_PUS15_ERR_MALFORMED;
        }
        migris_pktstore_set_enabled(store, 1);
        return MIGRIS_PUS15_OK;
    case MIGRIS_PUS15_SUBTYPE_DISABLE_STORAGE:
        if (app_len != 0U) {
            return MIGRIS_PUS15_ERR_MALFORMED;
        }
        migris_pktstore_set_enabled(store, 0);
        return MIGRIS_PUS15_OK;
    case MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE:
        return pus15_downlink(store, app_data, app_len);
    case MIGRIS_PUS15_SUBTYPE_DELETE_RANGE:
        return pus15_delete(store, app_data, app_len);
    case MIGRIS_PUS15_SUBTYPE_REPORT_REQUEST:
        if (app_len != 0U) {
            return MIGRIS_PUS15_ERR_MALFORMED;
        }
        return pus15_build_store_report(
            ctx, store, apid, tm_seq_count, now_seconds, tc_source_id, out, out_cap);
    default:
        return MIGRIS_PUS15_ERR_BAD_SUBTYPE;
    }
}
