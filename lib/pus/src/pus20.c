/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-20 (On-board parameter management) — service handler.
 *
 * Strictness policy (same as the leaf services): on-board command
 * handling. Anything that does not match the pinned wire format byte
 * for byte is rejected. A [20,1] report request whose ID list does not
 * resolve, and a [20,3] set request that is malformed, names an
 * unknown or read-only parameter, or whose value bytes do not parse,
 * are execution-stage failures — the router turns them into a PUS-1[8]
 * completion failure. The value bytes themselves are serialised by the
 * datapool's typed codec. Wire format pinned in docs/wire/pus-20.md.
 */

#include "migris/fsw/pus/pus20.h"

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

/* Read a 2-byte big-endian parameter ID from `buf` at `off`. */
static migris_dp_param_id_t pus20_read_id(const uint8_t* buf, size_t off) {
    return (migris_dp_param_id_t)(((uint16_t)buf[off] << 8) | (uint16_t)buf[off + 1U]);
}

/* Decode and validate a [20,1] report request. Application data is a
 * 1-byte count N followed by N 2-byte IDs. Resolves every ID against
 * the datapool, recording the parameter pointers in `found` and the
 * total [20,2] source-data size (1 count byte + per parameter its ID
 * and value width) in `*source_out`. Returns MIGRIS_PUS20_OK or a
 * negative migris_pus20_status_t. */
static int pus20_collect(const migris_datapool_t* dp,
                         const uint8_t* app,
                         size_t app_len,
                         const migris_dp_param_t** found,
                         size_t* n_out,
                         size_t* source_out) {
    if (app_len < 1U) {
        return MIGRIS_PUS20_ERR_MALFORMED;
    }
    const size_t count = app[0];
    if (count > MIGRIS_PUS20_MAX_PARAMS_PER_TC) {
        return MIGRIS_PUS20_ERR_TOO_MANY;
    }
    if (app_len != 1U + (2U * count)) {
        return MIGRIS_PUS20_ERR_MALFORMED;
    }
    size_t source = 1U; /* the count byte */
    for (size_t k = 0U; k < count; ++k) {
        const migris_dp_param_id_t id = pus20_read_id(app, 1U + (2U * k));
        const migris_dp_param_t* param = migris_datapool_find(dp, id);
        if (param == NULL) {
            return MIGRIS_PUS20_ERR_UNKNOWN_ID;
        }
        found[k] = param;
        source += 2U + migris_dp_type_width(param->value.type);
    }
    *n_out = count;
    *source_out = source;
    return MIGRIS_PUS20_OK;
}

/* Execute a [20,1] report request: build one [20,2] parameter value
 * report into `out`. Returns the positive packet byte count, or a
 * negative migris_pus20_status_t. */
static int pus20_report(migris_pus20_ctx_t* ctx,
                        const migris_datapool_t* dp,
                        uint16_t apid,
                        uint16_t* tm_seq_count,
                        uint32_t now_seconds,
                        uint16_t tc_source_id,
                        const uint8_t* app,
                        size_t app_len,
                        uint8_t* out,
                        size_t out_cap) {
    const migris_dp_param_t* found[MIGRIS_PUS20_MAX_PARAMS_PER_TC] = {0};
    size_t count = 0U;
    size_t source_data = 0U;
    const int rc = pus20_collect(dp, app, app_len, found, &count, &source_data);
    if (rc != MIGRIS_PUS20_OK) {
        return rc;
    }

    /* data_length = (data field bytes) − 1; the data field is the TM
     * secondary header + source data + the 2-byte CRC. */
    const uint16_t data_field =
        (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + source_data + 2U);
    const size_t packet_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + (size_t)data_field;
    if (out_cap < packet_size) {
        return MIGRIS_PUS20_ERR_BUF_TOO_SMALL;
    }

    /* Captured before the advance: written into the CCSDS primary
     * header of the report it labels. */
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
        return MIGRIS_PUS20_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
        .service_subtype = MIGRIS_PUS20_SUBTYPE_VALUE_REPORT,
        .msg_counter = ctx->msg_counter[0],
        .destination_id = tc_source_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS20_ERR_BUF_TOO_SMALL;
    }

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    out[off] = (uint8_t)count;
    off += 1U;
    for (size_t k = 0U; k < count; ++k) {
        out[off] = (uint8_t)((found[k]->id >> 8) & 0xFFU);
        out[off + 1U] = (uint8_t)(found[k]->id & 0xFFU);
        off += 2U;
        const int written = migris_dp_value_encode(&found[k]->value, &out[off], out_cap - off);
        if (written <= 0) {
            return MIGRIS_PUS20_ERR_BUF_TOO_SMALL;
        }
        off += (size_t)written;
    }

    /* CRC-16-CCITT-FALSE over every byte before the trailing CRC. */
    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);
    off += 2U;

    /* Commit state only once the whole packet is built. */
    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[0] = (uint8_t)(ctx->msg_counter[0] + 1U);
    return (int)off;
}

/* Decode and validate a [20,3] set request into the `ids` / `vals`
 * arrays (pass one of the all-or-nothing apply). Application data is a
 * 1-byte count N followed by N (2-byte ID, value) pairs; each value is
 * decoded at the width its parameter's registered type dictates — an
 * unknown ID therefore makes the remaining bytes unparseable and
 * aborts the walk. Validates that every ID is defined and read-write.
 * Returns MIGRIS_PUS20_OK with no datapool writes, or a negative
 * migris_pus20_status_t. */
static int pus20_decode_sets(const migris_datapool_t* dp,
                             const uint8_t* app,
                             size_t app_len,
                             migris_dp_param_id_t* ids,
                             migris_dp_value_t* vals,
                             size_t* n_out) {
    if (app_len < 1U) {
        return MIGRIS_PUS20_ERR_MALFORMED;
    }
    const size_t count = app[0];
    if (count > MIGRIS_PUS20_MAX_PARAMS_PER_TC) {
        return MIGRIS_PUS20_ERR_TOO_MANY;
    }
    size_t off = 1U;
    for (size_t k = 0U; k < count; ++k) {
        if ((off + 2U) > app_len) {
            return MIGRIS_PUS20_ERR_MALFORMED;
        }
        const migris_dp_param_id_t id = pus20_read_id(app, off);
        off += 2U;
        const migris_dp_param_t* param = migris_datapool_find(dp, id);
        if (param == NULL) {
            return MIGRIS_PUS20_ERR_UNKNOWN_ID;
        }
        if (param->access != MIGRIS_DP_ACCESS_READ_WRITE) {
            return MIGRIS_PUS20_ERR_READ_ONLY;
        }
        const int read =
            migris_dp_value_decode(&vals[k], param->value.type, &app[off], app_len - off);
        if (read < 0) {
            return MIGRIS_PUS20_ERR_MALFORMED;
        }
        off += (size_t)read;
        ids[k] = id;
    }
    /* Every application-data byte must be accounted for. */
    if (off != app_len) {
        return MIGRIS_PUS20_ERR_MALFORMED;
    }
    *n_out = count;
    return MIGRIS_PUS20_OK;
}

/* Execute a [20,3] set request, all-or-nothing. */
static int pus20_set(migris_datapool_t* dp, const uint8_t* app, size_t app_len) {
    migris_dp_param_id_t ids[MIGRIS_PUS20_MAX_PARAMS_PER_TC] = {0};
    migris_dp_value_t vals[MIGRIS_PUS20_MAX_PARAMS_PER_TC] = {0};
    size_t count = 0U;
    const int rc = pus20_decode_sets(dp, app, app_len, ids, vals, &count);
    if (rc != MIGRIS_PUS20_OK) {
        return rc;
    }
    /* Pass two: apply. pus20_decode_sets verified every ID exists, is
     * read-write, and that each value was decoded with its parameter's
     * own type — so no write here can fail. A repeated ID is permitted
     * (last-writer-wins). */
    for (size_t k = 0U; k < count; ++k) {
        (void)migris_datapool_set(dp, ids[k], &vals[k]);
    }
    return MIGRIS_PUS20_OK;
}

int migris_pus20_execute(migris_pus20_ctx_t* ctx,
                         migris_datapool_t* dp,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         const uint8_t* app_data,
                         size_t app_len,
                         uint8_t* out,
                         size_t out_cap) {
    if (ctx == NULL || dp == NULL || tm_seq_count == NULL || app_data == NULL || out == NULL) {
        return MIGRIS_PUS20_ERR_BAD_ARG;
    }
    switch (service_subtype) {
    case MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST:
        return pus20_report(ctx,
                            dp,
                            apid,
                            tm_seq_count,
                            now_seconds,
                            tc_source_id,
                            app_data,
                            app_len,
                            out,
                            out_cap);
    case MIGRIS_PUS20_SUBTYPE_SET_REQUEST:
        return pus20_set(dp, app_data, app_len);
    default:
        return MIGRIS_PUS20_ERR_BAD_SUBTYPE;
    }
}
