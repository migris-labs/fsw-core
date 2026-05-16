/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * CCSDS Space Packet primary header pack/unpack and CRC-16-CCITT-FALSE.
 */

#include "migris/fsw/pus/ccsds.h"

#include <stddef.h>
#include <stdint.h>

/* Big-endian byte helpers. Explicit byte-by-byte so host endianness
 * is irrelevant — the wire is big-endian by spec. */

static inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int migris_ccsds_primary_pack(const migris_ccsds_primary_header_t* hdr,
                              uint8_t* out,
                              size_t out_len) {
    if (out_len < MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) {
        return MIGRIS_CCSDS_ERR_BUF_TOO_SMALL;
    }
    if (hdr->version > 0x7U || hdr->type > 0x1U || hdr->sec_hdr_flag > 0x1U || hdr->apid > 0x7FFU ||
        hdr->seq_flags > 0x3U || hdr->seq_count > 0x3FFFU) {
        return MIGRIS_CCSDS_ERR_FIELD_OUT_OF_RANGE;
    }

    /* Bytes 0..1: ver (3) | type (1) | sec_hdr (1) | apid (11). */
    const uint16_t word0 = (uint16_t)((uint16_t)(hdr->version & 0x7U) << 13) |
                           (uint16_t)((uint16_t)(hdr->type & 0x1U) << 12) |
                           (uint16_t)((uint16_t)(hdr->sec_hdr_flag & 0x1U) << 11) |
                           (uint16_t)(hdr->apid & 0x7FFU);

    /* Bytes 2..3: seq_flags (2) | seq_count (14). */
    const uint16_t word1 =
        (uint16_t)((uint16_t)(hdr->seq_flags & 0x3U) << 14) | (uint16_t)(hdr->seq_count & 0x3FFFU);

    write_be16(&out[0], word0);
    write_be16(&out[2], word1);
    write_be16(&out[4], hdr->data_length);
    return MIGRIS_CCSDS_OK;
}

int migris_ccsds_primary_unpack(migris_ccsds_primary_header_t* hdr,
                                const uint8_t* in,
                                size_t in_len) {
    if (in_len < MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) {
        return MIGRIS_CCSDS_ERR_BUF_TOO_SMALL;
    }

    const uint16_t word0 = read_be16(&in[0]);
    const uint16_t word1 = read_be16(&in[2]);

    hdr->version = (uint8_t)((word0 >> 13) & 0x7U);
    hdr->type = (uint8_t)((word0 >> 12) & 0x1U);
    hdr->sec_hdr_flag = (uint8_t)((word0 >> 11) & 0x1U);
    hdr->apid = (uint16_t)(word0 & 0x7FFU);
    hdr->seq_flags = (uint8_t)((word1 >> 14) & 0x3U);
    hdr->seq_count = (uint16_t)(word1 & 0x3FFFU);
    hdr->data_length = read_be16(&in[4]);
    return MIGRIS_CCSDS_OK;
}

uint16_t migris_crc16_ccitt_false(const uint8_t* buf, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (unsigned b = 0; b < 8U; ++b) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
