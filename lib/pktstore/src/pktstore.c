/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board packet store — bounded, RAM-backed, circular TM buffer.
 * See migris/fsw/pktstore/pktstore.h for the contract and rationale.
 *
 * The buffer is time-ordered (the FSW clock is monotonic, so stored
 * packets have non-decreasing storage times). While a retrieval is in
 * progress the buffer is frozen — store and delete are suspended — so
 * the retrieval cursor (an offset from `head`) stays valid.
 */

#include "migris/fsw/pktstore/pktstore.h"

#include <stddef.h>
#include <stdint.h>

void migris_pktstore_init(migris_pktstore_t* store) {
    if (store == NULL) {
        return;
    }
    store->head = 0U;
    store->count = 0U;
    store->enabled = 1;
    store->retrieval_active = 0;
    store->retrieval_end = 0U;
    store->retrieval_cursor = 0U;
}

void migris_pktstore_set_enabled(migris_pktstore_t* store, int enabled) {
    if (store == NULL) {
        return;
    }
    store->enabled = (enabled != 0) ? 1 : 0;
}

int migris_pktstore_is_enabled(const migris_pktstore_t* store) {
    return (store != NULL && store->enabled != 0) ? 1 : 0;
}

size_t migris_pktstore_count(const migris_pktstore_t* store) {
    return (store == NULL) ? 0U : store->count;
}

int migris_pktstore_retrieval_active(const migris_pktstore_t* store) {
    return (store != NULL && store->retrieval_active != 0) ? 1 : 0;
}

int migris_pktstore_store(migris_pktstore_t* store,
                          const uint8_t* packet,
                          size_t packet_len,
                          uint32_t storage_time) {
    if (store == NULL || packet == NULL || packet_len == 0U) {
        return MIGRIS_PKTSTORE_ERR_BAD_ARG;
    }
    if (packet_len > MIGRIS_PKTSTORE_PACKET_MAX) {
        return MIGRIS_PKTSTORE_ERR_PACKET_TOO_LARGE;
    }
    if (store->enabled == 0 || store->retrieval_active != 0) {
        return 0; /* storage suspended — not an error */
    }
    if (store->count >= MIGRIS_PKTSTORE_CAPACITY) {
        /* Circular: overwrite the oldest packet. */
        store->head = (store->head + 1U) % MIGRIS_PKTSTORE_CAPACITY;
        store->count--;
    }
    const size_t tail = (store->head + store->count) % MIGRIS_PKTSTORE_CAPACITY;
    migris_pktstore_entry_t* slot = &store->entries[tail];
    slot->storage_time = storage_time;
    slot->packet_len = packet_len;
    for (size_t i = 0U; i < packet_len; ++i) {
        slot->packet[i] = packet[i];
    }
    store->count++;
    return 1;
}

int migris_pktstore_delete_up_to(migris_pktstore_t* store, uint32_t time_seconds) {
    if (store == NULL) {
        return MIGRIS_PKTSTORE_ERR_BAD_ARG;
    }
    if (store->retrieval_active != 0) {
        return MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE;
    }
    /* Oldest at head, time-ordered — delete the leading run. */
    int deleted = 0;
    while (store->count > 0U && store->entries[store->head].storage_time <= time_seconds) {
        store->head = (store->head + 1U) % MIGRIS_PKTSTORE_CAPACITY;
        store->count--;
        deleted++;
    }
    return deleted;
}

int migris_pktstore_arm_retrieval(migris_pktstore_t* store, uint32_t from_time, uint32_t to_time) {
    if (store == NULL || from_time > to_time) {
        return MIGRIS_PKTSTORE_ERR_BAD_ARG;
    }
    if (store->retrieval_active != 0) {
        return MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE;
    }
    /* Cursor = offset from head of the first entry at or after
     * `from_time`. The store is time-ordered, so the window's packets
     * are the contiguous run starting there. */
    size_t cursor = 0U;
    while (cursor < store->count) {
        const size_t idx = (store->head + cursor) % MIGRIS_PKTSTORE_CAPACITY;
        if (store->entries[idx].storage_time >= from_time) {
            break;
        }
        cursor++;
    }
    store->retrieval_cursor = cursor;
    store->retrieval_end = to_time;
    store->retrieval_active = 1;
    return MIGRIS_PKTSTORE_OK;
}

int migris_pktstore_retrieve_next(migris_pktstore_t* store,
                                  uint8_t* out,
                                  size_t out_cap,
                                  size_t* out_len) {
    if (store == NULL || out == NULL || out_len == NULL) {
        return MIGRIS_PKTSTORE_ERR_BAD_ARG;
    }
    if (store->retrieval_active == 0) {
        return 0;
    }
    if (store->retrieval_cursor >= store->count) {
        store->retrieval_active = 0;
        return 0; /* window exhausted */
    }
    const size_t idx = (store->head + store->retrieval_cursor) % MIGRIS_PKTSTORE_CAPACITY;
    const migris_pktstore_entry_t* entry = &store->entries[idx];
    if (entry->storage_time > store->retrieval_end) {
        store->retrieval_active = 0;
        return 0; /* past the window end (time-ordered — nothing later qualifies) */
    }
    if (out_cap < entry->packet_len) {
        return MIGRIS_PKTSTORE_ERR_BUF_TOO_SMALL;
    }
    for (size_t i = 0U; i < entry->packet_len; ++i) {
        out[i] = entry->packet[i];
    }
    *out_len = entry->packet_len;
    store->retrieval_cursor++;
    return 1;
}

int migris_pktstore_span(const migris_pktstore_t* store, uint32_t* oldest, uint32_t* newest) {
    if (store == NULL || oldest == NULL || newest == NULL || store->count == 0U) {
        return 0;
    }
    *oldest = store->entries[store->head].storage_time;
    const size_t last = (store->head + store->count - 1U) % MIGRIS_PKTSTORE_CAPACITY;
    *newest = store->entries[last].storage_time;
    return 1;
}
