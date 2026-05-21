/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-11 (On-board time-based scheduling) — service handler.
 *
 * Strictness policy (same as the leaf services): on-board command
 * handling. Anything that does not match the pinned wire format byte
 * for byte is rejected. Insert ([4]) and delete ([5]) are
 * all-or-nothing — every item is decoded and validated before any
 * change to the schedule. The embedded telecommands carried by an
 * insert are stored verbatim and validated only at release time, when
 * the TC router dispatches them. Wire format pinned in
 * docs/wire/pus-11.md.
 */

#include "migris/fsw/pus/pus11.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"
#include "migris/fsw/schedule/schedule.h"

#include <stddef.h>
#include <stdint.h>

/* Read a 4-byte big-endian unsigned integer from `buf` at `off`. */
static uint32_t pus11_be32(const uint8_t* buf, size_t off) {
    return ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1U] << 16) |
           ((uint32_t)buf[off + 2U] << 8) | (uint32_t)buf[off + 3U];
}

/* Write `value` big-endian into `out` at `off`; return the next offset. */
static size_t pus11_put_be32(uint8_t* out, size_t off, uint32_t value) {
    out[off] = (uint8_t)((value >> 24) & 0xFFU);
    out[off + 1U] = (uint8_t)((value >> 16) & 0xFFU);
    out[off + 2U] = (uint8_t)((value >> 8) & 0xFFU);
    out[off + 3U] = (uint8_t)(value & 0xFFU);
    return off + 4U;
}

/* True iff the MIGRIS_SCHEDULE_REQUEST_ID_SIZE bytes at `a` and `b`
 * are equal — a request-identifier compare. */
static int pus11_request_id_eq(const uint8_t* a, const uint8_t* b) {
    for (size_t i = 0U; i < MIGRIS_SCHEDULE_REQUEST_ID_SIZE; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* True iff `candidate`'s request identifier matches any of the first
 * `upto` telecommands in `tc_ptrs` — an in-batch duplicate check. */
static int pus11_id_seen(const uint8_t* const* tc_ptrs, size_t upto, const uint8_t* candidate) {
    for (size_t i = 0U; i < upto; ++i) {
        if (pus11_request_id_eq(tc_ptrs[i], candidate) != 0) {
            return 1;
        }
    }
    return 0;
}

/* Total wire length of the embedded CCSDS telecommand that begins at
 * `app[off]`, read from its own Packet Data Length. Returns 0 if its
 * primary header or declared body does not fit within `app_len`. */
static size_t pus11_embedded_tc_total(const uint8_t* app, size_t off, size_t app_len) {
    if ((off + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) > app_len) {
        return 0U;
    }
    const uint16_t data_length =
        (uint16_t)(((uint16_t)app[off + 4U] << 8) | (uint16_t)app[off + 5U]);
    const size_t total = migris_ccsds_packet_total_size(data_length);
    if ((off + total) > app_len) {
        return 0U;
    }
    return total;
}

/* Decode and validate a [11,4] insert request (pass one of the
 * all-or-nothing apply). Application data is a 1-byte count N followed
 * by N (4-byte release time, embedded CCSDS telecommand) pairs. Fills
 * the parallel `release_times` / `tc_ptrs` / `tc_lens` arrays and
 * validates room, per-TC size, and request-identifier uniqueness (both
 * within the batch and against the live schedule). Returns
 * MIGRIS_PUS11_OK with no schedule change, or a negative code. */
static int pus11_decode_inserts(const migris_schedule_t* sched,
                                const uint8_t* app,
                                size_t app_len,
                                uint32_t* release_times,
                                const uint8_t** tc_ptrs,
                                size_t* tc_lens,
                                size_t* n_out) {
    if (app_len < 1U) {
        return MIGRIS_PUS11_ERR_MALFORMED;
    }
    const size_t count = app[0];
    if (count > MIGRIS_PUS11_MAX_PER_TC) {
        return MIGRIS_PUS11_ERR_TOO_MANY;
    }
    if ((migris_schedule_count(sched) + count) > MIGRIS_SCHEDULE_CAPACITY) {
        return MIGRIS_PUS11_ERR_FULL;
    }
    size_t off = 1U;
    for (size_t k = 0U; k < count; ++k) {
        if ((off + 4U) > app_len) {
            return MIGRIS_PUS11_ERR_MALFORMED;
        }
        release_times[k] = pus11_be32(app, off);
        off += 4U;
        const size_t total = pus11_embedded_tc_total(app, off, app_len);
        if (total == 0U) {
            return MIGRIS_PUS11_ERR_MALFORMED;
        }
        if (total > MIGRIS_SCHEDULE_TC_MAX) {
            return MIGRIS_PUS11_ERR_TC_TOO_LARGE;
        }
        tc_ptrs[k] = &app[off];
        tc_lens[k] = total;
        off += total;
        if (pus11_id_seen(tc_ptrs, k, tc_ptrs[k]) != 0 ||
            migris_schedule_find(sched, tc_ptrs[k]) != NULL) {
            return MIGRIS_PUS11_ERR_DUPLICATE;
        }
    }
    if (off != app_len) {
        return MIGRIS_PUS11_ERR_MALFORMED;
    }
    *n_out = count;
    return MIGRIS_PUS11_OK;
}

/* Decode a request-identifier list — application data of a [11,5]
 * delete or a [11,11] report: a 1-byte count N followed by N 4-byte
 * request identifiers. Records borrowed pointers into `app`. */
static int
pus11_decode_request_ids(const uint8_t* app, size_t app_len, const uint8_t** ids, size_t* n_out) {
    if (app_len < 1U) {
        return MIGRIS_PUS11_ERR_MALFORMED;
    }
    const size_t count = app[0];
    if (count > MIGRIS_PUS11_MAX_PER_TC) {
        return MIGRIS_PUS11_ERR_TOO_MANY;
    }
    if (app_len != (1U + (MIGRIS_SCHEDULE_REQUEST_ID_SIZE * count))) {
        return MIGRIS_PUS11_ERR_MALFORMED;
    }
    for (size_t k = 0U; k < count; ++k) {
        ids[k] = &app[1U + (MIGRIS_SCHEDULE_REQUEST_ID_SIZE * k)];
    }
    *n_out = count;
    return MIGRIS_PUS11_OK;
}

/* Execute a [11,4] insert request, all-or-nothing. */
static int pus11_insert(migris_schedule_t* sched, const uint8_t* app, size_t app_len) {
    uint32_t release_times[MIGRIS_PUS11_MAX_PER_TC] = {0};
    const uint8_t* tc_ptrs[MIGRIS_PUS11_MAX_PER_TC] = {0};
    size_t tc_lens[MIGRIS_PUS11_MAX_PER_TC] = {0};
    size_t count = 0U;
    const int rc =
        pus11_decode_inserts(sched, app, app_len, release_times, tc_ptrs, tc_lens, &count);
    if (rc != MIGRIS_PUS11_OK) {
        return rc;
    }
    /* pus11_decode_inserts verified room, per-TC size and request-id
     * uniqueness, so no insert here can fail. */
    for (size_t k = 0U; k < count; ++k) {
        (void)migris_schedule_insert(sched, release_times[k], tc_ptrs[k], tc_lens[k]);
    }
    return MIGRIS_PUS11_OK;
}

/* Execute a [11,5] delete request, all-or-nothing. */
static int pus11_delete(migris_schedule_t* sched, const uint8_t* app, size_t app_len) {
    const uint8_t* ids[MIGRIS_PUS11_MAX_PER_TC] = {0};
    size_t count = 0U;
    const int rc = pus11_decode_request_ids(app, app_len, ids, &count);
    if (rc != MIGRIS_PUS11_OK) {
        return rc;
    }
    /* Pass one: every named activity must currently be scheduled. */
    for (size_t k = 0U; k < count; ++k) {
        if (migris_schedule_find(sched, ids[k]) == NULL) {
            return MIGRIS_PUS11_ERR_NOT_FOUND;
        }
    }
    /* Pass two: delete. A repeated identifier in the list deletes once
     * and then no-ops — delete is idempotent. */
    for (size_t k = 0U; k < count; ++k) {
        (void)migris_schedule_delete(sched, ids[k]);
    }
    return MIGRIS_PUS11_OK;
}

/* Build one [11,12] summary report from the collected activities. */
static int pus11_build_summary_report(migris_pus11_ctx_t* ctx,
                                      uint16_t apid,
                                      uint16_t* tm_seq_count,
                                      uint32_t now_seconds,
                                      uint16_t tc_source_id,
                                      const migris_schedule_activity_t* const* found,
                                      size_t found_n,
                                      uint8_t* out,
                                      size_t out_cap) {
    /* Source data: 1-byte count + per activity (4-byte release time +
     * 4-byte request identifier). */
    const size_t source_data = 1U + (found_n * 8U);
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + source_data + 2U);
    const size_t packet_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + (size_t)data_field;
    if (out_cap < packet_size) {
        return MIGRIS_PUS11_ERR_BUF_TOO_SMALL;
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
        return MIGRIS_PUS11_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_SCHEDULING,
        .service_subtype = MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT,
        .msg_counter = ctx->msg_counter[0],
        .destination_id = tc_source_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS11_ERR_BUF_TOO_SMALL;
    }

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    out[off] = (uint8_t)found_n;
    off += 1U;
    for (size_t k = 0U; k < found_n; ++k) {
        off = pus11_put_be32(out, off, found[k]->release_time);
        for (size_t i = 0U; i < MIGRIS_SCHEDULE_REQUEST_ID_SIZE; ++i) {
            out[off + i] = found[k]->tc[i];
        }
        off += MIGRIS_SCHEDULE_REQUEST_ID_SIZE;
    }

    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);
    off += 2U;

    *tm_seq_count = (uint16_t)((seq_snapshot + 1U) & 0x3FFFU);
    ctx->msg_counter[0] = (uint8_t)(ctx->msg_counter[0] + 1U);
    return (int)off;
}

/* Execute a [11,11] summary-report request: a query, so a requested
 * identifier that is not scheduled is omitted from the report rather
 * than failing it. */
static int pus11_report(migris_pus11_ctx_t* ctx,
                        const migris_schedule_t* sched,
                        uint16_t apid,
                        uint16_t* tm_seq_count,
                        uint32_t now_seconds,
                        uint16_t tc_source_id,
                        const uint8_t* app,
                        size_t app_len,
                        uint8_t* out,
                        size_t out_cap) {
    const uint8_t* ids[MIGRIS_PUS11_MAX_PER_TC] = {0};
    size_t count = 0U;
    const int rc = pus11_decode_request_ids(app, app_len, ids, &count);
    if (rc != MIGRIS_PUS11_OK) {
        return rc;
    }
    const migris_schedule_activity_t* found[MIGRIS_PUS11_MAX_PER_TC] = {0};
    size_t found_n = 0U;
    for (size_t k = 0U; k < count; ++k) {
        const migris_schedule_activity_t* activity = migris_schedule_find(sched, ids[k]);
        if (activity != NULL) {
            found[found_n] = activity;
            found_n++;
        }
    }
    return pus11_build_summary_report(
        ctx, apid, tm_seq_count, now_seconds, tc_source_id, found, found_n, out, out_cap);
}

int migris_pus11_execute(migris_pus11_ctx_t* ctx,
                         migris_schedule_t* sched,
                         uint16_t apid,
                         uint16_t* tm_seq_count,
                         uint32_t now_seconds,
                         uint8_t service_subtype,
                         uint16_t tc_source_id,
                         const uint8_t* app_data,
                         size_t app_len,
                         uint8_t* out,
                         size_t out_cap) {
    /* enable / disable / reset carry no application data, so app_data
     * may legitimately be NULL when app_len is 0. */
    if (ctx == NULL || sched == NULL || tm_seq_count == NULL || out == NULL ||
        (app_data == NULL && app_len > 0U)) {
        return MIGRIS_PUS11_ERR_BAD_ARG;
    }
    switch (service_subtype) {
    case MIGRIS_PUS11_SUBTYPE_ENABLE:
        if (app_len != 0U) {
            return MIGRIS_PUS11_ERR_MALFORMED;
        }
        migris_schedule_set_enabled(sched, 1);
        return MIGRIS_PUS11_OK;
    case MIGRIS_PUS11_SUBTYPE_DISABLE:
        if (app_len != 0U) {
            return MIGRIS_PUS11_ERR_MALFORMED;
        }
        migris_schedule_set_enabled(sched, 0);
        return MIGRIS_PUS11_OK;
    case MIGRIS_PUS11_SUBTYPE_RESET:
        if (app_len != 0U) {
            return MIGRIS_PUS11_ERR_MALFORMED;
        }
        migris_schedule_reset(sched);
        return MIGRIS_PUS11_OK;
    case MIGRIS_PUS11_SUBTYPE_INSERT:
        return pus11_insert(sched, app_data, app_len);
    case MIGRIS_PUS11_SUBTYPE_DELETE:
        return pus11_delete(sched, app_data, app_len);
    case MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT_REQUEST:
        return pus11_report(ctx,
                            sched,
                            apid,
                            tm_seq_count,
                            now_seconds,
                            tc_source_id,
                            app_data,
                            app_len,
                            out,
                            out_cap);
    default:
        return MIGRIS_PUS11_ERR_BAD_SUBTYPE;
    }
}
