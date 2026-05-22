/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board housekeeping-structure store — bounded set of ground-defined
 * PUS-3 housekeeping structures. See migris/fsw/hkstore/hkstore.h for
 * the contract and rationale.
 *
 * Slots are not compacted on delete: a deleted structure clears its
 * `in_use` flag and leaves the slot for a later create to reclaim. This
 * keeps a borrowed structure pointer stable across the creation or
 * deletion of other structures — `migris_hkstore_due` hands one to the
 * PUS-3 codec, and the find / due scans walk the whole slot array
 * skipping free slots.
 */

#include "migris/fsw/hkstore/hkstore.h"

#include <stddef.h>
#include <stdint.h>

void migris_hkstore_init(migris_hkstore_t* store) {
    if (store == NULL) {
        return;
    }
    store->count = 0U;
    for (size_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        store->structures[i].in_use = 0;
    }
}

size_t migris_hkstore_count(const migris_hkstore_t* store) {
    return (store == NULL) ? 0U : store->count;
}

/* Index of the in-use structure with Structure ID `sid`, or -1 if none
 * is defined. */
static int hkstore_index_of(const migris_hkstore_t* store, uint16_t sid) {
    for (size_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        if (store->structures[i].in_use != 0 && store->structures[i].sid == sid) {
            return (int)i;
        }
    }
    return -1;
}

int migris_hkstore_create(migris_hkstore_t* store,
                          uint16_t sid,
                          const uint16_t* param_ids,
                          size_t param_count,
                          uint32_t interval_sec) {
    if (store == NULL || param_ids == NULL || param_count == 0U || sid < MIGRIS_HKSTORE_SID_MIN) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    if (param_count > MIGRIS_HKSTORE_MAX_PARAMS) {
        return MIGRIS_HKSTORE_ERR_TOO_MANY;
    }
    if (hkstore_index_of(store, sid) >= 0) {
        return MIGRIS_HKSTORE_ERR_DUPLICATE;
    }
    int slot = -1;
    for (size_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        if (store->structures[i].in_use == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return MIGRIS_HKSTORE_ERR_FULL;
    }
    migris_hk_structure_t* s = &store->structures[slot];
    s->sid = sid;
    s->param_count = param_count;
    for (size_t i = 0U; i < param_count; ++i) {
        s->param_ids[i] = param_ids[i];
    }
    s->interval_sec = interval_sec;
    s->last_emit_sec = 0U;
    s->enabled = 0;
    s->in_use = 1;
    store->count++;
    return MIGRIS_HKSTORE_OK;
}

int migris_hkstore_delete(migris_hkstore_t* store, uint16_t sid) {
    if (store == NULL) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    const int idx = hkstore_index_of(store, sid);
    if (idx < 0) {
        return MIGRIS_HKSTORE_ERR_NOT_FOUND;
    }
    store->structures[idx].in_use = 0;
    store->count--;
    return MIGRIS_HKSTORE_OK;
}

int migris_hkstore_set_enabled(migris_hkstore_t* store, uint16_t sid, int enabled) {
    if (store == NULL) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    const int idx = hkstore_index_of(store, sid);
    if (idx < 0) {
        return MIGRIS_HKSTORE_ERR_NOT_FOUND;
    }
    store->structures[idx].enabled = (enabled != 0) ? 1 : 0;
    return MIGRIS_HKSTORE_OK;
}

const migris_hk_structure_t* migris_hkstore_find(const migris_hkstore_t* store, uint16_t sid) {
    if (store == NULL) {
        return NULL;
    }
    const int idx = hkstore_index_of(store, sid);
    return (idx < 0) ? NULL : &store->structures[idx];
}

const migris_hk_structure_t* migris_hkstore_due(migris_hkstore_t* store, uint32_t now_seconds) {
    if (store == NULL) {
        return NULL;
    }
    /* The enabled, periodically-reported structure whose interval has
     * elapsed and which has waited longest — emissions round-robin in
     * staleness order. */
    int best = -1;
    for (size_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        const migris_hk_structure_t* s = &store->structures[i];
        if (s->in_use == 0 || s->enabled == 0 || s->interval_sec == 0U) {
            continue;
        }
        if ((now_seconds - s->last_emit_sec) < s->interval_sec) {
            continue; /* not yet due */
        }
        if (best < 0 || s->last_emit_sec < store->structures[best].last_emit_sec) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return NULL;
    }
    store->structures[best].last_emit_sec = now_seconds;
    return &store->structures[best];
}
