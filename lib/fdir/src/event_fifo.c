/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Bounded event FIFO — drop-newest, single-context, non-atomic.
 * See migris/fsw/fdir/event_fifo.h for the contract and rationale.
 */

#include "migris/fsw/fdir/event_fifo.h"

#include <stddef.h>
#include <stdint.h>

void migris_event_fifo_init(migris_event_fifo_t* fifo) {
    if (fifo == NULL) {
        return;
    }
    fifo->head = 0U;
    fifo->count = 0U;
    fifo->dropped = 0U;
}

int migris_event_fifo_push(migris_event_fifo_t* fifo, const migris_fdir_event_t* ev) {
    if (fifo == NULL || ev == NULL) {
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }
    if (ev->aux_len > MIGRIS_FDIR_EVENT_AUX_MAX) {
        /* Producer bug, not a capacity event — do not touch `dropped`. */
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }
    if (fifo->count >= MIGRIS_FDIR_EVENT_FIFO_CAPACITY) {
        /* Drop-newest: the queued (causal-head) records are the ones
         * that diagnose a root cause; preserve them, count the loss. */
        fifo->dropped++;
        return MIGRIS_EVENT_FIFO_ERR_FULL;
    }

    const size_t tail = (fifo->head + fifo->count) % MIGRIS_FDIR_EVENT_FIFO_CAPACITY;
    fifo->slots[tail] = *ev;
    fifo->count++;
    return MIGRIS_EVENT_FIFO_OK;
}

int migris_event_fifo_pop(migris_event_fifo_t* fifo, migris_fdir_event_t* out) {
    if (fifo == NULL || out == NULL || fifo->count == 0U) {
        return 0;
    }
    *out = fifo->slots[fifo->head];
    fifo->head = (fifo->head + 1U) % MIGRIS_FDIR_EVENT_FIFO_CAPACITY;
    fifo->count--;
    return 1;
}

size_t migris_event_fifo_count(const migris_event_fifo_t* fifo) {
    return (fifo == NULL) ? 0U : fifo->count;
}

uint32_t migris_event_fifo_dropped(const migris_event_fifo_t* fifo) {
    return (fifo == NULL) ? 0U : fifo->dropped;
}
