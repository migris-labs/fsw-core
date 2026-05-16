/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-C telemetry secondary header pack/unpack.
 */

#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

int migris_pus_tm_secondary_pack(const migris_pus_tm_secondary_header_t* hdr,
                                 uint8_t* out,
                                 size_t out_len) {
    if (out_len < MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE) {
        return -1;
    }
    if (hdr->pus_version > 0xFU || hdr->sc_time_ref_status > 0xFU) {
        return -2;
    }

    out[0] = (uint8_t)(((hdr->pus_version & 0xFU) << 4) | (hdr->sc_time_ref_status & 0xFU));
    out[1] = hdr->service_type;
    out[2] = hdr->service_subtype;
    out[3] = hdr->msg_counter;
    out[4] = (uint8_t)(hdr->destination_id >> 8);
    out[5] = (uint8_t)(hdr->destination_id & 0xFFU);
    out[6] = (uint8_t)(hdr->time_seconds >> 24);
    out[7] = (uint8_t)((hdr->time_seconds >> 16) & 0xFFU);
    out[8] = (uint8_t)((hdr->time_seconds >> 8) & 0xFFU);
    out[9] = (uint8_t)(hdr->time_seconds & 0xFFU);
    return 0;
}

int migris_pus_tm_secondary_unpack(migris_pus_tm_secondary_header_t* hdr,
                                   const uint8_t* in,
                                   size_t in_len) {
    if (in_len < MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE) {
        return -1;
    }

    hdr->pus_version = (uint8_t)((in[0] >> 4) & 0xFU);
    hdr->sc_time_ref_status = (uint8_t)(in[0] & 0xFU);
    hdr->service_type = in[1];
    hdr->service_subtype = in[2];
    hdr->msg_counter = in[3];
    hdr->destination_id = (uint16_t)(((uint16_t)in[4] << 8) | (uint16_t)in[5]);
    hdr->time_seconds = ((uint32_t)in[6] << 24) | ((uint32_t)in[7] << 16) | ((uint32_t)in[8] << 8) |
                        (uint32_t)in[9];
    return 0;
}
