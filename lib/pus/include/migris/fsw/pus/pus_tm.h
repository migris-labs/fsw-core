/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-C (ECSS-E-ST-70-41C) telemetry secondary header.
 *
 * Slice fsw-4 ships the smallest viable PUS-C TM secondary header that
 * is still wire-format complete: PUS version, spacecraft time
 * reference status, service type/subtype, message counter,
 * destination ID, and a 4-byte CUC coarse-seconds time field. Total
 * size is fixed at 10 bytes; see docs/wire/pus-17.md.
 *
 * The time field epoch is *boot* today and becomes mission-config
 * once PUS-9-equivalent time correlation lands. The wire shape stays.
 */

#ifndef MIGRIS_FSW_PUS_PUS_TM_H_
#define MIGRIS_FSW_PUS_PUS_TM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the PUS-C TM secondary header on the wire. */
#define MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE 10U

typedef struct {
    uint8_t pus_version;         /**< 4 bits, ``2`` (PUS-C). */
    uint8_t sc_time_ref_status;  /**< 4 bits, ``0`` while time correlation is undefined. */
    uint8_t service_type;        /**< 8 bits. */
    uint8_t service_subtype;     /**< 8 bits. */
    uint8_t msg_counter;         /**< 8 bits, monotonic per service+subtype. */
    uint16_t destination_id;     /**< 16 bits, echoes the source ID of the triggering TC. */
    uint32_t time_seconds;       /**< 32 bits, CUC coarse seconds, big-endian on the wire. */
} migris_pus_tm_secondary_header_t;

int migris_pus_tm_secondary_pack(const migris_pus_tm_secondary_header_t *hdr,
                                 uint8_t *out, size_t out_len);

int migris_pus_tm_secondary_unpack(migris_pus_tm_secondary_header_t *hdr,
                                   const uint8_t *in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS_TM_H_
