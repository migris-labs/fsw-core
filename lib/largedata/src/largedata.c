/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Large-data downlink session — the stateful half of PUS-13.
 * See migris/fsw/largedata/largedata.h for the contract and rationale.
 *
 * The session is a cursor over a borrowed data unit. Each
 * migris_largedata_next_part call slices off the next chunk and hands
 * it to the stateless lib/pus/pus13.c codec; the last part returns the
 * session to IDLE so it can be reused for the next transfer.
 */

#include "migris/fsw/largedata/largedata.h"

#include "migris/fsw/pus/pus13.h"

#include <stddef.h>
#include <stdint.h>

void migris_largedata_init(migris_largedata_session_t* session) {
    if (session == NULL) {
        return;
    }
    session->state = MIGRIS_LARGEDATA_IDLE;
    session->unit = NULL;
    session->unit_len = 0U;
    session->cursor = 0U;
    session->transaction_id = 0U;
    session->next_part = 0U;
    session->total_parts = 0U;
    for (size_t i = 0U; i < sizeof session->pus13.msg_counter; ++i) {
        session->pus13.msg_counter[i] = 0U;
    }
}

int migris_largedata_active(const migris_largedata_session_t* session) {
    return (session != NULL && session->state == MIGRIS_LARGEDATA_ACTIVE) ? 1 : 0;
}

int migris_largedata_start(migris_largedata_session_t* session,
                           uint16_t transaction_id,
                           const uint8_t* unit,
                           size_t unit_len) {
    if (session == NULL || unit == NULL || unit_len == 0U) {
        return MIGRIS_LARGEDATA_ERR_BAD_ARG;
    }
    if (session->state != MIGRIS_LARGEDATA_IDLE) {
        return MIGRIS_LARGEDATA_ERR_BUSY;
    }
    if (unit_len > MIGRIS_LARGEDATA_UNIT_MAX) {
        return MIGRIS_LARGEDATA_ERR_UNIT_TOO_LARGE;
    }

    /* Ceiling division — the last part may be shorter than the part
     * size. unit_len >= 1 here, so total_parts >= 1. */
    session->total_parts =
        (uint16_t)((unit_len + MIGRIS_PUS13_PART_SIZE - 1U) / MIGRIS_PUS13_PART_SIZE);
    session->unit = unit;
    session->unit_len = unit_len;
    session->cursor = 0U;
    session->transaction_id = transaction_id;
    session->next_part = 0U;
    session->state = MIGRIS_LARGEDATA_ACTIVE;
    return MIGRIS_LARGEDATA_OK;
}

int migris_largedata_next_part(migris_largedata_session_t* session,
                               uint16_t apid,
                               uint16_t* tm_seq_count,
                               uint32_t now_seconds,
                               uint16_t destination_id,
                               uint8_t* out,
                               size_t out_cap) {
    if (session == NULL || tm_seq_count == NULL || out == NULL) {
        return MIGRIS_LARGEDATA_ERR_BAD_ARG;
    }
    if (session->state != MIGRIS_LARGEDATA_ACTIVE) {
        return 0;
    }

    /* This part carries at most one part-size chunk of the remainder. */
    size_t chunk = session->unit_len - session->cursor;
    if (chunk > MIGRIS_PUS13_PART_SIZE) {
        chunk = MIGRIS_PUS13_PART_SIZE;
    }

    const int rc = migris_pus13_build_part(&session->pus13,
                                           apid,
                                           tm_seq_count,
                                           now_seconds,
                                           destination_id,
                                           session->transaction_id,
                                           session->next_part,
                                           session->total_parts,
                                           &session->unit[session->cursor],
                                           chunk,
                                           out,
                                           out_cap);
    if (rc == MIGRIS_PUS13_ERR_BUF_TOO_SMALL) {
        return MIGRIS_LARGEDATA_ERR_BUF_TOO_SMALL; /* session left unchanged — caller may retry */
    }
    if (rc < 0) {
        return MIGRIS_LARGEDATA_ERR_BAD_ARG;
    }

    session->cursor += chunk;
    session->next_part = (uint16_t)(session->next_part + 1U);
    if (session->next_part >= session->total_parts) {
        session->state = MIGRIS_LARGEDATA_IDLE; /* last part emitted — transfer complete */
    }
    return rc;
}
