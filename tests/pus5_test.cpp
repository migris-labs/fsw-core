// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus5.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// User-data-field offset: primary (6) + PUS-C TM secondary (10).
constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

// Smallest PUS-5 packet: udf + event ID (2) + CRC (2).
constexpr std::size_t bare_size = udf + MIGRIS_PUS5_EVENT_ID_SIZE + 2U;

// Pure helpers (no gtest macros — keeps assertion helpers and test
// bodies inside the cognitive-complexity budget).
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

std::uint16_t event_id_of(const std::uint8_t* pkt) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[udf]) << 8) |
                                      static_cast<std::uint16_t>(pkt[udf + 1U]));
}

bool crc_ok(const std::uint8_t* pkt, std::size_t size) {
    const std::uint16_t computed = migris_crc16_ccitt_false(pkt, size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(pkt[size - 1U]));
    return computed == on_wire;
}

TEST(Pus5, InfoEventIsWellFormed) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};

    const int rc = migris_pus5_build_event_report(&ctx,
                                                  test_apid,
                                                  &seq,
                                                  0x01020304U,
                                                  MIGRIS_PUS5_SEV_INFO,
                                                  MIGRIS_PUS5_EVT_FSW_BOOT,
                                                  nullptr,
                                                  0U,
                                                  0U,
                                                  tm.data(),
                                                  tm.size());
    ASSERT_EQ(rc, static_cast<int>(bare_size));

    const migris_ccsds_primary_header_t p = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());

    // Data field = sec hdr (10) + event ID (2) + CRC (2) → length-1 = 13.
    EXPECT_EQ(p.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(p.apid, test_apid);
    EXPECT_EQ(p.seq_count, 0U);
    EXPECT_EQ(p.data_length, 13U);
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_EVENT_REPORTING);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS5_SUBTYPE_INFO);
    EXPECT_EQ(s.destination_id, 0U);
    EXPECT_EQ(s.time_seconds, 0x01020304U);
    EXPECT_EQ(event_id_of(tm.data()), MIGRIS_PUS5_EVT_FSW_BOOT);
    EXPECT_TRUE(crc_ok(tm.data(), bare_size));
}

TEST(Pus5, EventIdIsBigEndian) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_INFO,
                                             0x1234U,
                                             nullptr,
                                             0U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              0);
    EXPECT_EQ(tm[udf], 0x12U);
    EXPECT_EQ(tm[udf + 1U], 0x34U);
}

TEST(Pus5, SeverityMapsToSubtypeAndCounter) {
    struct Case {
        migris_pus5_severity_t sev;
        std::uint8_t subtype;
        std::size_t counter_idx;
    };

    const std::array<Case, 4> cases = {{
        {MIGRIS_PUS5_SEV_INFO, MIGRIS_PUS5_SUBTYPE_INFO, 0U},
        {MIGRIS_PUS5_SEV_LOW, MIGRIS_PUS5_SUBTYPE_LOW, 1U},
        {MIGRIS_PUS5_SEV_MEDIUM, MIGRIS_PUS5_SUBTYPE_MEDIUM, 2U},
        {MIGRIS_PUS5_SEV_HIGH, MIGRIS_PUS5_SUBTYPE_HIGH, 3U},
    }};

    for (const Case& c : cases) {
        migris_pus5_ctx_t ctx{};
        std::uint16_t seq = 0U;
        std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
        ASSERT_GT(
            migris_pus5_build_event_report(
                &ctx, test_apid, &seq, 0U, c.sev, 0xBEEFU, nullptr, 0U, 0U, tm.data(), tm.size()),
            0);
        EXPECT_EQ(secondary_of(tm.data()).service_subtype, c.subtype);
        EXPECT_EQ(ctx.msg_counter[c.counter_idx], 1U);
    }
}

TEST(Pus5, AuxDataIsAppendedVerbatim) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const std::array<std::uint8_t, 4> aux = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};

    const int rc = migris_pus5_build_event_report(&ctx,
                                                  test_apid,
                                                  &seq,
                                                  0U,
                                                  MIGRIS_PUS5_SEV_LOW,
                                                  0x00ABU,
                                                  aux.data(),
                                                  aux.size(),
                                                  0x1234U,
                                                  tm.data(),
                                                  tm.size());
    ASSERT_EQ(rc, static_cast<int>(bare_size + aux.size()));

    EXPECT_EQ(primary_of(tm.data()).data_length,
              static_cast<std::uint16_t>(MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE +
                                         MIGRIS_PUS5_EVENT_ID_SIZE + aux.size() + 2U - 1U));
    EXPECT_EQ(secondary_of(tm.data()).destination_id, 0x1234U);
    EXPECT_EQ(event_id_of(tm.data()), 0x00ABU);
    EXPECT_EQ(std::memcmp(&tm[udf + MIGRIS_PUS5_EVENT_ID_SIZE], aux.data(), aux.size()), 0);
    EXPECT_TRUE(crc_ok(tm.data(), bare_size + aux.size()));
}

TEST(Pus5, NonNullAuxWithZeroLenIsLegal) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const std::array<std::uint8_t, 1> aux = {0xFFU};
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_HIGH,
                                             0x0001U,
                                             aux.data(),
                                             0U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              static_cast<int>(bare_size));
}

TEST(Pus5, MessageCountersAreIndependentPerSeverity) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};

    const auto emit = [&](migris_pus5_severity_t sev) {
        migris_pus5_build_event_report(
            &ctx, test_apid, &seq, 0U, sev, 0x0010U, nullptr, 0U, 0U, tm.data(), tm.size());
    };
    emit(MIGRIS_PUS5_SEV_INFO);
    emit(MIGRIS_PUS5_SEV_INFO);
    emit(MIGRIS_PUS5_SEV_LOW);
    emit(MIGRIS_PUS5_SEV_HIGH);

    const std::array<std::uint8_t, 4> counters = {
        ctx.msg_counter[0], ctx.msg_counter[1], ctx.msg_counter[2], ctx.msg_counter[3]};
    EXPECT_EQ(counters, (std::array<std::uint8_t, 4>{2U, 1U, 0U, 1U}));
    EXPECT_EQ(seq, 4U);  // one shared sequence count per packet
}

TEST(Pus5, SequenceCountWrapsMod2Pow14) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0x3FFFU;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
    ASSERT_GT(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_INFO,
                                             0x0001U,
                                             nullptr,
                                             0U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              0);
    EXPECT_EQ(seq, 0U);
}

TEST(Pus5, RejectsTooSmallBufferWithoutSideEffects) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 5U;
    std::array<std::uint8_t, bare_size - 1U> tm{};
    EXPECT_EQ(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_INFO,
                                             0x0001U,
                                             nullptr,
                                             0U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              MIGRIS_PUS5_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(seq, 5U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus5, RejectsAuxOverMax) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_AUX_MAX_LEN + 1U> aux{};
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE + 8U> tm{};
    EXPECT_EQ(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_INFO,
                                             0x0001U,
                                             aux.data(),
                                             aux.size(),
                                             0U,
                                             tm.data(),
                                             tm.size()),
              MIGRIS_PUS5_ERR_BAD_ARG);
    EXPECT_EQ(seq, 0U);
}

TEST(Pus5, RejectsBadSeverity) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             static_cast<migris_pus5_severity_t>(9),
                                             0x0001U,
                                             nullptr,
                                             0U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              MIGRIS_PUS5_ERR_BAD_ARG);
}

TEST(Pus5, RejectsNullAuxWithNonzeroLen) {
    migris_pus5_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> tm{};
    EXPECT_EQ(migris_pus5_build_event_report(&ctx,
                                             test_apid,
                                             &seq,
                                             0U,
                                             MIGRIS_PUS5_SEV_INFO,
                                             0x0001U,
                                             nullptr,
                                             4U,
                                             0U,
                                             tm.data(),
                                             tm.size()),
              MIGRIS_PUS5_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
