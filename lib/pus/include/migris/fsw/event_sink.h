/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Event sink — the seam that lets a generic producer report an anomaly
 * without depending on the fault-management subsystem that consumes it.
 *
 * The TC router (generic PUS infrastructure, lib/pus/) detects rejected
 * commands; FDIR (a separate framework concern, lib/fdir/) decides what
 * to do with them and turns them into PUS-5 telemetry. The router must
 * not #include FDIR — that would couple generic dispatch to a specific
 * fault policy and mislabel FDIR as a PUS service. This header is the
 * minimal contract between them: a two-field vtable. The router holds a
 * nullable ``const migris_event_sink_t*`` and, on a rejection, calls
 * ``sink->report(sink->self, …)``; FDIR provides the concrete sink
 * (see migris/fsw/fdir/fdir.h). A NULL sink — the zero-initialised
 * default — means "no consumer wired"; the router emits nothing extra,
 * so callers that do not use FDIR are unaffected.
 *
 * It depends only on ``migris_pus5_severity_t`` from pus5.h — the
 * wire-level anomaly classification, not FDIR policy. The producer
 * supplies the severity and the PUS-5 event identity (both frozen
 * cross-repo wire constants it is already entitled to reference, like
 * the PUS-1 failure codes it already uses); the sink owns buffering
 * and emission.
 *
 * Sink contract — an implementation of ``report`` MUST:
 *   * be callable from the single main-loop producer context (the
 *     event FIFO is non-atomic by design — no ISR ever calls a sink);
 *   * not block and not emit TM (it only records the event for a
 *     later drain by the buffer owner);
 *   * copy anything it needs out of ``aux`` before returning (``aux``
 *     is borrowed, not owned);
 *   * treat ``now_seconds`` as the detection time to stamp on the
 *     recorded event (the report carries detection time, not the
 *     later drain time);
 *   * return 0 if the event was recorded, non-zero if it was dropped.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib.
 */

#ifndef MIGRIS_FSW_EVENT_SINK_H_
#define MIGRIS_FSW_EVENT_SINK_H_

#include "migris/fsw/pus/pus5.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A borrowed reference to an event consumer. ``self`` is the
 *  consumer's opaque context, passed back as the first argument of
 *  ``report``. Both fields zero (the struct value-initialised, or the
 *  holding pointer NULL) is a valid "no consumer" state the producer
 *  must tolerate. */
typedef struct migris_event_sink {
    /** Record one detected event. ``now_seconds`` is the FSW-clock
     *  detection time to stamp on it; ``severity`` is the PUS-5
     *  severity (selects the report subtype); ``event_id`` is the
     *  2-byte event-definition ID; ``aux``/``aux_len`` is optional
     *  event-specific data (borrowed — copy before returning;
     *  ``aux_len`` is at most ``MIGRIS_PUS5_AUX_MAX_LEN``). Returns 0
     *  if recorded, non-zero if dropped. */
    int (*report)(void* self,
                  uint32_t now_seconds,
                  migris_pus5_severity_t severity,
                  uint16_t event_id,
                  const uint8_t* aux,
                  size_t aux_len);
    void* self;
} migris_event_sink_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_EVENT_SINK_H_
