/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * FDIR primitives — anomaly registry + event sink + PUS-5 drain.
 * Detection + reporting only this slice; see migris/fsw/fdir/fdir.h
 * for the contract and the deferred Isolation/Recovery scope.
 */

#include "migris/fsw/fdir/fdir.h"

#include "migris/fsw/event_sink.h"
#include "migris/fsw/fdir/event_fifo.h"
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

static int fdir_sink_report(void* self,
                            uint32_t now_seconds,
                            migris_pus5_severity_t severity,
                            uint16_t event_id,
                            const uint8_t* aux,
                            size_t aux_len) {
    return fdir_enqueue((migris_fdir_ctx_t*)self, now_seconds, severity, event_id, aux, aux_len);
}

void migris_fdir_init(migris_fdir_ctx_t* ctx) {
    if (ctx == NULL) {
        return;
    }
    migris_event_fifo_init(&ctx->fifo);
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

    return fdir_enqueue(ctx, now_seconds, severity, event_id, aux, aux_len);
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
