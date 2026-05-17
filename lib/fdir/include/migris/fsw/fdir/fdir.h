/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * FDIR primitives — slice fsw-8. The framework's first fault-detection
 * and -reporting layer: a detected anomaly becomes a spontaneous PUS-5
 * event report on the downlink, decoupled from its detector by the
 * bounded event FIFO.
 *
 * Scope this slice is **detection + event reporting only**. An anomaly
 * registry maps a typed anomaly to a (PUS-5 severity, event-definition
 * ID) pair; producers either use that typed API (the main-loop UART
 * RX-overflow detector) or report an already-classified event through
 * the generic event sink (the TC router, which must not depend on
 * FDIR — see migris/fsw/event_sink.h). Both paths enqueue into the
 * same FIFO; the buffer owner drains it, one PUS-5 report per record.
 *
 * Isolation and Recovery — occurrence counters with thresholds,
 * debounce / confirmation, recovery actions, mode transitions, FDIR
 * enable/disable (PUS-5 control subtypes 5/6), persistence across
 * reset — are deliberately **deferred**. They presuppose a mode
 * manager and a recoverable-subsystem consumer, neither of which
 * exists yet; building them now would be an abstraction with no
 * caller. Trigger to revisit: the first slice that introduces a mode
 * manager or a subsystem with a defined recovery action.
 *
 * The severity / event-ID mapping in the registry (fdir.c) is the
 * single source of truth for how an anomaly classifies on the wire;
 * it is operator-meaningful and intentionally one edit to retune.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib.
 */

#ifndef MIGRIS_FSW_FDIR_FDIR_H_
#define MIGRIS_FSW_FDIR_FDIR_H_

#include "migris/fsw/event_sink.h"
#include "migris/fsw/fdir/event_fifo.h"
#include "migris/fsw/pus/pus5.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Typed anomalies for the producer API. The static registry in
 *  fdir.c maps each to a (severity, event-definition ID) pair and an
 *  aux encoding of the supplied ``detail`` word. A ``switch``, not a
 *  registration table — the table is the next abstraction, earned at a
 *  third independent anomaly (cf. the fsw-7 service-dispatch decision). */
typedef enum {
    /** TC failed acceptance. ``detail`` =
     *  ``fc << 16 | service_type << 8 | service_subtype`` → 3-byte
     *  aux. Severity low [2]; event ``MIGRIS_PUS5_EVT_TC_REJECTED``. */
    MIGRIS_FDIR_ANOM_TC_REJECTED = 0,
    /** UART RX-ring overflow. ``detail`` = bytes dropped since the
     *  last report → 4-byte big-endian aux. Severity medium [3];
     *  event ``MIGRIS_PUS5_EVT_RX_OVERFLOW``. */
    MIGRIS_FDIR_ANOM_RX_OVERFLOW = 1
} migris_fdir_anomaly_t;

/** FDIR state. Caller-owned, zero-initialised once at startup (or via
 *  ``migris_fdir_init``). This slice holds only the event FIFO (the
 *  outbox); recovery state arrives with its first consumer. */
typedef struct {
    migris_event_fifo_t fifo;
} migris_fdir_ctx_t;

/** Reset an FDIR context to empty. A zero-initialised
 *  ``migris_fdir_ctx_t`` is already valid; this is provided for
 *  explicitness at startup. */
void migris_fdir_init(migris_fdir_ctx_t* ctx);

/** Build a borrowed event-sink view onto ``ctx``, for a generic
 *  producer (the TC router) that reports an anomaly it has already
 *  classified into a (severity, event-definition ID, aux). The
 *  returned value must outlive the producer's use of it — keep it
 *  where the producer's context can point at it. */
migris_event_sink_t migris_fdir_event_sink(migris_fdir_ctx_t* ctx);

/** Typed producer: report ``anomaly`` at ``now_seconds`` with a single
 *  ``detail`` word the registry encodes into the event's auxiliary
 *  data. For producers that think in anomaly terms (the main-loop
 *  RX-overflow detector). Returns ``MIGRIS_EVENT_FIFO_OK`` if
 *  enqueued, a negative ``migris_event_fifo_status_t`` if the FIFO was
 *  full (event dropped, counted) or the anomaly id was unknown. */
int migris_fdir_report_anomaly(migris_fdir_ctx_t* ctx,
                               migris_fdir_anomaly_t anomaly,
                               uint32_t now_seconds,
                               uint32_t detail);

/** Drain one queued event into a PUS-5 report.
 *
 *  Pops the oldest event and serialises it with
 *  ``migris_pus5_build_event_report`` as a spontaneous report
 *  (destination ID 0), threading the shared per-APID ``tm_seq_count``
 *  and the caller-owned PUS-5 ``ctx`` exactly as any spontaneous
 *  report does — so the per-APID CCSDS sequence stays strictly
 *  monotonic across boot, verification/service bursts, periodic
 *  housekeeping and these anomalies.
 *
 *  Returns the positive byte count written; ``0`` if the FIFO was
 *  empty (``out`` untouched, nothing consumed);
 *  ``MIGRIS_PUS5_ERR_BUF_TOO_SMALL`` if ``out_cap <
 *  MIGRIS_PUS5_TM_MAX_PACKET_SIZE`` (checked up front — nothing
 *  consumed, so the event is not lost).
 *
 *  The caller owns TX ordering and emits one report per call: the TC
 *  router writes its PUS-1 verification into its own buffer and
 *  returns; the buffer owner drains FDIR *afterwards*, so a rejected
 *  TC's PUS-1 ack precedes its PUS-5 anomaly on the wire. */
int migris_fdir_drain(migris_fdir_ctx_t* ctx,
                      uint16_t apid,
                      uint16_t* tm_seq_count,
                      migris_pus5_ctx_t* pus5_ctx,
                      uint8_t* out,
                      size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_FDIR_FDIR_H_
