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
    store->generation = 0U;
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
    store->generation++;
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
    store->generation++;
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
    const int next = (enabled != 0) ? 1 : 0;
    if (store->structures[idx].enabled == next) {
        return MIGRIS_HKSTORE_OK; /* No-op: don't bump generation on an unchanged value. */
    }
    store->structures[idx].enabled = next;
    store->generation++;
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

uint32_t migris_hkstore_generation(const migris_hkstore_t* store) {
    return (store == NULL) ? 0U : store->generation;
}

/* --- Serialisation (slice fsw-17) -------------------------------- */

/* Layout: count(2 BE) + { sid(2 BE), interval_sec(4 BE), enabled(1),
 * param_count(1), param_ids(2 BE) * param_count } * count. Only the
 * in-use slots are written, packed back-to-back; last_emit_sec and
 * in_use are NOT serialised. The generation counter is RAM-only.
 *
 * One static helper does the per-structure write so the top-level
 * loop stays inside the clang-tidy cognitive-complexity budget. */
static int hkstore_write_struct(const migris_hk_structure_t* s, uint8_t* out, size_t cap) {
    const size_t need = 2U + 4U + 1U + 1U + 2U * s->param_count;
    if (cap < need) {
        return MIGRIS_HKSTORE_ERR_BUF_TOO_SMALL;
    }
    out[0] = (uint8_t)((s->sid >> 8) & 0xFFU);
    out[1] = (uint8_t)(s->sid & 0xFFU);
    out[2] = (uint8_t)((s->interval_sec >> 24) & 0xFFU);
    out[3] = (uint8_t)((s->interval_sec >> 16) & 0xFFU);
    out[4] = (uint8_t)((s->interval_sec >> 8) & 0xFFU);
    out[5] = (uint8_t)(s->interval_sec & 0xFFU);
    out[6] = (uint8_t)((s->enabled != 0) ? 1U : 0U);
    out[7] = (uint8_t)s->param_count;
    for (size_t i = 0U; i < s->param_count; ++i) {
        out[8U + (2U * i)] = (uint8_t)((s->param_ids[i] >> 8) & 0xFFU);
        out[8U + (2U * i) + 1U] = (uint8_t)(s->param_ids[i] & 0xFFU);
    }
    return (int)need;
}

int migris_hkstore_serialize(const migris_hkstore_t* store, uint8_t* out, size_t out_cap) {
    if (store == NULL || out == NULL) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    if (out_cap < 2U) {
        return MIGRIS_HKSTORE_ERR_BUF_TOO_SMALL;
    }
    out[0] = (uint8_t)((store->count >> 8) & 0xFFU);
    out[1] = (uint8_t)(store->count & 0xFFU);
    size_t off = 2U;
    for (size_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        const migris_hk_structure_t* s = &store->structures[i];
        if (s->in_use == 0) {
            continue;
        }
        const int n = hkstore_write_struct(s, &out[off], out_cap - off);
        if (n < 0) {
            return n;
        }
        off += (size_t)n;
    }
    return (int)off;
}

/* Decode one structure into `slot`. Returns the number of bytes
 * consumed on success, or a negative migris_hkstore_status_t. The
 * SID-floor and duplicate-SID checks happen in the caller (the latter
 * needs the partially-rebuilt low-slot range). */
static int hkstore_read_struct(migris_hk_structure_t* slot, const uint8_t* in, size_t in_len) {
    if (in_len < 8U) {
        return MIGRIS_HKSTORE_ERR_TRUNCATED;
    }
    const uint16_t sid = (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
    const uint32_t interval_sec = ((uint32_t)in[2] << 24) | ((uint32_t)in[3] << 16) |
                                  ((uint32_t)in[4] << 8) | (uint32_t)in[5];
    const int enabled = (in[6] != 0U) ? 1 : 0;
    const uint8_t param_count = in[7];
    if (sid < MIGRIS_HKSTORE_SID_MIN) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    if ((size_t)param_count > MIGRIS_HKSTORE_MAX_PARAMS) {
        return MIGRIS_HKSTORE_ERR_TOO_MANY;
    }
    const size_t need = 8U + (2U * (size_t)param_count);
    if (in_len < need) {
        return MIGRIS_HKSTORE_ERR_TRUNCATED;
    }
    slot->sid = sid;
    slot->interval_sec = interval_sec;
    slot->enabled = enabled;
    slot->param_count = param_count;
    slot->last_emit_sec = 0U; /* Re-arm from now after restore. */
    slot->in_use = 1;
    for (size_t i = 0U; i < (size_t)param_count; ++i) {
        slot->param_ids[i] =
            (uint16_t)(((uint16_t)in[8U + (2U * i)] << 8) | (uint16_t)in[8U + (2U * i) + 1U]);
    }
    return (int)need;
}

int migris_hkstore_deserialize(migris_hkstore_t* store, const uint8_t* in, size_t in_len) {
    if (store == NULL || in == NULL) {
        return MIGRIS_HKSTORE_ERR_BAD_ARG;
    }
    /* A failed decode leaves the store empty — same post-init state,
     * never a half-restored set. */
    migris_hkstore_init(store);
    if (in_len < 2U) {
        return MIGRIS_HKSTORE_ERR_TRUNCATED;
    }
    const uint16_t count = (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
    if ((size_t)count > MIGRIS_HKSTORE_CAPACITY) {
        return MIGRIS_HKSTORE_ERR_FULL;
    }
    size_t off = 2U;
    for (uint16_t i = 0U; i < count; ++i) {
        const int n = hkstore_read_struct(&store->structures[i], &in[off], in_len - off);
        if (n < 0) {
            migris_hkstore_init(store);
            return n;
        }
        /* Reject a duplicate SID against the structures already
         * rebuilt — mirrors the migris_hkstore_create contract. */
        for (uint16_t j = 0U; j < i; ++j) {
            if (store->structures[j].sid == store->structures[i].sid) {
                migris_hkstore_init(store);
                return MIGRIS_HKSTORE_ERR_DUPLICATE;
            }
        }
        off += (size_t)n;
        store->count++;
    }
    return MIGRIS_HKSTORE_OK;
}
