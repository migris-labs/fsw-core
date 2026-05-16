/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * CCSDS Space Packet Protocol (CCSDS 133.0-B-2) — primary header and
 * the packet error control CRC used across every PUS service in the
 * Migris flight-software framework.
 *
 * Freestanding C: no Zephyr deps, no malloc, no stdlib. The same
 * translation units compile on Cortex-M7 (Zephyr) and on the host
 * toolchain (linked into the migris::fsw-core library so the bytes
 * we ship to space get exercised under ASan/UBSan/clang-tidy on
 * every PR).
 *
 * Byte-level specification: docs/wire/pus-17.md.
 */

#ifndef MIGRIS_FSW_PUS_CCSDS_H_
#define MIGRIS_FSW_PUS_CCSDS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the CCSDS Space Packet primary header on the wire. */
#define MIGRIS_CCSDS_PRIMARY_HEADER_SIZE 6U

/** Packet Type bit values (CCSDS 133.0-B-2 §4.1.3.3.2). */
#define MIGRIS_CCSDS_PACKET_TYPE_TM 0U
#define MIGRIS_CCSDS_PACKET_TYPE_TC 1U

/** Sequence Flags values (CCSDS 133.0-B-2 §4.1.3.4.2). We always use
 *  UNSEGMENTED — slice fsw-4 has no need for segmented packets. */
#define MIGRIS_CCSDS_SEQ_FLAGS_CONTINUATION 0U
#define MIGRIS_CCSDS_SEQ_FLAGS_FIRST 1U
#define MIGRIS_CCSDS_SEQ_FLAGS_LAST 2U
#define MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED 3U

/** Decoded CCSDS Space Packet primary header. Fields below are the
 *  numeric values of the bit-fields on the wire (not raw bit
 *  patterns) — i.e. ``type == 1`` means TC, not ``type == 0x10``. */
typedef struct {
    uint8_t version;      /**< 3 bits, ``0`` for every packet we emit. */
    uint8_t type;         /**< 1 bit, ``0``=TM, ``1``=TC. */
    uint8_t sec_hdr_flag; /**< 1 bit, ``1`` for PUS packets. */
    uint16_t apid;        /**< 11 bits. */
    uint8_t seq_flags;    /**< 2 bits, ``3`` (UNSEGMENTED) for our packets. */
    uint16_t seq_count;   /**< 14 bits, per-direction monotonic count. */
    uint16_t data_length; /**< 16 bits, ``(bytes in data field) − 1``. */
} migris_ccsds_primary_header_t;

/** Error codes returned by the codec functions in this header and the
 *  PUS-secondary-header / PUS-17 headers. Same convention everywhere:
 *  ``0`` on success, a negative value on failure. */
typedef enum {
    MIGRIS_CCSDS_OK = 0,
    MIGRIS_CCSDS_ERR_BUF_TOO_SMALL = -1,
    MIGRIS_CCSDS_ERR_FIELD_OUT_OF_RANGE = -2
} migris_ccsds_status_t;

/** Encode ``hdr`` into the first ``MIGRIS_CCSDS_PRIMARY_HEADER_SIZE``
 *  bytes of ``out``. ``out_len`` is the size of the output buffer.
 *  Returns 0 on success, a negative ``migris_ccsds_status_t`` on
 *  failure. Fails fast if any field is out of range — the caller is
 *  responsible for keeping ``apid`` < 2^11, ``seq_count`` < 2^14, etc. */
int migris_ccsds_primary_pack(const migris_ccsds_primary_header_t* hdr,
                              uint8_t* out,
                              size_t out_len);

/** Decode the first ``MIGRIS_CCSDS_PRIMARY_HEADER_SIZE`` bytes of
 *  ``in`` into ``hdr``. ``in_len`` is the size of the input buffer.
 *  Returns 0 on success, a negative ``migris_ccsds_status_t`` on
 *  failure. Does not validate semantics (caller checks APID match,
 *  packet type, etc.). */
int migris_ccsds_primary_unpack(migris_ccsds_primary_header_t* hdr,
                                const uint8_t* in,
                                size_t in_len);

/** Total CCSDS Space Packet length implied by ``data_length``: the
 *  6-byte primary header plus ``data_length + 1`` bytes of data
 *  field. Returns ``size_t`` so it composes cleanly with buffer
 *  capacity checks. */
static inline size_t migris_ccsds_packet_total_size(uint16_t data_length) {
    /* Arithmetic stays in unsigned int (data_length promotes to int and
     * the two unsigned-int operands force unsigned wraparound semantics);
     * the return is a widening conversion to size_t, no narrowing.
     * Avoids C-style casts in this header so it stays warning-clean
     * when consumed by C++ host code under `-Wold-style-cast`. */
    return MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + data_length + 1U;
}

/** CRC-16-CCITT-FALSE.
 *
 *  Polynomial ``0x1021``, initial value ``0xFFFF``, no input or
 *  output reflection, no output XOR. Standard known-good vector:
 *  the ASCII string ``"123456789"`` (9 bytes) yields ``0x29B1``.
 *
 *  This is the *Packet Error Control* CRC used at the tail of every
 *  CCSDS Space Packet on the Migris on-board interface. The wire
 *  encoding is big-endian (high byte first).
 *
 *  Bit-by-bit implementation: ~100 cycles per byte on Cortex-M7,
 *  trivial for typical PUS packet sizes (<100 bytes) and avoids the
 *  512-byte lookup table's ROM cost. Revisit if a hot path arrives.
 */
uint16_t migris_crc16_ccitt_false(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PUS_CCSDS_H_
