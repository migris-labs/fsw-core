/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-C (ECSS-E-ST-70-41C) telecommand secondary header.
 *
 * Migris ships the smallest viable PUS-C TC secondary header: PUS
 * version, ack flags, service type/subtype, source ID. No spare
 * fields, no message authentication. Length is fixed at 5 bytes —
 * see docs/wire/pus-17.md for the byte layout.
 */

#ifndef MIGRIS_FSW_PUS_PUS_TC_H_
#define MIGRIS_FSW_PUS_PUS_TC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the PUS-C TC secondary header on the wire. */
#define MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE 5U

/** PUS-C version field nibble — section 7.4.4.1 of ECSS-E-ST-70-41C. */
#define MIGRIS_PUS_VERSION_C 2U

/** Ack flag bit values. OR them into ``ack_flags`` to request the
 *  corresponding PUS-1 verification report. Slice fsw-4 leaves
 *  these unset (PUS-1 service not yet implemented). */
#define MIGRIS_PUS_TC_ACK_ACCEPTANCE 0x01U
#define MIGRIS_PUS_TC_ACK_START      0x02U
#define MIGRIS_PUS_TC_ACK_PROGRESS   0x04U
#define MIGRIS_PUS_TC_ACK_COMPLETION 0x08U

typedef struct {
    uint8_t pus_version;     /**< 4 bits, ``2`` (PUS-C). */
    uint8_t ack_flags;       /**< 4 bits. */
    uint8_t service_type;    /**< 8 bits. */
    uint8_t service_subtype; /**< 8 bits. */
    uint16_t source_id;      /**< 16 bits, operator-assigned. */
} migris_pus_tc_secondary_header_t;

int migris_pus_tc_secondary_pack(const migris_pus_tc_secondary_header_t *hdr,
                                 uint8_t *out, size_t out_len);

int migris_pus_tc_secondary_unpack(migris_pus_tc_secondary_header_t *hdr,
                                   const uint8_t *in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_PUS_TC_H_
