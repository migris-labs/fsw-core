// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus17.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t kApid = 0x100U;

// Build a complete, CRC-correct PUS-17[1] TC the way the ground side
// does — the same shape that tests/renode/_pus.py emits. Keeping this
// in the test file is deliberate: it gives the host suite a
// self-contained ground-side reference encoder that's pinned to the
// wire-format spec, independent of the FSW decoder under test.
std::vector<std::uint8_t> build_tc_are_you_alive(std::uint16_t source_id,
                                                 std::uint16_t seq_count) {
    std::vector<std::uint8_t> tc(MIGRIS_PUS17_TC_PACKET_SIZE, 0U);

    const migris_ccsds_primary_header_t primary = {
        /*version=*/0,
        /*type=*/MIGRIS_CCSDS_PACKET_TYPE_TC,
        /*sec_hdr_flag=*/1,
        /*apid=*/kApid,
        /*seq_flags=*/MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        /*seq_count=*/seq_count,
        /*data_length=*/MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + 2U - 1U,
    };
    EXPECT_EQ(migris_ccsds_primary_pack(&primary, tc.data(), tc.size()),
              MIGRIS_CCSDS_OK);

    const migris_pus_tc_secondary_header_t tc_sec = {
        /*pus_version=*/MIGRIS_PUS_VERSION_C,
        /*ack_flags=*/0,
        /*service_type=*/MIGRIS_PUS_SERVICE_TEST,
        /*service_subtype=*/MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
        /*source_id=*/source_id,
    };
    EXPECT_EQ(migris_pus_tc_secondary_pack(
                  &tc_sec, &tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                  tc.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);

    const std::size_t crc_offset = tc.size() - 2U;
    const std::uint16_t crc = migris_crc16_ccitt_false(tc.data(), crc_offset);
    tc[crc_offset] = static_cast<std::uint8_t>(crc >> 8);
    tc[crc_offset + 1U] = static_cast<std::uint8_t>(crc & 0xFFU);
    return tc;
}

TEST(Pus17, ValidTcProducesWellFormedTm) {
    migris_pus17_ctx_t ctx = {kApid, /*tm_seq_count=*/0, /*tm_msg_counter=*/0};
    const auto tc = build_tc_are_you_alive(/*source_id=*/0xBEEF, /*seq_count=*/42);

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    const int rc = migris_pus17_handle_are_you_alive(
        &ctx, /*now_seconds=*/0xDEADBEEFU, tc.data(), tc.size(), tm.data(), tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));

    // Primary header: version 0, TM, sec_hdr=1, APID 0x100, seq_flags=11, seq_count=0.
    migris_ccsds_primary_header_t tm_primary{};
    ASSERT_EQ(migris_ccsds_primary_unpack(&tm_primary, tm.data(), tm.size()),
              MIGRIS_CCSDS_OK);
    EXPECT_EQ(tm_primary.version, 0U);
    EXPECT_EQ(tm_primary.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(tm_primary.sec_hdr_flag, 1U);
    EXPECT_EQ(tm_primary.apid, kApid);
    EXPECT_EQ(tm_primary.seq_flags, MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED);
    EXPECT_EQ(tm_primary.seq_count, 0U);
    EXPECT_EQ(tm_primary.data_length,
              MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 2U - 1U);

    // TM secondary header: PUS-C, service 17, subtype 2, msg_counter=0,
    // destination_id echoes TC source_id, time field matches `now_seconds`.
    migris_pus_tm_secondary_header_t tm_sec{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(
                  &tm_sec, &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                  tm.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(tm_sec.pus_version, MIGRIS_PUS_VERSION_C);
    EXPECT_EQ(tm_sec.service_type, MIGRIS_PUS_SERVICE_TEST);
    EXPECT_EQ(tm_sec.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tm_sec.msg_counter, 0U);
    EXPECT_EQ(tm_sec.destination_id, 0xBEEFU);
    EXPECT_EQ(tm_sec.time_seconds, 0xDEADBEEFU);

    // CRC over everything except the last two bytes.
    const std::uint16_t computed =
        migris_crc16_ccitt_false(tm.data(), tm.size() - 2U);
    const std::uint16_t on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(tm[tm.size() - 2U]) << 8) |
                                   static_cast<std::uint16_t>(tm[tm.size() - 1U]));
    EXPECT_EQ(computed, on_wire);
}

TEST(Pus17, ContextCountersAdvanceAndWrap) {
    migris_pus17_ctx_t ctx = {kApid, /*tm_seq_count=*/0x3FFEU, /*tm_msg_counter=*/0xFEU};
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};

    for (int i = 0; i < 4; ++i) {
        const auto tc = build_tc_are_you_alive(0, static_cast<std::uint16_t>(i));
        const int rc = migris_pus17_handle_are_you_alive(
            &ctx, 0U, tc.data(), tc.size(), tm.data(), tm.size());
        ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));
    }

    // After 4 invocations starting from 0x3FFE / 0xFE:
    //   seq_count: 3FFE -> 3FFF -> 0000 (wrap mod 2^14) -> 0001 -> 0002
    //   msg_count:   FE ->   FF -> 00   (wrap mod 2^8)  ->   01 ->   02
    EXPECT_EQ(ctx.tm_seq_count, 0x0002U);
    EXPECT_EQ(ctx.tm_msg_counter, 0x02U);
}

TEST(Pus17, RejectsTruncatedTc) {
    migris_pus17_ctx_t ctx = {kApid, 0, 0};
    auto tc = build_tc_are_you_alive(0, 0);
    tc.pop_back();  // 12 bytes instead of 13.

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_TRUNCATED);
    EXPECT_EQ(ctx.tm_seq_count, 0U);    // No side effects on failure.
    EXPECT_EQ(ctx.tm_msg_counter, 0U);
}

TEST(Pus17, RejectsCorruptedCrc) {
    migris_pus17_ctx_t ctx = {kApid, 0, 0};
    auto tc = build_tc_are_you_alive(0, 0);
    tc.back() ^= 0xFFU;  // Flip every bit of the last CRC byte.

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_BAD_CRC);
}

TEST(Pus17, RejectsWrongApid) {
    migris_pus17_ctx_t ctx = {/*apid=*/kApid + 1U, 0, 0};
    auto tc = build_tc_are_you_alive(0, 0);

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_BAD_PRIMARY);
}

TEST(Pus17, RejectsTmPacket) {
    // A TC built with the TM packet type bit must be rejected.
    migris_pus17_ctx_t ctx = {kApid, 0, 0};
    auto tc = build_tc_are_you_alive(0, 0);
    tc[0] = static_cast<std::uint8_t>(tc[0] & 0xEFU);  // Clear the TC type bit.
    // Recompute CRC so we don't get rejected for the wrong reason.
    const std::uint16_t crc = migris_crc16_ccitt_false(tc.data(), tc.size() - 2U);
    tc[tc.size() - 2U] = static_cast<std::uint8_t>(crc >> 8);
    tc[tc.size() - 1U] = static_cast<std::uint8_t>(crc & 0xFFU);

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_BAD_PRIMARY);
}

TEST(Pus17, RejectsWrongServiceSubtype) {
    migris_pus17_ctx_t ctx = {kApid, 0, 0};
    auto tc = build_tc_are_you_alive(0, 0);
    // Mutate subtype from 1 to 3 (application-specific test — not in scope).
    tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + 2U] = 3U;
    const std::uint16_t crc = migris_crc16_ccitt_false(tc.data(), tc.size() - 2U);
    tc[tc.size() - 2U] = static_cast<std::uint8_t>(crc >> 8);
    tc[tc.size() - 1U] = static_cast<std::uint8_t>(crc & 0xFFU);

    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_NOT_PUS17_TC);
}

TEST(Pus17, RejectsTooSmallTmBuffer) {
    migris_pus17_ctx_t ctx = {kApid, 0, 0};
    const auto tc = build_tc_are_you_alive(0, 0);
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE - 1> tm{};
    EXPECT_EQ(migris_pus17_handle_are_you_alive(&ctx, 0U, tc.data(), tc.size(),
                                                tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_BUF_TOO_SMALL);
}

}  // namespace
}  // namespace migris::fsw::pus::test
