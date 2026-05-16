// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus1.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// A representative request ID: the first four bytes of a PUS-17[1] TC
// on APID 0x100, seq_count 42 (matches docs/wire/pus-17.md framing).
constexpr std::array<std::uint8_t, MIGRIS_PUS1_REQUEST_ID_SIZE> req_id = {
    0x19U, 0x00U, 0xC0U, 0x2AU};

constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

// Pure helpers (no gtest macros — keeps the assertion functions and
// test bodies inside the cognitive-complexity budget).
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

bool crc_ok(const std::uint8_t* pkt, std::size_t size) {
    const std::uint16_t computed = migris_crc16_ccitt_false(pkt, size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(pkt[size - 1U]));
    return computed == on_wire;
}

TEST(Pus1, AcceptanceSuccessIsWellFormed) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus1_build_acceptance(&ctx,
                                                test_apid,
                                                &seq,
                                                0x01020304U,
                                                req_id.data(),
                                                0xBEEFU,
                                                MIGRIS_PUS1_FC_NONE,
                                                tm.data(),
                                                tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE));

    const migris_ccsds_primary_header_t p = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());

    // Primary: TM on our APID; data field = sec hdr (10) + req ID (4)
    // + CRC (2) → length-1 = 15.
    EXPECT_EQ(p.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(p.apid, test_apid);
    EXPECT_EQ(p.seq_count, 0U);
    EXPECT_EQ(p.data_length, 15U);
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_VERIFICATION);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS);
    EXPECT_EQ(s.destination_id, 0xBEEFU);
    EXPECT_EQ(s.time_seconds, 0x01020304U);
    EXPECT_EQ(std::memcmp(&tm[udf], req_id.data(), req_id.size()), 0);
    EXPECT_TRUE(crc_ok(tm.data(), tm.size()));
}

TEST(Pus1, AcceptanceSuccessCommitsCounters) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus1_build_acceptance(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           req_id.data(),
                                           0U,
                                           MIGRIS_PUS1_FC_NONE,
                                           tm.data(),
                                           tm.size()),
              0);
    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.msg_counter[0], 1U);  // acceptance-success counter
    EXPECT_EQ(ctx.msg_counter[1], 0U);
}

TEST(Pus1, AcceptanceFailureCarriesFailureCode) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 7U;
    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> tm{};

    const int rc = migris_pus1_build_acceptance(&ctx,
                                                test_apid,
                                                &seq,
                                                0U,
                                                req_id.data(),
                                                0x1234U,
                                                MIGRIS_PUS1_FC_CRC_FAILURE,
                                                tm.data(),
                                                tm.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE));

    const migris_ccsds_primary_header_t p = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());

    // Failure data field = 10 + 4 + 1 + 2 → length-1 = 16.
    EXPECT_EQ(p.seq_count, 7U);
    EXPECT_EQ(p.data_length, 16U);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE);
    EXPECT_EQ(std::memcmp(&tm[udf], req_id.data(), req_id.size()), 0);
    EXPECT_EQ(tm[udf + MIGRIS_PUS1_REQUEST_ID_SIZE],
              static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE));
    EXPECT_TRUE(crc_ok(tm.data(), tm.size()));
    EXPECT_EQ(ctx.msg_counter[1], 1U);  // acceptance-failure counter
}

TEST(Pus1, CompletionSuccessUsesSubtype7) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus1_build_completion(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           req_id.data(),
                                           0xAAU,
                                           MIGRIS_PUS1_FC_NONE,
                                           tm.data(),
                                           tm.size()),
              0);
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS);
    EXPECT_EQ(ctx.msg_counter[2], 1U);  // completion-success counter
}

TEST(Pus1, CompletionFailureUsesSubtype8AndCode) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus1_build_completion(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           req_id.data(),
                                           0xAAU,
                                           MIGRIS_PUS1_FC_EXEC_FAILURE,
                                           tm.data(),
                                           tm.size()),
              0);
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE);
    EXPECT_EQ(tm[udf + MIGRIS_PUS1_REQUEST_ID_SIZE],
              static_cast<std::uint8_t>(MIGRIS_PUS1_FC_EXEC_FAILURE));
    EXPECT_EQ(ctx.msg_counter[3], 1U);  // completion-failure counter
}

TEST(Pus1, MessageCountersAreIndependentPerSubtype) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE> tm{};

    migris_pus1_build_acceptance(
        &ctx, test_apid, &seq, 0U, req_id.data(), 0U, MIGRIS_PUS1_FC_NONE, tm.data(), tm.size());
    migris_pus1_build_acceptance(
        &ctx, test_apid, &seq, 0U, req_id.data(), 0U, MIGRIS_PUS1_FC_NONE, tm.data(), tm.size());
    migris_pus1_build_acceptance(&ctx,
                                 test_apid,
                                 &seq,
                                 0U,
                                 req_id.data(),
                                 0U,
                                 MIGRIS_PUS1_FC_BAD_PRIMARY,
                                 tm.data(),
                                 tm.size());
    migris_pus1_build_completion(
        &ctx, test_apid, &seq, 0U, req_id.data(), 0U, MIGRIS_PUS1_FC_NONE, tm.data(), tm.size());

    const std::array<std::uint8_t, 4> counters = {
        ctx.msg_counter[0], ctx.msg_counter[1], ctx.msg_counter[2], ctx.msg_counter[3]};
    EXPECT_EQ(counters, (std::array<std::uint8_t, 4>{2U, 1U, 1U, 0U}));
    EXPECT_EQ(seq, 4U);  // one shared sequence count per packet
}

TEST(Pus1, SequenceCountWrapsMod2Pow14) {
    migris_pus1_ctx_t ctx{};
    std::uint16_t seq = 0x3FFFU;
    std::array<std::uint8_t, MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus1_build_acceptance(&ctx,
                                           test_apid,
                                           &seq,
                                           0U,
                                           req_id.data(),
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
                                           req_id.data(),
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
