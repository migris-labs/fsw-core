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

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/hkstore/hkstore.h"
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

static uint32_t get_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
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
    const uint16_t data_field =
        (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + MIGRIS_PUS3_HK_SOURCE_DATA_SIZE + 2U);
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

/* --- Structure management (slice fsw-15) -------------------------- */

/* Resolve every datapool parameter `structure` names into `values`
 * (caller-sized for MIGRIS_HKSTORE_MAX_PARAMS entries, which bounds
 * structure->param_count) and return the sum of their on-wire widths,
 * or -1 if the datapool does not define one of them. */
static int pus3_resolve_params(const migris_datapool_t* datapool,
                               const migris_hk_structure_t* structure,
                               migris_dp_value_t* values) {
    size_t total = 0U;
    for (size_t i = 0U; i < structure->param_count; ++i) {
        if (migris_datapool_get(datapool, structure->param_ids[i], &values[i]) !=
            MIGRIS_DATAPOOL_OK) {
            return -1;
        }
        total += migris_dp_type_width(values[i].type);
    }
    return (int)total;
}

int migris_pus3_build_dynamic_hk_report(migris_pus3_ctx_t* ctx,
                                        const migris_datapool_t* datapool,
                                        const migris_hk_structure_t* structure,
                                        uint16_t apid,
                                        uint16_t* tm_seq_count,
                                        uint32_t now_seconds,
                                        uint16_t destination_id,
                                        uint8_t* out,
                                        size_t out_cap) {
    if (ctx == NULL || datapool == NULL || structure == NULL || tm_seq_count == NULL ||
        out == NULL) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }

    /* Resolve every parameter up front: a structure naming a parameter
     * the datapool does not define fails the whole report, so the
     * packet length is deterministic before a single byte is written. */
    migris_dp_value_t values[MIGRIS_HKSTORE_MAX_PARAMS];
    const int value_bytes = pus3_resolve_params(datapool, structure, values);
    if (value_bytes < 0) {
        return MIGRIS_PUS3_ERR_UNKNOWN_PARAM;
    }

    /* data_length = (data field bytes) − 1. The data field is the TM
     * secondary header + source data (SID + parameter values) + CRC. */
    const size_t source_data = MIGRIS_PUS3_SID_SIZE + (size_t)value_bytes;
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + source_data + 2U);
    const size_t packet_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + (size_t)data_field;
    if (out_cap < packet_size) {
        return MIGRIS_PUS3_ERR_BUF_TOO_SMALL;
    }

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

    /* Source data — SID then each parameter value, big-endian, no
     * padding (docs/wire/pus-3.md). Values are MIB-decoded, not
     * self-describing: ground decodes from the structure definition. */
    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    out[off++] = (uint8_t)((structure->sid >> 8) & 0xFFU);
    out[off++] = (uint8_t)(structure->sid & 0xFFU);
    for (size_t i = 0U; i < structure->param_count; ++i) {
        const int written = migris_dp_value_encode(&values[i], &out[off], out_cap - off);
        if (written <= 0) {
            return MIGRIS_PUS3_ERR_BUF_TOO_SMALL; /* sized above — defensive */
        }
        off += (size_t)written;
    }

    /* CRC-16-CCITT-FALSE over every byte before the trailing CRC. */
    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit state only once the whole packet is built (mod 2^14 /
     * mod 2^8) — the [25] message counter is shared with the frozen
     * FRAMEWORK_DIAG report, both being subtype 25. */
    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[0] = (uint8_t)(ctx->msg_counter[0] + 1U);

    return (int)packet_size;
}

/* Execute a [3,1] create. Application data is SID(2) + count(1) +
 * parameter ID(2 each) + interval(4); the decode is all-or-nothing. */
static int pus3_exec_create(migris_hkstore_t* store, const uint8_t* app, size_t app_len) {
    /* SID(2) + count(1) + interval(4) is the 7-byte minimum; each
     * parameter the structure names adds 2 more. */
    if (app_len < 7U) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }
    const size_t count = app[2];
    if (app_len != 7U + (2U * count)) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }
    if (count > MIGRIS_HKSTORE_MAX_PARAMS) {
        return MIGRIS_PUS3_ERR_EXEC_FAILED;
    }
    const uint16_t sid = get_u16_be(&app[0]);
    uint16_t ids[MIGRIS_HKSTORE_MAX_PARAMS];
    for (size_t i = 0U; i < count; ++i) {
        ids[i] = get_u16_be(&app[3U + (2U * i)]);
    }
    const uint32_t interval = get_u32_be(&app[3U + (2U * count)]);
    const int rc = migris_hkstore_create(store, sid, ids, count, interval);
    return (rc == MIGRIS_HKSTORE_OK) ? MIGRIS_PUS3_OK : MIGRIS_PUS3_ERR_EXEC_FAILED;
}

/* Execute a [3,2] delete / [3,5] enable / [3,6] disable. Application
 * data is exactly one SID (2 bytes, big-endian). */
static int
pus3_exec_sid_only(migris_hkstore_t* store, const uint8_t* app, size_t app_len, uint8_t subtype) {
    if (app_len != MIGRIS_PUS3_SID_SIZE) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }
    const uint16_t sid = get_u16_be(&app[0]);
    if (subtype == MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE) {
        return (migris_hkstore_delete(store, sid) == MIGRIS_HKSTORE_OK)
                   ? MIGRIS_PUS3_OK
                   : MIGRIS_PUS3_ERR_EXEC_FAILED;
    }
    const int enable = (subtype == MIGRIS_PUS3_SUBTYPE_ENABLE_STRUCTURE) ? 1 : 0;
    return (migris_hkstore_set_enabled(store, sid, enable) == MIGRIS_HKSTORE_OK)
               ? MIGRIS_PUS3_OK
               : MIGRIS_PUS3_ERR_EXEC_FAILED;
}

int migris_pus3_execute(migris_hkstore_t* store,
                        uint8_t service_subtype,
                        const uint8_t* app_data,
                        size_t app_len) {
    if (store == NULL || app_data == NULL) {
        return MIGRIS_PUS3_ERR_BAD_ARG;
    }
    if (service_subtype == MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE) {
        return pus3_exec_create(store, app_data, app_len);
    }
    if (service_subtype == MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE ||
        service_subtype == MIGRIS_PUS3_SUBTYPE_ENABLE_STRUCTURE ||
        service_subtype == MIGRIS_PUS3_SUBTYPE_DISABLE_STRUCTURE) {
        return pus3_exec_sid_only(store, app_data, app_len, service_subtype);
    }
    return MIGRIS_PUS3_ERR_BAD_ARG; /* not a structure-management subtype */
}
