// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus1.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// A representative request ID: the first four bytes of a PUS-17[1] TC
// on APID 0x100, seq_count 42 (matches docs/wire/pus-17.md framing).
constexpr std::array<std::uint8_t, MIGRIS_PUS1_REQUEST_ID_SIZE> kReqId = {
    0x19U, 0x00U, 0xC0U, 0x2AU};

void check_primary(const std::uint8_t* pkt,
                   std::uint16_t expect_seq,
                   std::uint16_t expect_data_len) {
    migris_ccsds_primary_header_t p{};
    ASSERT_EQ(migris_ccsds_primary_unpack(&p, pkt, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              MIGRIS_CCSDS_OK);
    EXPECT_EQ(p.version, 0U);
    EXPECT_EQ(p.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(p.sec_hdr_flag, 1U);
    EXPECT_EQ(p.apid, test_apid);
    EXPECT_EQ(p.seq_flags, MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED);
    EXPECT_EQ(p.seq_count, expect_seq);
    EXPECT_EQ(p.data_length, expect_data_len);
}

void check_crc(const std::uint8_t* pkt, std::size_t size) {
    const std::uint16_t computed = migris_crc16_ccitt_false(pkt, size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(pkt[size - 1U]));
    EXPECT_EQ(computed, on_wire);
}

TEST(Pus1, AcceptanceSuccessIsWellFormed) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus1_build_acceptance(&ctx,
                                                test_apid,
                                                &seq,
                                                /*now_seconds=*/0x01020304U,
                                                kReqId.data(),
                                                /*destination_id=*/0xBEEFU,
                                                MIGRIS_PUS1_FC_NONE,
                                                tm.data(),
                                                tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE));

    // data field = TM sec hdr (10) + request ID (4) + CRC (2) → len-1 = 15.
    check_primary(tm.data(), /*expect_seq=*/0U, /*expect_data_len=*/15U);

    migris_pus_tm_secondary_header_t s{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&s,
                                             &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             tm.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(s.pus_version, MIGRIS_PUS_VERSION_C);
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_VERIFICATION);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS);
    EXPECT_EQ(s.msg_counter, 0U);
    EXPECT_EQ(s.destination_id, 0xBEEFU);
    EXPECT_EQ(s.time_seconds, 0x01020304U);

    // User data = the 4-byte request ID, no failure code on a success report.
    const std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    for (std::size_t i = 0; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        EXPECT_EQ(tm[udf + i], kReqId[i]) << "request id byte " << i;
    }
    check_crc(tm.data(), tm.size());

    // Side effects committed on success.
    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.msg_counter[0], 1U);
    EXPECT_EQ(ctx.msg_counter[1], 0U);
}

TEST(Pus1, AcceptanceFailureCarriesFailureCode) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 7U;
    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus1_build_acceptance(&ctx,
                                                test_apid,
                                                &seq,
                                                /*now_seconds=*/0U,
                                                kReqId.data(),
                                                /*destination_id=*/0x1234U,
                                                MIGRIS_PUS1_FC_CRC_FAILURE,
                                                tm.data(),
                                                tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE));

    // data field = 10 + 4 + 1 + 2 → len-1 = 16.
    check_primary(tm.data(), /*expect_seq=*/7U, /*expect_data_len=*/16U);

    migris_pus_tm_secondary_header_t s{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&s,
                                             &tm[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             tm.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE);

    const std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    for (std::size_t i = 0; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        EXPECT_EQ(tm[udf + i], kReqId[i]);
    }
    EXPECT_EQ(tm[udf + MIGRIS_PUS1_REQUEST_ID_SIZE],
              static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE));
    check_crc(tm.data(), tm.size());

    EXPECT_EQ(seq, 8U);
    EXPECT_EQ(ctx.msg_counter[1], 1U);  // acceptance-failure counter
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus1, CompletionSuccessAndFailureUseSubtypes7And8) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;

    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> ok{};
    ASSERT_EQ(migris_pus1_build_completion(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           kReqId.data(),
                                           0xAAU,
                                           MIGRIS_PUS1_FC_NONE,
                                           ok.data(),
                                           ok.size()),
              static_cast<int>(MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE));

    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> bad{};
    ASSERT_EQ(migris_pus1_build_completion(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           kReqId.data(),
                                           0xAAU,
                                           MIGRIS_PUS1_FC_EXEC_FAILURE,
                                           bad.data(),
                                           bad.size()),
              static_cast<int>(MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE));

    migris_pus_tm_secondary_header_t s{};
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&s,
                                             &ok[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             ok.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS);
    ASSERT_EQ(migris_pus_tm_secondary_unpack(&s,
                                             &bad[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             bad.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE);
    EXPECT_EQ(bad[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE +
                  MIGRIS_PUS1_REQUEST_ID_SIZE],
              static_cast<std::uint8_t>(MIGRIS_PUS1_FC_EXEC_FAILURE));

    // Completion uses message counters [2] (success) and [3] (failure);
    // acceptance counters [0]/[1] are untouched.
    EXPECT_EQ(ctx.msg_counter[2], 1U);
    EXPECT_EQ(ctx.msg_counter[3], 1U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
    EXPECT_EQ(ctx.msg_counter[1], 0U);
    EXPECT_EQ(seq, 2U);
}

TEST(Pus1, MessageCountersAreIndependentPerSubtype) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> tm{};

    auto acc = [&](migris_pus1_failure_code_t fc) {
        return migris_pus1_build_acceptance(
            &ctx, test_apid, &seq, 0U, kReqId.data(), 0U, fc, tm.data(), tm.size());
    };
    ASSERT_GT(acc(MIGRIS_PUS1_FC_NONE), 0);
    ASSERT_GT(acc(MIGRIS_PUS1_FC_NONE), 0);
    ASSERT_GT(acc(MIGRIS_PUS1_FC_BAD_PRIMARY), 0);
    ASSERT_GT(migris_pus1_build_completion(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           kReqId.data(),
                                           0U,
                                           MIGRIS_PUS1_FC_NONE,
                                           tm.data(),
                                           tm.size()),
              0);

    EXPECT_EQ(ctx.msg_counter[0], 2U);  // 2 acceptance successes
    EXPECT_EQ(ctx.msg_counter[1], 1U);  // 1 acceptance failure
    EXPECT_EQ(ctx.msg_counter[2], 1U);  // 1 completion success
    EXPECT_EQ(ctx.msg_counter[3], 0U);
    EXPECT_EQ(seq, 4U);  // shared sequence count advanced once per packet
}

TEST(Pus1, SequenceCountWrapsMod2Pow14) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0x3FFFU;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus1_build_acceptance(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           kReqId.data(),
                                           0U,
                                           MIGRIS_PUS1_FC_NONE,
                                           tm.data(),
                                           tm.size()),
              0);
    EXPECT_EQ(seq, 0U);
}

TEST(Pus1, RejectsTooSmallBufferWithoutSideEffects) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 5U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE - 1> tm{};
    EXPECT_EQ(migris_pus1_build_acceptance(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           kReqId.data(),
                                           0U,
                                           MIGRIS_PUS1_FC_NONE,
                                           tm.data(),
                                           tm.size()),
              MIGRIS_PUS1_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(seq, 5U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus1, RejectsNullRequestId) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};
    EXPECT_EQ(
        migris_pus1_build_acceptance(
            &ctx, test_apid, &seq, 0U, nullptr, 0U, MIGRIS_PUS1_FC_NONE, tm.data(), tm.size()),
        MIGRIS_PUS1_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
