/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-5 — Event reporting service. Slice fsw-6 ships the four
 * asynchronous event-report TM subtypes:
 *
 *   * Subtype [1] (TM) → "Informative event report".
 *   * Subtype [2] (TM) → "Low-severity anomaly report".
 *   * Subtype [3] (TM) → "Medium-severity anomaly report".
 *   * Subtype [4] (TM) → "High-severity anomaly report".
 *
 * The control subtypes [5]/[6] (enable/disable event generation),
 * [7] (report disabled events) and [8] (disabled-events list) are
 * deliberately *not* in this slice: TC-driven event reconfiguration
 * overlaps PUS-20 (onboard parameter management, P1). They land if a
 * concrete operational need appears — see workspace CLAUDE.md and
 * CHANGELOG.md.
 *
 * Unlike PUS-1 and PUS-17, PUS-5 is the framework's first
 * *asynchronous* service: an event report is emitted spontaneously at
 * the point a condition is detected, not as a side effect of an
 * inbound TC. This header is a pure report *encoder* — the caller
 * decides when an event fires, owns the event identity, and owns the
 * output buffer; here we only serialise one report. There is
 * deliberately no event queue: PUS-5 stays the smallest viable
 * surface (the proven pus1/pus17 shape). A freestanding bounded event
 * FIFO is the explicit *next* abstraction, earned when a producer
 * that does not own a TM buffer first exists (an FDIR monitor, PUS-3
 * housekeeping, or the ISR-context UART RX-ring overflow event). Until
 * then it would be an abstraction with a single straight-line caller.
 * Freestanding C — no Zephyr, no malloc, no stdlib.
 *
 * Event-definition IDs are a frozen cross-repo contract. The range
 * 0x0001..0x00FF is reserved for fsw-core *framework* events; mission
 * flight software (cry4-fsw) owns 0x0100 and above (the exact
 * mission-side scheme is pinned when cry4-fsw bootstraps). This
 * mirrors the pinned "PUS-128+ vendor assignments live in cry4 /
 * cry4-fsw, not in fsw-core" decision. Only the framework IDs this
 * slice actually emits are defined below (smallest viable surface,
 * like pus1.h deferring its unused subtypes).
 *
 * Byte-level specification: docs/wire/pus-5.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS5_H_
#define MIGRIS_FSW_PUS_PUS5_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_EVENT_REPORTING 5U

#define MIGRIS_PUS5_SUBTYPE_INFO 1U
#define MIGRIS_PUS5_SUBTYPE_LOW 2U
#define MIGRIS_PUS5_SUBTYPE_MEDIUM 3U
#define MIGRIS_PUS5_SUBTYPE_HIGH 4U

/** Event-definition ID = a 2-byte big-endian identifier (the ECSS
 *  RID), the first field of every event report's source data. */
#define MIGRIS_PUS5_EVENT_ID_SIZE 2U

/** Maximum auxiliary-data length appended after the event ID. Frozen
 *  contract: widening this later is non-breaking, narrowing it is a
 *  breaking change (see docs/wire/pus-5.md). 32 bytes is generous for
 *  framework events (most carry 0–4) while keeping the largest PUS-5
 *  packet small. */
#define MIGRIS_PUS5_AUX_MAX_LEN 32U

/** Largest PUS-5 packet: primary (6) + TM sec header (10) + event ID
 *  (2) + max aux (32) + CRC (2). A bare event (no aux) is 20 bytes. */
#define MIGRIS_PUS5_TM_MAX_PACKET_SIZE 52U

/** fsw-core framework event-definition IDs (reserved block
 *  0x0001..0x00FF). Mission events live at 0x0100+ and are not
 *  defined here. */
#define MIGRIS_PUS5_EVT_FSW_BOOT 0x0001U

/** Event severity. The wire subtype is ``severity + 1`` (INFO→[1],
 *  LOW→[2], MEDIUM→[3], HIGH→[4]); the enum value is also the index
 *  into the per-severity message-counter array in
 *  ``migris_pus5_ctx_t``. */
typedef enum {
    MIGRIS_PUS5_SEV_INFO = 0,   /**< Informative — subtype [1]. */
    MIGRIS_PUS5_SEV_LOW = 1,    /**< Low-severity anomaly — subtype [2]. */
    MIGRIS_PUS5_SEV_MEDIUM = 2, /**< Medium-severity anomaly — subtype [3]. */
    MIGRIS_PUS5_SEV_HIGH = 3    /**< High-severity anomaly — subtype [4]. */
} migris_pus5_severity_t;

/** PUS-5 encoder return / error codes. Same convention as the rest of
 *  the codec: a positive value is the byte count written, a negative
 *  value is one of these. */
typedef enum {
    MIGRIS_PUS5_OK = 0,
    MIGRIS_PUS5_ERR_BUF_TOO_SMALL = -1, /**< Output buffer < required packet size. */
    MIGRIS_PUS5_ERR_BAD_ARG = -2        /**< Bad severity, aux over max, or NULL aux w/ len. */
} migris_pus5_status_t;

/** Per-severity message counters for the four PUS-5 report subtypes.
 *  Index order matches ``migris_pus5_severity_t``: [0]=info (1),
 *  [1]=low (2), [2]=medium (3), [3]=high (4). Caller-owned;
 *  zero-initialised once at startup. The CCSDS TM sequence count is
 *  *not* here — it is shared per-APID across every service and is
 *  threaded in by pointer (see ``tm_seq_count``). */
typedef struct {
    uint8_t msg_counter[4];
} migris_pus5_ctx_t;

/** Encode one PUS-5 event report into ``out``.
 *
 *  ``severity`` selects the subtype (``severity + 1``) and the
 *  ``ctx`` message counter. ``event_id`` is the 2-byte big-endian
 *  event-definition ID. ``aux`` / ``aux_len`` is optional
 *  event-specific data appended verbatim after the event ID;
 *  ``aux_len`` must be ``<= MIGRIS_PUS5_AUX_MAX_LEN``. A bare event
 *  is legal: pass ``aux == NULL`` with ``aux_len == 0`` (a non-NULL
 *  ``aux`` with ``aux_len == 0`` is also fine). ``destination_id`` is
 *  ``0`` for a spontaneous event (no triggering TC). ``tm_seq_count``
 *  is the shared per-APID CCSDS TM sequence count: read into the
 *  packet, then advanced mod 2^14.
 *
 *  Returns the positive byte count written (20 ..
 *  ``MIGRIS_PUS5_TM_MAX_PACKET_SIZE``) on success, or a negative
 *  ``migris_pus5_status_t``. Side effects (the shared sequence count
 *  and the relevant ``ctx`` message counter) advance on success only. */
int migris_pus5_build_event_report(migris_pus5_ctx_t* ctx,
                                   uint16_t apid,
                                   uint16_t* tm_seq_count,
                                   uint32_t now_seconds,
                                   migris_pus5_severity_t severity,
                                   uint16_t event_id,
                                   const uint8_t* aux,
                                   size_t aux_len,
                                   uint16_t destination_id,
                                   uint8_t* out,
                                   size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS5_H_
