/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-C telecommand secondary header pack/unpack.
 */

#include "migris/fsw/pus/pus_tc.h"

#include <stddef.h>
#include <stdint.h>

int migris_pus_tc_secondary_pack(const migris_pus_tc_secondary_header_t* hdr,
                                 uint8_t* out,
                                 size_t out_len) {
    if (out_len < MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE) {
        return -1;
    }
    if (hdr->pus_version > 0xFU || hdr->ack_flags > 0xFU) {
        return -2;
    }

    out[0] = (uint8_t)(((hdr->pus_version & 0xFU) << 4) | (hdr->ack_flags & 0xFU));
    out[1] = hdr->service_type;
    out[2] = hdr->service_subtype;
    out[3] = (uint8_t)(hdr->source_id >> 8);
    out[4] = (uint8_t)(hdr->source_id & 0xFFU);
    return 0;
}

int migris_pus_tc_secondary_unpack(migris_pus_tc_secondary_header_t* hdr,
                                   const uint8_t* in,
                                   size_t in_len) {
    if (in_len < MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE) {
        return -1;
    }

    hdr->pus_version = (uint8_t)((in[0] >> 4) & 0xFU);
    hdr->ack_flags = (uint8_t)(in[0] & 0xFU);
    hdr->service_type = in[1];
    hdr->service_subtype = in[2];
    hdr->source_id = (uint16_t)(((uint16_t)in[3] << 8) | (uint16_t)in[4]);
    return 0;
}
