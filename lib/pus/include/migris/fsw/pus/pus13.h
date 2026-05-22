/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-13 — Large data transfer (downlink). Slice fsw-12 ships a
 * pragmatic downlink-only subset of ECSS-E-ST-70-41C service 13: a
 * data unit larger than one CCSDS Space Packet is downlinked as an
 * ordered sequence of telemetry "part" packets the ground reassembles.
 *
 *   * Subtype [1] (TM) → first downlink part.
 *   * Subtype [2] (TM) → intermediate downlink part.
 *   * Subtype [3] (TM) → last downlink part.
 *
 * PUS-13 is telemetry-only this slice: it has no inbound subtype, so
 * the TC router does not route service 13 (a service-13 TC is rejected
 * at acceptance with UNKNOWN_SERVICE). The downlink is driven by the
 * stateful large-data session in lib/largedata/, which slices a
 * borrowed data unit into parts and emits one per call; this header is
 * the stateless part-packet codec it builds on.
 *
 * Every part carries a 6-byte part header — transaction identifier,
 * 0-based part number, total part count — which is the authoritative
 * reassembly key. The uplink direction, the [13,16] downlink-abort
 * report, and concurrent transactions are deliberately out of this
 * slice (see CHANGELOG.md).
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. Byte-level
 * specification: docs/wire/pus-13.md.
 */

#ifndef MIGRIS_FSW_PUS_PUS13_H_
#define MIGRIS_FSW_PUS_PUS13_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIGRIS_PUS_SERVICE_LARGE_DATA 13U

#define MIGRIS_PUS13_SUBTYPE_FIRST_PART 1U        /**< TM, first downlink part. */
#define MIGRIS_PUS13_SUBTYPE_INTERMEDIATE_PART 2U /**< TM, intermediate downlink part. */
#define MIGRIS_PUS13_SUBTYPE_LAST_PART 3U         /**< TM, last downlink part. */

/** Largest payload, in bytes, one downlink part carries — the data
 *  unit is sliced into chunks of this size, the last chunk possibly
 *  shorter. A sender-side chunking parameter: the receiver derives
 *  each part's payload length from its CCSDS data length and need not
 *  know this value. Compile-time constant; override with
 *  ``-DMIGRIS_PUS13_PART_SIZE=<n>``. Keep ``24 + MIGRIS_PUS13_PART_SIZE``
 *  at or below 128 so a part packet fits the framework's telemetry
 *  buffers. */
#ifndef MIGRIS_PUS13_PART_SIZE
#    define MIGRIS_PUS13_PART_SIZE 64U
#endif

/** Size of the PUS-13 part header on the wire: transaction id (2) +
 *  part number (2) + total parts (2). It prefixes the payload in the
 *  source data of every [13,1] / [13,2] / [13,3] part. */
#define MIGRIS_PUS13_PART_HEADER_SIZE 6U

/** Largest [13,1/2/3] part packet on the wire: primary header (6) +
 *  PUS-C TM secondary header (10) + part header (6) + payload (up to
 *  MIGRIS_PUS13_PART_SIZE) + CRC (2). */
#define MIGRIS_PUS13_PART_PACKET_MAX (24U + MIGRIS_PUS13_PART_SIZE)

/** PUS-13 codec return / error codes. Same convention as the rest of
 *  the framework: a positive value is the byte count written, a
 *  negative value is one of these. */
typedef enum {
    MIGRIS_PUS13_OK = 0,
    MIGRIS_PUS13_ERR_BUF_TOO_SMALL = -1, /**< Output buffer below the part packet. */
    MIGRIS_PUS13_ERR_BAD_ARG = -2 /**< NULL argument, bad payload length, or bad part index. */
} migris_pus13_status_t;

/** Per-(service, subtype) message counters for the three PUS-13 part
 *  subtypes this slice emits: index [0] = [13,1], [1] = [13,2],
 *  [2] = [13,3]. Caller-owned; zero-initialised once at startup and
 *  then advanced monotonically — it must outlive any single transfer
 *  so the counters stay monotonic across transfers. */
typedef struct {
    uint8_t msg_counter[3];
} migris_pus13_ctx_t;

/** Encode one large-data downlink part into ``out``.
 *
 *  ``part_number`` is 0-based and ``total_parts`` (>= 1) is the part
 *  count of the whole transfer; with ``transaction_id`` they form the
 *  6-byte part header. The service subtype is derived from the part's
 *  position: index 0 of a multi-part transfer is [13,1], index
 *  ``total_parts - 1`` is [13,3], anything between is [13,2] — and a
 *  single-part transfer is one [13,3]. ``payload`` / ``payload_len``
 *  is the slice of the data unit this part carries: 1 ..
 *  ``MIGRIS_PUS13_PART_SIZE`` bytes.
 *
 *  Returns the positive part-packet byte count on success, or a
 *  negative ``migris_pus13_status_t``: ``ERR_BAD_ARG`` on a NULL
 *  argument, an empty or oversized payload, ``total_parts`` of 0, or
 *  ``part_number`` not below ``total_parts``; ``ERR_BUF_TOO_SMALL`` if
 *  ``out_cap`` is below the encoded packet. ``tm_seq_count`` is the
 *  shared per-APID CCSDS TM sequence count: read into the packet then
 *  advanced mod 2^14. ``destination_id`` echoes the triggering TC's
 *  source ID, or 0 for a spontaneous downlink. On any failure nothing
 *  is written to ``out`` and neither counter advances. */
int migris_pus13_build_part(migris_pus13_ctx_t* ctx,
                            uint16_t apid,
                            uint16_t* tm_seq_count,
                            uint32_t now_seconds,
                            uint16_t destination_id,
                            uint16_t transaction_id,
                            uint16_t part_number,
                            uint16_t total_parts,
                            const uint8_t* payload,
                            size_t payload_len,
                            uint8_t* out,
                            size_t out_cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS13_H_
