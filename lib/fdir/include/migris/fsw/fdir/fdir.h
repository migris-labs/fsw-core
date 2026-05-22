/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * FDIR primitives — slice fsw-8. The framework's first fault-detection
 * and -reporting layer: a detected anomaly becomes a spontaneous PUS-5
 * event report on the downlink, decoupled from its detector by the
 * bounded event FIFO.
 *
 * Detection and event reporting (slice fsw-8): an anomaly registry
 * maps a typed anomaly to a (PUS-5 severity, event-definition ID)
 * pair; producers either use that typed API (the main-loop UART
 * RX-overflow detector) or report an already-classified event through
 * the generic event sink (the TC router, which must not depend on
 * FDIR — see migris/fsw/event_sink.h). Both paths enqueue into the
 * same FIFO; the buffer owner drains it, one PUS-5 report per record.
 *
 * Isolation and recovery (slice fsw-14): each anomaly accumulates a
 * saturating occurrence count; when it crosses an application-supplied
 * threshold the fault is *confirmed* — a single transient never
 * recovers (debounce) — and FDIR autonomously commands a transition to
 * a safe mode through the mode manager (lib/mode/) and emits a
 * high-severity FDIR_RECOVERY event. Recovery is armed by
 * ``migris_fdir_arm_recovery`` and can be suppressed with
 * ``migris_fdir_set_enabled`` (for commissioning, where a confirmed
 * fault must not safe the vehicle). Persistence of the occurrence
 * counters and the confirmation latch across reset stays deferred —
 * they are RAM-only, like every other framework store, until a
 * non-volatile-storage subsystem exists.
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
#include "migris/fsw/mode/mode.h"
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

/** Number of distinct anomaly types — the size of the per-anomaly
 *  occurrence / threshold / confirmation arrays. Kept in lockstep with
 *  ``migris_fdir_anomaly_t``: adding an anomaly bumps the enum, this
 *  count, and the registry ``switch`` in fdir.c together. */
#define MIGRIS_FDIR_ANOMALY_COUNT 2U

/** Per-anomaly confirmation policy, supplied by the application to
 *  ``migris_fdir_arm_recovery``. ``threshold`` is the occurrence count
 *  at which the anomaly is *confirmed* and recovery fires; 0 means the
 *  anomaly never confirms (detection-only). fsw-core hard-codes no
 *  thresholds — they are mission tuning, supplied like the datapool
 *  parameter set or the mode set. */
typedef struct {
    migris_fdir_anomaly_t anomaly;
    uint16_t threshold;
} migris_fdir_confirm_def_t;

/** FDIR state. Caller-owned, zero-initialised once at startup (or via
 *  ``migris_fdir_init``). Holds the event FIFO (the outbox) and, since
 *  slice fsw-14, the isolation/recovery state: per-anomaly occurrence
 *  counters, confirmation thresholds, a per-anomaly confirmation latch,
 *  and the wiring to the mode manager for the autonomous safe-mode
 *  transition. The recovery fields are zero (no recovery) until
 *  ``migris_fdir_arm_recovery`` is called. */
typedef struct {
    migris_event_fifo_t fifo;
    uint16_t occurrences[MIGRIS_FDIR_ANOMALY_COUNT]; /**< Saturating per-anomaly count. */
    uint16_t
        thresholds[MIGRIS_FDIR_ANOMALY_COUNT];    /**< Per-anomaly confirm threshold; 0 = never. */
    uint8_t confirmed[MIGRIS_FDIR_ANOMALY_COUNT]; /**< Latched once recovery has fired. */
    migris_mode_manager_t* mode; /**< Recovery target manager; NULL = no recovery. */
    migris_mode_id_t safe_mode;  /**< Mode FDIR commands on a confirmed fault. */
    int recovery_enabled;        /**< 1 = recovery armed, 0 = suppressed (commissioning). */
} migris_fdir_ctx_t;

/** Reset an FDIR context to empty, with recovery disarmed (no
 *  thresholds, no mode manager). A zero-initialised
 *  ``migris_fdir_ctx_t`` is already valid; this is provided for
 *  explicitness at startup. Arm recovery afterwards with
 *  ``migris_fdir_arm_recovery``. */
void migris_fdir_init(migris_fdir_ctx_t* ctx);

/** Arm FDIR isolation/recovery. Call after ``migris_fdir_init``.
 *
 *  ``mode`` is the mode manager FDIR commands on a confirmed fault
 *  (it may be NULL — then a fault still counts and latches but no
 *  transition is commanded); ``safe_mode`` is the mode it requests.
 *  ``confirms`` supplies per-anomaly thresholds — an anomaly absent
 *  from the array keeps threshold 0 (never confirms, detection-only).
 *  Resets the occurrence counters and confirmation latches and enables
 *  recovery. Returns ``MIGRIS_EVENT_FIFO_OK``, or
 *  ``MIGRIS_EVENT_FIFO_ERR_BAD_ARG`` on a NULL ``ctx``, a NULL
 *  ``confirms`` with ``n_confirms`` > 0, or a confirm-def naming an
 *  anomaly outside ``migris_fdir_anomaly_t``. */
int migris_fdir_arm_recovery(migris_fdir_ctx_t* ctx,
                             migris_mode_manager_t* mode,
                             migris_mode_id_t safe_mode,
                             const migris_fdir_confirm_def_t* confirms,
                             size_t n_confirms);

/** Enable (the armed default) or suppress FDIR recovery. While
 *  suppressed a confirmed anomaly still counts, still latches, and
 *  still downlinks its PUS-5 anomaly report — but FDIR emits no
 *  FDIR_RECOVERY event and commands no mode transition. For
 *  commissioning, where a confirmed fault must not safe the vehicle. */
void migris_fdir_set_enabled(migris_fdir_ctx_t* ctx, int enabled);

/** Non-zero iff FDIR recovery is currently armed and enabled. */
int migris_fdir_is_enabled(const migris_fdir_ctx_t* ctx);

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
