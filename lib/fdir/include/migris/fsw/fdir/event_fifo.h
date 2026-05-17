/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Bounded event FIFO — the framework's first decoupling buffer between
 * an anomaly *producer* that does not own a TM output buffer (the TC
 * router on a rejection, the main-loop UART RX-overflow detector, a
 * future FDIR monitor) and the single *emitter* that owns the downlink
 * buffer and serialises one PUS-5 report per drained record. Slice
 * fsw-8 — the abstraction pus5.h named as the explicit next step.
 *
 * Single-producer / single-consumer, **non-atomic by contract**: every
 * push and every pop runs in the same (main-loop) execution context.
 * No ISR ever touches this structure — the UART RX ISR keeps its own
 * `volatile uint32_t` drop counter; the main loop observes the delta
 * and is the one that pushes. The ring is therefore a plain index pair
 * with zero memory-ordering reasoning, consistent with the repo's
 * explicit "no atomic_t / no irq-lock in ISR" philosophy.
 *
 * Overflow policy: **drop-newest**. When the ring is full a push is
 * rejected and an internal dropped counter is advanced; the records
 * already queued (the causal head — the first faults, the ones that
 * diagnose a root cause) are preserved. The per-severity PUS-5 message
 * counter already lets ground detect a sequence gap on the wire, so
 * the dropped count is an internal-health field; surfacing it on the
 * wire is deferred until a consumer needs it.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib.
 */

#ifndef MIGRIS_FSW_FDIR_EVENT_FIFO_H_
#define MIGRIS_FSW_FDIR_EVENT_FIFO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the ring, in records. Compile-time constant so the
 *  buffer is statically sized (freestanding, no malloc). Override with
 *  ``-DMIGRIS_FDIR_EVENT_FIFO_CAPACITY=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_FDIR_EVENT_FIFO_CAPACITY``); the host library
 *  uses the default. */
#ifndef MIGRIS_FDIR_EVENT_FIFO_CAPACITY
#    define MIGRIS_FDIR_EVENT_FIFO_CAPACITY 16U
#endif

/** Maximum auxiliary-data bytes a queued event carries. The two fsw-8
 *  framework events use 3 (TC_REJECTED) and 4 (RX_OVERFLOW); 8 gives
 *  headroom while keeping the record 16 bytes. Always
 *  ``<= MIGRIS_PUS5_AUX_MAX_LEN`` (32) — the emitter passes this aux
 *  straight to the PUS-5 encoder. */
#define MIGRIS_FDIR_EVENT_AUX_MAX 8U

/** Event FIFO status / error codes. Same convention as the rest of the
 *  codec: 0 is success, negative is one of these. */
typedef enum {
    MIGRIS_EVENT_FIFO_OK = 0,
    MIGRIS_EVENT_FIFO_ERR_FULL = -1,   /**< Ring full — record dropped (drop-newest). */
    MIGRIS_EVENT_FIFO_ERR_BAD_ARG = -2 /**< NULL arg, or ``aux_len`` over the max. */
} migris_event_fifo_status_t;

/** One queued event, stored by value (no pointers — the producer does
 *  not own a buffer that outlives the push). ``severity`` holds a
 *  ``migris_pus5_severity_t`` value; ``aux``/``aux_len`` is the opaque
 *  event-specific data the PUS-5 encoder appends verbatim after the
 *  event ID; ``t_seconds`` is the FSW-clock CUC coarse time captured at
 *  detection (the report carries detection time, not drain time). */
typedef struct {
    uint16_t event_id;
    uint8_t severity;
    uint8_t aux_len;
    uint8_t aux[MIGRIS_FDIR_EVENT_AUX_MAX];
    uint32_t t_seconds;
} migris_fdir_event_t;

/** The ring itself. Caller-owned, zero-initialised once at startup (or
 *  via ``migris_event_fifo_init``). ``head`` is the index of the oldest
 *  queued record; ``count`` the number queued; ``dropped`` the running
 *  number of pushes rejected because the ring was full. */
typedef struct {
    migris_fdir_event_t slots[MIGRIS_FDIR_EVENT_FIFO_CAPACITY];
    size_t head;
    size_t count;
    uint32_t dropped;
} migris_event_fifo_t;

/** Reset a FIFO to empty (``head = count = dropped = 0``). A
 *  zero-initialised ``migris_event_fifo_t`` is already valid; this is
 *  provided for explicitness at startup. */
void migris_event_fifo_init(migris_event_fifo_t* fifo);

/** Enqueue a copy of ``ev`` (drop-newest on full).
 *
 *  Returns ``MIGRIS_EVENT_FIFO_OK`` on enqueue;
 *  ``MIGRIS_EVENT_FIFO_ERR_FULL`` (and advances ``fifo->dropped``) if
 *  the ring is full — the queued records are left untouched;
 *  ``MIGRIS_EVENT_FIFO_ERR_BAD_ARG`` if ``fifo``/``ev`` is NULL or
 *  ``ev->aux_len > MIGRIS_FDIR_EVENT_AUX_MAX`` (a producer bug, not a
 *  capacity event — ``dropped`` is *not* advanced). */
int migris_event_fifo_push(migris_event_fifo_t* fifo, const migris_fdir_event_t* ev);

/** Dequeue the oldest record into ``*out``. Returns 1 if a record was
 *  written, 0 if the ring was empty (``*out`` untouched) or an
 *  argument was NULL. */
int migris_event_fifo_pop(migris_event_fifo_t* fifo, migris_fdir_event_t* out);

/** Number of records currently queued. */
size_t migris_event_fifo_count(const migris_event_fifo_t* fifo);

/** Running count of pushes rejected because the ring was full. */
uint32_t migris_event_fifo_dropped(const migris_event_fifo_t* fifo);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_FDIR_EVENT_FIFO_H_
