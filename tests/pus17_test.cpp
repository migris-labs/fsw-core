// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// PUS-17 is a leaf service as of slice fsw-5: generic TC reception
// (CCSDS primary / length / CRC / PUS-C version / APID) is the TC
// router's job and is covered in tc_router_test.cpp. Here we test
// only migris_pus17_execute: subtype gating and the PUS-17[2]
// response it builds for an already-accepted, already-routed TC.

#include "migris/fsw/pus/pus17.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

TEST(Pus17Execute, ValidSubtypeProducesWellFormedTm) {
    migris_pus17_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus17_execute(&ctx,
                                        test_apid,
                                        &seq,
                                        /*now_seconds=*/0xDEADBEEFU,
                                        MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                                        /*tc_source_id=*/0xBEEFU,
                                        tm.data(),
                                        tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));

    migris_ccsds_primary_header_t tm_primary{};
    ASSERT_EQ(migris_ccsds_primary_unpack(&tm_primary, tm.data(), tm.size()), MIGRIS_CCSDS_OK);
    EXPECT_EQ(tm_primary.version, 0U);
    EXPECT_EQ(tm_primary.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(tm_primary.sec_hdr_flag, 1U);
    EXPECT_EQ(tm_primary.apid, test_apid);
    EXPECT_EQ(tm_primary.seq_flags, MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED);
    EXPECT_EQ(tm_primary.seq_count, 0U);
    EXPECT_EQ(tm_primary.data_length, MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 2U - 1U);

    migris_pus_tm_secondary_header_t tm_sec{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&tm_sec,
                                             &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             tm.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(tm_sec.pus_version, MIGRIS_PUS_VERSION_C);
    EXPECT_EQ(tm_sec.service_type, MIGRIS_PUS_SERVICE_TEST);
    EXPECT_EQ(tm_sec.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tm_sec.msg_counter, 0U);
    EXPECT_EQ(tm_sec.destination_id, 0xBEEFU);
    EXPECT_EQ(tm_sec.time_seconds, 0xDEADBEEFU);

    const std::uint16_t computed = migris_crc16_ccitt_false(tm.data(), tm.size() - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(tm[tm.size() - 2U]) << 8) |
                                   static_cast<std::uint16_t>(tm[tm.size() - 1U]));
    EXPECT_EQ(computed, on_wire);

    // Side effects committed: shared seq + this service's msg counter.
    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.tm_msg_counter, 1U);
}

TEST(Pus17Execute, CountersAdvanceAndWrap) {
    migris_pus17_ctx_t ctx{};
    ctx.tm_msg_counter = 0xFEU;
    std::uint16_t seq = 0x3FFEU;
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(migris_pus17_execute(&ctx,
                                       test_apid,
                                       &seq,
                                       0U,
                                       MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                                       0U,
                                       tm.data(),
                                       tm.size()),
                  static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));
    }
    // seq: 3FFE → 3FFF → 0000 (wrap mod 2^14) → 0001 → 0002
    // msg:   FE →   FF →   00 (wrap mod 2^8)  →   01 →   02
    EXPECT_EQ(seq, 0x0002U);
    EXPECT_EQ(ctx.tm_msg_counter, 0x02U);
}

TEST(Pus17Execute, RejectsUnsupportedSubtype) {
    migris_pus17_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE> tm{};
    // Subtype 3 (application-specific connection test) is not in scope.
    EXPECT_EQ(migris_pus17_execute(
                  &ctx, test_apid, &seq, 0U, /*service_subtype=*/3U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS17_ERR_NOT_PUS17_TC);
    EXPECT_EQ(seq, 0U);
    EXPECT_EQ(ctx.tm_msg_counter, 0U);
}

TEST(Pus17Execute, RejectsTooSmallBuffer) {
    migris_pus17_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS17_TM_PACKET_SIZE - 1> tm{};
    EXPECT_EQ(migris_pus17_execute(&ctx,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                                   0U,
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS17_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(seq, 0U);
    EXPECT_EQ(ctx.tm_msg_counter, 0U);
}

}  // namespace
}  // namespace migris::fsw::pus::test
