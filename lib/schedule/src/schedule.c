/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board schedule — bounded, time-tagged TC store. See
 * migris/fsw/schedule/schedule.h for the contract and rationale.
 *
 * Activities are kept unordered: release order is decided per tick by
 * migris_schedule_pop_due (earliest release time first), so insert and
 * delete need not maintain any ordering — delete fills the gap with
 * the last entry in O(1).
 */

#include "migris/fsw/schedule/schedule.h"

#include <stddef.h>
#include <stdint.h>

void migris_schedule_init(migris_schedule_t* sched) {
    if (sched == NULL) {
        return;
    }
    sched->count = 0U;
    sched->enabled = 0;
}

void migris_schedule_reset(migris_schedule_t* sched) {
    if (sched == NULL) {
        return;
    }
    sched->count = 0U;
}

void migris_schedule_set_enabled(migris_schedule_t* sched, int enabled) {
    if (sched == NULL) {
        return;
    }
    sched->enabled = (enabled != 0) ? 1 : 0;
}

int migris_schedule_is_enabled(const migris_schedule_t* sched) {
    return (sched != NULL && sched->enabled != 0) ? 1 : 0;
}

size_t migris_schedule_count(const migris_schedule_t* sched) {
    return (sched == NULL) ? 0U : sched->count;
}

/* True iff the MIGRIS_SCHEDULE_REQUEST_ID_SIZE bytes at `a` and `b`
 * are equal — a request-identifier compare. */
static int sched_request_id_eq(const uint8_t* a, const uint8_t* b) {
    for (size_t i = 0U; i < MIGRIS_SCHEDULE_REQUEST_ID_SIZE; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Index of the activity whose request identifier (its tc[0..3])
 * matches `request_id`, or -1 if none is scheduled. */
static int sched_index_of(const migris_schedule_t* sched, const uint8_t* request_id) {
    for (size_t i = 0U; i < sched->count; ++i) {
        if (sched_request_id_eq(sched->activities[i].tc, request_id) != 0) {
            return (int)i;
        }
    }
    return -1;
}

int migris_schedule_insert(migris_schedule_t* sched,
                           uint32_t release_time,
                           const uint8_t* tc,
                           size_t tc_len) {
    if (sched == NULL || tc == NULL || tc_len < MIGRIS_SCHEDULE_REQUEST_ID_SIZE) {
        return MIGRIS_SCHEDULE_ERR_BAD_ARG;
    }
    if (tc_len > MIGRIS_SCHEDULE_TC_MAX) {
        return MIGRIS_SCHEDULE_ERR_TC_TOO_LARGE;
    }
    if (sched_index_of(sched, tc) >= 0) {
        return MIGRIS_SCHEDULE_ERR_DUPLICATE;
    }
    if (sched->count >= MIGRIS_SCHEDULE_CAPACITY) {
        return MIGRIS_SCHEDULE_ERR_FULL;
    }
    migris_schedule_activity_t* slot = &sched->activities[sched->count];
    slot->release_time = release_time;
    slot->tc_len = tc_len;
    for (size_t i = 0U; i < tc_len; ++i) {
        slot->tc[i] = tc[i];
    }
    sched->count++;
    return MIGRIS_SCHEDULE_OK;
}

int migris_schedule_delete(migris_schedule_t* sched, const uint8_t* request_id) {
    if (sched == NULL || request_id == NULL) {
        return MIGRIS_SCHEDULE_ERR_BAD_ARG;
    }
    const int idx = sched_index_of(sched, request_id);
    if (idx < 0) {
        return MIGRIS_SCHEDULE_ERR_NOT_FOUND;
    }
    sched->activities[idx] = sched->activities[sched->count - 1U];
    sched->count--;
    return MIGRIS_SCHEDULE_OK;
}

const migris_schedule_activity_t* migris_schedule_find(const migris_schedule_t* sched,
                                                       const uint8_t* request_id) {
    if (sched == NULL || request_id == NULL) {
        return NULL;
    }
    const int idx = sched_index_of(sched, request_id);
    if (idx < 0) {
        return NULL;
    }
    return &sched->activities[idx];
}

int migris_schedule_pop_due(migris_schedule_t* sched,
                            uint32_t now_seconds,
                            uint8_t* out_tc,
                            size_t out_cap,
                            size_t* out_len) {
    if (sched == NULL || out_tc == NULL || out_len == NULL) {
        return MIGRIS_SCHEDULE_ERR_BAD_ARG;
    }
    if (sched->enabled == 0) {
        return 0;
    }
    /* The due activity (release_time <= now) with the earliest release
     * time — releases run in time order. */
    int best = -1;
    for (size_t i = 0U; i < sched->count; ++i) {
        if (sched->activities[i].release_time > now_seconds) {
            continue;
        }
        if (best < 0 || sched->activities[i].release_time < sched->activities[best].release_time) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return 0; /* nothing due */
    }
    const migris_schedule_activity_t* due = &sched->activities[best];
    if (out_cap < due->tc_len) {
        /* Caller's buffer cannot hold it — leave it scheduled. */
        return MIGRIS_SCHEDULE_ERR_BUF_TOO_SMALL;
    }
    for (size_t i = 0U; i < due->tc_len; ++i) {
        out_tc[i] = due->tc[i];
    }
    *out_len = due->tc_len;
    sched->activities[best] = sched->activities[sched->count - 1U];
    sched->count--;
    return 1;
}
