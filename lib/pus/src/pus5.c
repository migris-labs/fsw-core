/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * PUS-5 (Event reporting) — report encoder.
 *
 * Pure serialiser: the caller decides an event has fired and supplies
 * its severity, event-definition ID and optional auxiliary data; this
 * file only turns that into a wire-format report. Source data is the
 * 2-byte big-endian event ID followed by the auxiliary bytes verbatim.
 * Wire format pinned in docs/wire/pus-5.md.
 */

#include "migris/fsw/pus/pus5.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <stddef.h>
#include <stdint.h>

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
                                   size_t out_cap) {
    /* Defensive depth for the freestanding C flight side: an unsigned
     * compare catches a corrupted / out-of-range severity (incl. a
     * negative one, which becomes a huge unsigned value) before it
     * indexes ctx->msg_counter[]. Not unit-tested from the C++ harness
     * — constructing an out-of-range unscoped enum there is
     * unspecified behaviour (see tests/pus5_test.cpp). */
    if ((unsigned int)severity > (unsigned int)MIGRIS_PUS5_SEV_HIGH) {
        return MIGRIS_PUS5_ERR_BAD_ARG;
    }
    if (aux_len > MIGRIS_PUS5_AUX_MAX_LEN) {
        return MIGRIS_PUS5_ERR_BAD_ARG;
    }
    if (aux == NULL && aux_len != 0U) {
        return MIGRIS_PUS5_ERR_BAD_ARG;
    }

    const uint8_t subtype = (uint8_t)((unsigned int)severity + 1U);
    const size_t msg_idx = (size_t)severity;

    /* data_length = (data field bytes) − 1. The data field is the TM
     * secondary header + event ID (2) + aux + the 2-byte CRC. */
    const uint16_t data_field = (uint16_t)(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE +
                                           MIGRIS_PUS5_EVENT_ID_SIZE + aux_len + 2U);
    const size_t packet_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + (size_t)data_field;

    if (out_cap < packet_size) {
        return MIGRIS_PUS5_ERR_BUF_TOO_SMALL;
    }

    const migris_ccsds_primary_header_t primary = {
        .version = 0U,
        .type = MIGRIS_CCSDS_PACKET_TYPE_TM,
        .sec_hdr_flag = 1U,
        .apid = apid,
        .seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        .seq_count = *tm_seq_count,
        .data_length = (uint16_t)(data_field - 1U),
    };
    if (migris_ccsds_primary_pack(&primary, out, out_cap) != MIGRIS_CCSDS_OK) {
        return MIGRIS_PUS5_ERR_BUF_TOO_SMALL;
    }

    const migris_pus_tm_secondary_header_t sec = {
        .pus_version = MIGRIS_PUS_VERSION_C,
        .sc_time_ref_status = 0U,
        .service_type = MIGRIS_PUS_SERVICE_EVENT_REPORTING,
        .service_subtype = subtype,
        .msg_counter = ctx->msg_counter[msg_idx],
        .destination_id = destination_id,
        .time_seconds = now_seconds,
    };
    if (migris_pus_tm_secondary_pack(&sec,
                                     &out[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                     out_cap - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) != 0) {
        return MIGRIS_PUS5_ERR_BUF_TOO_SMALL;
    }

    size_t off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    out[off++] = (uint8_t)(event_id >> 8);
    out[off++] = (uint8_t)(event_id & 0xFFU);
    for (size_t i = 0U; i < aux_len; ++i) {
        out[off++] = aux[i];
    }

    /* CRC-16-CCITT-FALSE over every byte before the trailing CRC. */
    const uint16_t crc = migris_crc16_ccitt_false(out, off);
    out[off] = (uint8_t)(crc >> 8);
    out[off + 1U] = (uint8_t)(crc & 0xFFU);

    /* Commit state only once the whole packet is built: advance the
     * shared per-APID CCSDS sequence count (mod 2^14) and this
     * severity's PUS message counter (mod 2^8). */
    *tm_seq_count = (uint16_t)((*tm_seq_count + 1U) & 0x3FFFU);
    ctx->msg_counter[msg_idx] = (uint8_t)(ctx->msg_counter[msg_idx] + 1U);

    return (int)packet_size;
}
