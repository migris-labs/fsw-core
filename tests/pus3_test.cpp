// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus3.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// Source-data offset: primary (6) + PUS-C TM secondary (10).
constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

// Frozen framework-structure field offsets within the source data
// (docs/wire/pus-3.md). Named so a layout regression is a one-line
// diff, not an arithmetic hunt.
constexpr std::size_t off_sid = 0U;
constexpr std::size_t off_uptime = 2U;
constexpr std::size_t off_seq = 6U;
constexpr std::size_t off_pus1 = 8U;
constexpr std::size_t off_pus5 = 12U;
constexpr std::size_t off_pus17 = 16U;
constexpr std::size_t off_accepted = 17U;
constexpr std::size_t off_rejected = 21U;
constexpr std::size_t off_drops = 25U;

migris_ccsds_primary_header_t primary_of(const std::uint8_t* pkt) {
    migris_ccsds_primary_header_t p{};
    migris_ccsds_primary_unpack(&p, pkt, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE);
    return p;
}

migris_pus_tm_secondary_header_t secondary_of(const std::uint8_t* pkt) {
    migris_pus_tm_secondary_header_t s{};
    migris_pus_tm_secondary_unpack(
        &s, &pkt[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE], MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE);
    return s;
}

std::uint16_t u16_be(const std::uint8_t* pkt, std::size_t at) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[udf + at]) << 8) |
                                      static_cast<std::uint16_t>(pkt[udf + at + 1U]));
}

std::uint32_t u32_be(const std::uint8_t* pkt, std::size_t at) {
    return (static_cast<std::uint32_t>(pkt[udf + at]) << 24) |
           (static_cast<std::uint32_t>(pkt[udf + at + 1U]) << 16) |
           (static_cast<std::uint32_t>(pkt[udf + at + 2U]) << 8) |
           static_cast<std::uint32_t>(pkt[udf + at + 3U]);
}

bool crc_ok(const std::uint8_t* pkt, std::size_t size) {
    const std::uint16_t computed = migris_crc16_ccitt_false(pkt, size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(pkt[size - 1U]));
    return computed == on_wire;
}

// A parameter snapshot with a distinct sentinel in every field, so a
// byte-offset regression in the serialiser is unambiguous.
migris_pus3_hk_params_t sentinel_params() {
    migris_pus3_hk_params_t p{};
    p.pus1_msg_counter[0] = 0x11U;
    p.pus1_msg_counter[1] = 0x12U;
    p.pus1_msg_counter[2] = 0x13U;
    p.pus1_msg_counter[3] = 0x14U;
    p.pus5_msg_counter[0] = 0x21U;
    p.pus5_msg_counter[1] = 0x22U;
    p.pus5_msg_counter[2] = 0x23U;
    p.pus5_msg_counter[3] = 0x24U;
    p.pus17_tm_msg_counter = 0x31U;
    p.tc_accepted_count = 0xAABBCCDDU;
    p.tc_rejected_count = 0x01020304U;
    p.rx_ring_overflow_drops = 0xDEADBEEFU;
    return p;
}

TEST(Pus3, HkReportIsWellFormed) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus3_build_hk_report(&ctx,
                                               test_apid,
                                               &seq,
                                               0x01020304U,
                                               MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                               &p,
                                               0U,
                                               tm.data(),
                                               tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS3_HK_TM_PACKET_SIZE));

    const migris_ccsds_primary_header_t hp = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());

    // Data field = sec hdr (10) + source data (29) + CRC (2) → len-1 = 40.
    EXPECT_EQ(hp.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(hp.apid, test_apid);
    EXPECT_EQ(hp.seq_count, 0U);
    EXPECT_EQ(hp.data_length, 40U);
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_HOUSEKEEPING);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT);
    EXPECT_EQ(s.destination_id, 0U);
    EXPECT_EQ(s.time_seconds, 0x01020304U);
    EXPECT_TRUE(crc_ok(tm.data(), MIGRIS_PUS3_HK_TM_PACKET_SIZE));
}

TEST(Pus3, ParameterBlockLayoutIsPinned) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0x0ABCU;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};

    ASSERT_GT(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0xCAFEF00DU,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0x1234U,
                                          tm.data(),
                                          tm.size()),
              0);

    EXPECT_EQ(u16_be(tm.data(), off_sid), MIGRIS_PUS3_SID_FRAMEWORK_DIAG);
    EXPECT_EQ(u32_be(tm.data(), off_uptime), 0xCAFEF00DU);
    EXPECT_EQ(u16_be(tm.data(), off_seq), 0x0ABCU);  // pre-advance snapshot
    EXPECT_EQ(tm[udf + off_pus1 + 0U], 0x11U);
    EXPECT_EQ(tm[udf + off_pus1 + 3U], 0x14U);
    EXPECT_EQ(tm[udf + off_pus5 + 0U], 0x21U);
    EXPECT_EQ(tm[udf + off_pus5 + 3U], 0x24U);
    EXPECT_EQ(tm[udf + off_pus17], 0x31U);
    EXPECT_EQ(u32_be(tm.data(), off_accepted), 0xAABBCCDDU);
    EXPECT_EQ(u32_be(tm.data(), off_rejected), 0x01020304U);
    EXPECT_EQ(u32_be(tm.data(), off_drops), 0xDEADBEEFU);
    // destination ID echoes the (poll) source ID when non-zero.
    EXPECT_EQ(secondary_of(tm.data()).destination_id, 0x1234U);
}

TEST(Pus3, SeqSnapshotIsPreAdvanceAndCountAdvances) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0x1FFFU;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};

    ASSERT_GT(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              0);
    // The CCSDS primary header AND the param block both carry the
    // pre-advance count; the shared counter is then advanced by one.
    EXPECT_EQ(primary_of(tm.data()).seq_count, 0x1FFFU);
    EXPECT_EQ(u16_be(tm.data(), off_seq), 0x1FFFU);
    EXPECT_EQ(seq, 0x2000U);
}

TEST(Pus3, SequenceCountWrapsMod2Pow14) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0x3FFFU;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              0);
    EXPECT_EQ(seq, 0U);
    EXPECT_EQ(u16_be(tm.data(), off_seq), 0x3FFFU);  // wrapped value written first
}

TEST(Pus3, MessageCounterAdvancesMod2Pow8) {
    migris_pus3_ctx_t ctx{};
    ctx.msg_counter[0] = 0xFFU;
    std::uint16_t seq = 0U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              0);
    EXPECT_EQ(secondary_of(tm.data()).msg_counter, 0xFFU);  // pre-advance value on wire
    EXPECT_EQ(ctx.msg_counter[0], 0U);                      // wrapped
}

TEST(Pus3, RejectsUnknownSidWithoutSideEffects) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 7U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          static_cast<migris_pus3_sid_t>(0x0002U),
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              MIGRIS_PUS3_ERR_UNKNOWN_SID);
    EXPECT_EQ(seq, 7U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus3, RejectsTooSmallBufferWithoutSideEffects) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 5U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE - 1U> tm{};
    EXPECT_EQ(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              MIGRIS_PUS3_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(seq, 5U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus3, RejectsNullArguments) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus3_build_hk_report(nullptr,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          &p,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus3_build_hk_report(&ctx,
                                          test_apid,
                                          &seq,
                                          0U,
                                          MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                          nullptr,
                                          0U,
                                          tm.data(),
                                          tm.size()),
              MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(
        migris_pus3_build_hk_report(
            &ctx, test_apid, &seq, 0U, MIGRIS_PUS3_SID_FRAMEWORK_DIAG, &p, 0U, nullptr, tm.size()),
        MIGRIS_PUS3_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
