// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

TEST(PusTcSecondary, RoundTripsPus17_1) {
    const migris_pus_tc_secondary_header_t in = {
        /*pus_version=*/MIGRIS_PUS_VERSION_C,
        /*ack_flags=*/0,
        /*service_type=*/17,
        /*service_subtype=*/1,
        /*source_id=*/0,
    };
    std::array<std::uint8_t, MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_pus_tc_secondary_pack(&in, buf.data(), buf.size()), 0);

    EXPECT_EQ(buf[0], 0x20U);
    EXPECT_EQ(buf[1], 0x11U);
    EXPECT_EQ(buf[2], 0x01U);
    EXPECT_EQ(buf[3], 0x00U);
    EXPECT_EQ(buf[4], 0x00U);

    migris_pus_tc_secondary_header_t out{};
    ASSERT_EQ(migris_pus_tc_secondary_unpack(&out, buf.data(), buf.size()), 0);
    EXPECT_EQ(out.pus_version, in.pus_version);
    EXPECT_EQ(out.ack_flags, in.ack_flags);
    EXPECT_EQ(out.service_type, in.service_type);
    EXPECT_EQ(out.service_subtype, in.service_subtype);
    EXPECT_EQ(out.source_id, in.source_id);
}

TEST(PusTcSecondary, EncodesAllAckFlagsSet) {
    const migris_pus_tc_secondary_header_t in = {
        /*pus_version=*/MIGRIS_PUS_VERSION_C,
        /*ack_flags=*/MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_START |
            MIGRIS_PUS_TC_ACK_PROGRESS | MIGRIS_PUS_TC_ACK_COMPLETION,
        /*service_type=*/17,
        /*service_subtype=*/1,
        /*source_id=*/0xCAFE,
    };
    std::array<std::uint8_t, MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_pus_tc_secondary_pack(&in, buf.data(), buf.size()), 0);
    EXPECT_EQ(buf[0], 0x2FU);
    EXPECT_EQ(buf[3], 0xCAU);
    EXPECT_EQ(buf[4], 0xFEU);
}

TEST(PusTcSecondary, RejectsTooShortBuffer) {
    const migris_pus_tc_secondary_header_t in = {MIGRIS_PUS_VERSION_C, 0, 17, 1, 0};
    std::array<std::uint8_t, MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE - 1> buf{};
    EXPECT_LT(migris_pus_tc_secondary_pack(&in, buf.data(), buf.size()), 0);

    migris_pus_tc_secondary_header_t out{};
    EXPECT_LT(migris_pus_tc_secondary_unpack(&out, buf.data(), buf.size()), 0);
}

TEST(PusTmSecondary, RoundTripsPus17_2) {
    const migris_pus_tm_secondary_header_t in = {
        /*pus_version=*/MIGRIS_PUS_VERSION_C,
        /*sc_time_ref_status=*/0,
        /*service_type=*/17,
        /*service_subtype=*/2,
        /*msg_counter=*/0,
        /*destination_id=*/0xBEEF,
        /*time_seconds=*/0x01020304,
    };
    std::array<std::uint8_t, MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE> buf{};
    ASSERT_EQ(migris_pus_tm_secondary_pack(&in, buf.data(), buf.size()), 0);

    EXPECT_EQ(buf[0], 0x20U);
    EXPECT_EQ(buf[1], 0x11U);
    EXPECT_EQ(buf[2], 0x02U);
    EXPECT_EQ(buf[3], 0x00U);
    EXPECT_EQ(buf[4], 0xBEU);
    EXPECT_EQ(buf[5], 0xEFU);
    EXPECT_EQ(buf[6], 0x01U);
    EXPECT_EQ(buf[7], 0x02U);
    EXPECT_EQ(buf[8], 0x03U);
    EXPECT_EQ(buf[9], 0x04U);

    migris_pus_tm_secondary_header_t out{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&out, buf.data(), buf.size()), 0);
    EXPECT_EQ(out.pus_version, in.pus_version);
    EXPECT_EQ(out.sc_time_ref_status, in.sc_time_ref_status);
    EXPECT_EQ(out.service_type, in.service_type);
    EXPECT_EQ(out.service_subtype, in.service_subtype);
    EXPECT_EQ(out.msg_counter, in.msg_counter);
    EXPECT_EQ(out.destination_id, in.destination_id);
    EXPECT_EQ(out.time_seconds, in.time_seconds);
}

TEST(PusTmSecondary, RejectsTooShortBuffer) {
    const migris_pus_tm_secondary_header_t in = {
        MIGRIS_PUS_VERSION_C,
        0,
        17,
        2,
        0,
        0,
        0,
    };
    std::array<std::uint8_t, MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE - 1> buf{};
    EXPECT_LT(migris_pus_tm_secondary_pack(&in, buf.data(), buf.size()), 0);

    migris_pus_tm_secondary_header_t out{};
    EXPECT_LT(migris_pus_tm_secondary_unpack(&out, buf.data(), buf.size()), 0);
}

}  // namespace
}  // namespace migris::fsw::pus::test
