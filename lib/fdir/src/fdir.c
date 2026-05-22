/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * FDIR primitives — anomaly registry, event sink, PUS-5 drain, and
 * (slice fsw-14) occurrence counting, confirmation, and autonomous
 * safe-mode recovery. See migris/fsw/fdir/fdir.h for the contract.
 */

#include "migris/fsw/fdir/fdir.h"

#include "migris/fsw/event_sink.h"
#include "migris/fsw/fdir/event_fifo.h"
#include "migris/fsw/mode/mode.h"
#include "migris/fsw/pus/pus5.h"

#include <stddef.h>
#include <stdint.h>

/* Build one event record (field-by-field — no aggregate/designated
 * init, which would trip the GCC missing-field-initializers ↔
 * clang-tidy redundant-member-init two-linter conflict on the aux[]
 * tail) and enqueue it. Single internal path for both the generic
 * sink and the typed report_anomaly. */
static int fdir_enqueue(migris_fdir_ctx_t* ctx,
                        uint32_t now_seconds,
                        migris_pus5_severity_t severity,
                        uint16_t event_id,
                        const uint8_t* aux,
                        size_t aux_len) {
    if (ctx == NULL) {
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }
    if (aux_len > MIGRIS_FDIR_EVENT_AUX_MAX) {
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }

    migris_fdir_event_t ev;
    ev.event_id = event_id;
    ev.severity = (uint8_t)severity;
    ev.aux_len = (uint8_t)aux_len;
    for (size_t i = 0U; i < MIGRIS_FDIR_EVENT_AUX_MAX; ++i) {
        ev.aux[i] = (i < aux_len && aux != NULL) ? aux[i] : 0U;
    }
    ev.t_seconds = now_seconds;

    return migris_event_fifo_push(&ctx->fifo, &ev);
}

/* Map an event-definition ID back to its anomaly type. Returns 1 and
 * sets *out for a confirmable framework anomaly, 0 otherwise — a
 * non-anomaly event (the boot event, a mode change, a recovery report)
 * never feeds the confirmation counters. */
static int fdir_event_anomaly(uint16_t event_id, migris_fdir_anomaly_t* out) {
    if (event_id == MIGRIS_PUS5_EVT_TC_REJECTED) {
        *out = MIGRIS_FDIR_ANOM_TC_REJECTED;
        return 1;
    }
    if (event_id == MIGRIS_PUS5_EVT_RX_OVERFLOW) {
        *out = MIGRIS_FDIR_ANOM_RX_OVERFLOW;
        return 1;
    }
    return 0;
}

/* Account one occurrence of `anomaly` and, on the confirmation edge,
 * fire recovery: emit a high-severity FDIR_RECOVERY event and command
 * the safe mode. Called from both anomaly entry points — the typed
 * report_anomaly and the generic sink — so a TC_REJECTED counts the
 * same whichever API delivered it. Recovery fires once per anomaly per
 * boot (the confirmation latch). */
static void
fdir_observe(migris_fdir_ctx_t* ctx, migris_fdir_anomaly_t anomaly, uint32_t now_seconds) {
    const size_t i = (size_t)anomaly;
    if (ctx->occurrences[i] < 0xFFFFU) {
        ctx->occurrences[i] = (uint16_t)(ctx->occurrences[i] + 1U);
    }
    const int below = (ctx->thresholds[i] == 0U) || (ctx->occurrences[i] < ctx->thresholds[i]);
    if (below != 0 || ctx->confirmed[i] != 0U) {
        return;
    }
    ctx->confirmed[i] = 1U; /* latch — recovery fires once per anomaly per boot */
    if (ctx->recovery_enabled == 0 || ctx->mode == NULL) {
        return;
    }
    /* FDIR_RECOVERY aux: anomaly type, the commanded safe mode, the
     * occurrence count at confirmation (big-endian). Enqueued before
     * the mode request so the cause precedes the MODE_CHANGED effect on
     * the wire. */
    uint8_t aux[4];
    aux[0] = (uint8_t)anomaly;
    aux[1] = ctx->safe_mode;
    aux[2] = (uint8_t)((ctx->occurrences[i] >> 8) & 0xFFU);
    aux[3] = (uint8_t)(ctx->occurrences[i] & 0xFFU);
    (void)fdir_enqueue(
        ctx, now_seconds, MIGRIS_PUS5_SEV_HIGH, MIGRIS_PUS5_EVT_FDIR_RECOVERY, aux, sizeof aux);
    (void)migris_mode_request(ctx->mode, ctx->safe_mode, now_seconds);
}

static int fdir_sink_report(void* self,
                            uint32_t now_seconds,
                            migris_pus5_severity_t severity,
                            uint16_t event_id,
                            const uint8_t* aux,
                            size_t aux_len) {
    migris_fdir_ctx_t* ctx = (migris_fdir_ctx_t*)self;
    const int rc = fdir_enqueue(ctx, now_seconds, severity, event_id, aux, aux_len);
    /* A generic producer (the TC router) classifies the event itself;
     * map it back to an anomaly so a router-reported TC_REJECTED feeds
     * the confirmation counters exactly as the typed path does. */
    migris_fdir_anomaly_t anomaly = MIGRIS_FDIR_ANOM_TC_REJECTED;
    if (ctx != NULL && fdir_event_anomaly(event_id, &anomaly) != 0) {
        fdir_observe(ctx, anomaly, now_seconds);
    }
    return rc;
}

void migris_fdir_init(migris_fdir_ctx_t* ctx) {
    if (ctx == NULL) {
        return;
    }
    migris_event_fifo_init(&ctx->fifo);
    for (size_t i = 0U; i < MIGRIS_FDIR_ANOMALY_COUNT; ++i) {
        ctx->occurrences[i] = 0U;
        ctx->thresholds[i] = 0U;
        ctx->confirmed[i] = 0U;
    }
    ctx->mode = NULL;
    ctx->safe_mode = 0U;
    ctx->recovery_enabled = 0;
}

int migris_fdir_arm_recovery(migris_fdir_ctx_t* ctx,
                             migris_mode_manager_t* mode,
                             migris_mode_id_t safe_mode,
                             const migris_fdir_confirm_def_t* confirms,
                             size_t n_confirms) {
    if (ctx == NULL || (confirms == NULL && n_confirms > 0U)) {
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }
    for (size_t i = 0U; i < n_confirms; ++i) {
        if ((size_t)confirms[i].anomaly >= MIGRIS_FDIR_ANOMALY_COUNT) {
            return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
        }
    }
    /* Fresh accounting — a re-arm starts from a clean slate. */
    for (size_t i = 0U; i < MIGRIS_FDIR_ANOMALY_COUNT; ++i) {
        ctx->occurrences[i] = 0U;
        ctx->thresholds[i] = 0U;
        ctx->confirmed[i] = 0U;
    }
    for (size_t i = 0U; i < n_confirms; ++i) {
        ctx->thresholds[(size_t)confirms[i].anomaly] = confirms[i].threshold;
    }
    ctx->mode = mode;
    ctx->safe_mode = safe_mode;
    ctx->recovery_enabled = 1;
    return MIGRIS_EVENT_FIFO_OK;
}

void migris_fdir_set_enabled(migris_fdir_ctx_t* ctx, int enabled) {
    if (ctx == NULL) {
        return;
    }
    ctx->recovery_enabled = (enabled != 0) ? 1 : 0;
}

int migris_fdir_is_enabled(const migris_fdir_ctx_t* ctx) {
    return (ctx != NULL && ctx->recovery_enabled != 0) ? 1 : 0;
}

migris_event_sink_t migris_fdir_event_sink(migris_fdir_ctx_t* ctx) {
    migris_event_sink_t sink;
    sink.report = fdir_sink_report;
    sink.self = ctx;
    return sink;
}

int migris_fdir_report_anomaly(migris_fdir_ctx_t* ctx,
                               migris_fdir_anomaly_t anomaly,
                               uint32_t now_seconds,
                               uint32_t detail) {
    migris_pus5_severity_t severity = MIGRIS_PUS5_SEV_LOW;
    uint16_t event_id = 0U;
    uint8_t aux[4] = {0U, 0U, 0U, 0U};
    size_t aux_len = 0U;

    switch (anomaly) {
    case MIGRIS_FDIR_ANOM_TC_REJECTED:
        severity = MIGRIS_PUS5_SEV_LOW;
        event_id = MIGRIS_PUS5_EVT_TC_REJECTED;
        aux[0] = (uint8_t)((detail >> 16) & 0xFFU); /* PUS-1 failure code */
        aux[1] = (uint8_t)((detail >> 8) & 0xFFU);  /* service type      */
        aux[2] = (uint8_t)(detail & 0xFFU);         /* service subtype   */
        aux_len = 3U;
        break;
    case MIGRIS_FDIR_ANOM_RX_OVERFLOW:
        severity = MIGRIS_PUS5_SEV_MEDIUM;
        event_id = MIGRIS_PUS5_EVT_RX_OVERFLOW;
        aux[0] = (uint8_t)((detail >> 24) & 0xFFU); /* bytes dropped, big-endian */
        aux[1] = (uint8_t)((detail >> 16) & 0xFFU);
        aux[2] = (uint8_t)((detail >> 8) & 0xFFU);
        aux[3] = (uint8_t)(detail & 0xFFU);
        aux_len = 4U;
        break;
    default:
        return MIGRIS_EVENT_FIFO_ERR_BAD_ARG;
    }

    const int rc = fdir_enqueue(ctx, now_seconds, severity, event_id, aux, aux_len);
    if (ctx != NULL) {
        fdir_observe(ctx, anomaly, now_seconds);
    }
    return rc;
}

int migris_fdir_drain(migris_fdir_ctx_t* ctx,
                      uint16_t apid,
                      uint16_t* tm_seq_count,
                      migris_pus5_ctx_t* pus5_ctx,
                      uint8_t* out,
                      size_t out_cap) {
    if (ctx == NULL) {
        return 0;
    }
    /* Up-front capacity check (mirrors the TC router's contract): if
     * the buffer is too small we report it without popping, so the
     * event is preserved for a correctly-sized retry. */
    if (out_cap < MIGRIS_PUS5_TM_MAX_PACKET_SIZE) {
        return MIGRIS_PUS5_ERR_BUF_TOO_SMALL;
    }

    migris_fdir_event_t ev;
    if (migris_event_fifo_pop(&ctx->fifo, &ev) == 0) {
        return 0; /* nothing queued */
    }

    return migris_pus5_build_event_report(pus5_ctx,
                                          apid,
                                          tm_seq_count,
                                          ev.t_seconds,
                                          (migris_pus5_severity_t)ev.severity,
                                          ev.event_id,
                                          ev.aux,
                                          ev.aux_len,
                                          0U, /* spontaneous — no triggering TC */
                                          out,
                                          out_cap);
}
