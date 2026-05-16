// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/ccsds.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

TEST(Crc16CcittFalse, KnownVector123456789) {
    // Canonical CRC-16/CCITT-FALSE check vector (Catalogue of CRC
    // Routines): "123456789" -> 0x29B1.
    const std::array<std::uint8_t, 9> input = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(migris_crc16_ccitt_false(input.data(), input.size()), 0x29B1U);
}

TEST(Crc16CcittFalse, EmptyBufferGivesInitialValue) {
    // No input bytes consumed -> CRC remains at its initial value.
    EXPECT_EQ(migris_crc16_ccitt_false(nullptr, 0), 0xFFFFU);
}

TEST(CcsdsPrimary, RoundTripsTcAtApid0x100SeqCount0) {
    // Matches the byte-level example in docs/wire/pus-17.md.
    const migris_ccsds_primary_header_t in = {
        /*version=*/0,
        /*type=*/MIGRIS_CCSDS_PACKET_TYPE_TC,
        /*sec_hdr_flag=*/1,
        /*apid=*/0x100,
        /*seq_flags=*/MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        /*seq_count=*/0,
        /*data_length=*/6,
    };

    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()), MIGRIS_CCSDS_OK);

    // Byte-exact verification — the wire-format spec is the contract.
    EXPECT_EQ(buf[0], 0x19U);
    EXPECT_EQ(buf[1], 0x00U);
    EXPECT_EQ(buf[2], 0xC0U);
    EXPECT_EQ(buf[3], 0x00U);
    EXPECT_EQ(buf[4], 0x00U);
    EXPECT_EQ(buf[5], 0x06U);

    migris_ccsds_primary_header_t out{};
    ASSERT_EQ(migris_ccsds_primary_unpack(&out, buf.data(), buf.size()), MIGRIS_CCSDS_OK);
    EXPECT_EQ(out.version, in.version);
    EXPECT_EQ(out.type, in.type);
    EXPECT_EQ(out.sec_hdr_flag, in.sec_hdr_flag);
    EXPECT_EQ(out.apid, in.apid);
    EXPECT_EQ(out.seq_flags, in.seq_flags);
    EXPECT_EQ(out.seq_count, in.seq_count);
    EXPECT_EQ(out.data_length, in.data_length);
}

TEST(CcsdsPrimary, RoundTripsTmAtApid0x100SeqCount0) {
    const migris_ccsds_primary_header_t in = {
        /*version=*/0,
        /*type=*/MIGRIS_CCSDS_PACKET_TYPE_TM,
        /*sec_hdr_flag=*/1,
        /*apid=*/0x100,
        /*seq_flags=*/MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        /*seq_count=*/0,
        /*data_length=*/11,
    };

    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()), MIGRIS_CCSDS_OK);

    EXPECT_EQ(buf[0], 0x09U);
    EXPECT_EQ(buf[1], 0x00U);
    EXPECT_EQ(buf[2], 0xC0U);
    EXPECT_EQ(buf[3], 0x00U);
    EXPECT_EQ(buf[4], 0x00U);
    EXPECT_EQ(buf[5], 0x0BU);
}

TEST(CcsdsPrimary, RoundTripsMaxFieldValues) {
    const migris_ccsds_primary_header_t in = {
        /*version=*/7,
        /*type=*/1,
        /*sec_hdr_flag=*/1,
        /*apid=*/0x7FF,
        /*seq_flags=*/3,
        /*seq_count=*/0x3FFF,
        /*data_length=*/0xFFFF,
    };
    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()), MIGRIS_CCSDS_OK);

    migris_ccsds_primary_header_t out{};
    ASSERT_EQ(migris_ccsds_primary_unpack(&out, buf.data(), buf.size()), MIGRIS_CCSDS_OK);
    EXPECT_EQ(out.version, 7U);
    EXPECT_EQ(out.type, 1U);
    EXPECT_EQ(out.sec_hdr_flag, 1U);
    EXPECT_EQ(out.apid, 0x7FFU);
    EXPECT_EQ(out.seq_flags, 3U);
    EXPECT_EQ(out.seq_count, 0x3FFFU);
    EXPECT_EQ(out.data_length, 0xFFFFU);
}

TEST(CcsdsPrimary, RejectsTooShortBuffer) {
    const migris_ccsds_primary_header_t in = {0, 0, 0, 0, 0, 0, 0};
    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE - 1> buf{};
    EXPECT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()),
              MIGRIS_CCSDS_ERR_BUF_TOO_SMALL);

    migris_ccsds_primary_header_t out{};
    EXPECT_EQ(migris_ccsds_primary_unpack(&out, buf.data(), buf.size()),
              MIGRIS_CCSDS_ERR_BUF_TOO_SMALL);
}

TEST(CcsdsPrimary, RejectsOutOfRangeApid) {
    const migris_ccsds_primary_header_t in = {0, 0, 0, /*apid=*/0x800, 0, 0, 0};
    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE> buf{};
    EXPECT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()),
              MIGRIS_CCSDS_ERR_FIELD_OUT_OF_RANGE);
}

TEST(CcsdsPrimary, RejectsOutOfRangeSeqCount) {
    const migris_ccsds_primary_header_t in = {0, 0, 0, 0, 0, /*seq_count=*/0x4000, 0};
    std::array<std::uint8_t, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE> buf{};
    EXPECT_EQ(migris_ccsds_primary_pack(&in, buf.data(), buf.size()),
              MIGRIS_CCSDS_ERR_FIELD_OUT_OF_RANGE);
}

TEST(CcsdsPacketTotalSize, CountsHeaderPlusDataLengthPlusOne) {
    EXPECT_EQ(migris_ccsds_packet_total_size(6U), 13U);   // PUS-17[1] TC.
    EXPECT_EQ(migris_ccsds_packet_total_size(11U), 18U);  // PUS-17[2] TM.
}

}  // namespace
}  // namespace migris::fsw::pus::test
