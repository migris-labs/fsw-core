// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// PUS-20 on-board parameter management — service handler.
// Pure (gtest-free) helpers build the TC application data and decode
// the packet headers, keeping the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise the [20,1] report
// path, the all-or-nothing [20,3] set path, and the malformed /
// unknown-ID / read-only / bad-subtype rejection contracts against a
// mixed-type datapool fixture.

#include "migris/fsw/pus/pus20.h"

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;
constexpr std::uint16_t test_source_id = 0xCAFEU;

// Source-data offset: primary (6) + PUS-C TM secondary (10).
constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

migris_dp_param_t
param(migris_dp_param_id_t id, migris_dp_access_t access, migris_dp_value_t value) {
    migris_dp_param_t out{};
    out.id = id;
    out.access = access;
    out.value = value;
    return out;
}

// A mixed-type datapool: one parameter of every supported type plus a
// read-only parameter, with distinctive sentinel initial values.
migris_datapool_t make_pool() {
    const std::array<migris_dp_param_t, 8U> defs{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(0x11223344U)),
        param(0x0002U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u16(0xABCDU)),
        param(0x0003U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u8(0x5AU)),
        param(0x0004U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_i32(-2)),
        param(0x0005U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_i16(-3)),
        param(0x0006U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_i8(-4)),
        param(0x0007U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_f32(1.5F)),
        param(0x0008U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_u32(0xDEADBEEFU)),
    };
    migris_datapool_t dp{};
    migris_datapool_init(&dp, defs.data(), defs.size());
    return dp;
}

// [20,1] application data: 1-byte count + 2-byte big-endian IDs.
std::vector<std::uint8_t> report_app(const std::vector<std::uint16_t>& ids) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(ids.size()));
    for (const std::uint16_t id : ids) {
        app.push_back(static_cast<std::uint8_t>(id >> 8));
        app.push_back(static_cast<std::uint8_t>(id & 0xFFU));
    }
    return app;
}

// [20,3] application data: 1-byte count + (2-byte ID, value) pairs.
std::vector<std::uint8_t>
set_app(const std::vector<std::pair<std::uint16_t, migris_dp_value_t>>& items) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(items.size()));
    for (const auto& item : items) {
        app.push_back(static_cast<std::uint8_t>(item.first >> 8));
        app.push_back(static_cast<std::uint8_t>(item.first & 0xFFU));
        std::array<std::uint8_t, 4U> vbuf{};
        const int written = migris_dp_value_encode(&item.second, vbuf.data(), vbuf.size());
        const std::size_t width = (written > 0) ? static_cast<std::size_t>(written) : 0U;
        for (std::size_t i = 0U; i < width; ++i) {
            app.push_back(vbuf[i]);
        }
    }
    return app;
}

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

TEST(Pus20, ReportRequestEmitsValueReport) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const auto app = report_app({0x0001U, 0x0007U});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    const int rc = migris_pus20_execute(&ctx,
                                        &dp,
                                        test_apid,
                                        &seq,
                                        0x01020304U,
                                        MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                        test_source_id,
                                        app.data(),
                                        app.size(),
                                        tm.data(),
                                        tm.size());
    // primary (6) + TM sec (10) + source [1 + (2+4) + (2+4)] + CRC (2) = 31.
    ASSERT_EQ(rc, 31);

    const migris_ccsds_primary_header_t p = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t s = secondary_of(tm.data());
    EXPECT_EQ(p.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(p.apid, test_apid);
    EXPECT_EQ(p.seq_count, 0U);
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS20_SUBTYPE_VALUE_REPORT);
    EXPECT_EQ(s.destination_id, test_source_id);
    EXPECT_EQ(s.time_seconds, 0x01020304U);
    EXPECT_TRUE(crc_ok(tm.data(), 31U));

    EXPECT_EQ(tm[udf + 0U], 2U);     // count
    EXPECT_EQ(tm[udf + 1U], 0x00U);  // id 0x0001
    EXPECT_EQ(tm[udf + 2U], 0x01U);
    EXPECT_EQ(tm[udf + 3U], 0x11U);  // u32 0x11223344, big-endian
    EXPECT_EQ(tm[udf + 4U], 0x22U);
    EXPECT_EQ(tm[udf + 5U], 0x33U);
    EXPECT_EQ(tm[udf + 6U], 0x44U);
    EXPECT_EQ(tm[udf + 7U], 0x00U);  // id 0x0007
    EXPECT_EQ(tm[udf + 8U], 0x07U);
    EXPECT_EQ(tm[udf + 9U], 0x3FU);  // f32 1.5, IEEE-754 big-endian
    EXPECT_EQ(tm[udf + 10U], 0xC0U);
    EXPECT_EQ(tm[udf + 11U], 0x00U);
    EXPECT_EQ(tm[udf + 12U], 0x00U);

    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.msg_counter[0], 1U);
}

TEST(Pus20, ReportRequestAtMaxParamsFitsTheBoundedPacket) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const auto app =
        report_app({0x0001U, 0x0002U, 0x0003U, 0x0004U, 0x0005U, 0x0006U, 0x0007U, 0x0008U});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    const int rc = migris_pus20_execute(&ctx,
                                        &dp,
                                        test_apid,
                                        &seq,
                                        0U,
                                        MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                        test_source_id,
                                        app.data(),
                                        app.size(),
                                        tm.data(),
                                        tm.size());
    ASSERT_GT(rc, 0);
    EXPECT_LE(rc, static_cast<int>(MIGRIS_PUS20_TM_MAX_PACKET_SIZE));
    EXPECT_TRUE(crc_ok(tm.data(), static_cast<std::size_t>(rc)));
    EXPECT_EQ(tm[udf], MIGRIS_PUS20_MAX_PARAMS_PER_TC);  // count
}

TEST(Pus20, ReportRequestRejectsUnknownIdWithoutSideEffects) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 4U;
    const auto app = report_app({0x0001U, 0x0999U});  // 0x0999 is undefined
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_UNKNOWN_ID);
    EXPECT_EQ(seq, 4U);
    EXPECT_EQ(ctx.msg_counter[0], 0U);
}

TEST(Pus20, ReportRequestRejectsMalformedAndOversizedRequests) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    // Count says 2 but only one ID follows.
    const std::array<std::uint8_t, 3U> short_app{0x02U, 0x00U, 0x01U};
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   short_app.data(),
                                   short_app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_MALFORMED);

    // Empty application data — no count byte.
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   short_app.data(),
                                   0U,
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_MALFORMED);

    // Count over the per-TC maximum.
    const std::array<std::uint8_t, 1U> too_many{
        static_cast<std::uint8_t>(MIGRIS_PUS20_MAX_PARAMS_PER_TC + 1U)};
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   too_many.data(),
                                   too_many.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_TOO_MANY);
}

TEST(Pus20, SetRequestAppliesEveryWriteAndEmitsNoTelemetry) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 9U;
    const auto app =
        set_app({{0x0001U, migris_dp_u32(0x55667788U)}, {0x0005U, migris_dp_i16(1234)}});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_OK);
    EXPECT_EQ(seq, 9U);  // no report → no sequence advance
    EXPECT_EQ(ctx.msg_counter[0], 0U);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 0x55667788U);
    ASSERT_EQ(migris_datapool_get(&dp, 0x0005U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_i16(&got), 1234);
}

TEST(Pus20, SetRequestIsAtomicWhenAnIdIsUnknown) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    // First item is valid, second names an undefined parameter.
    const auto app = set_app({{0x0001U, migris_dp_u32(0xFFFFFFFFU)}, {0x0999U, migris_dp_u32(0U)}});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_UNKNOWN_ID);

    // The valid first write must NOT have landed.
    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 0x11223344U);  // unchanged
}

TEST(Pus20, SetRequestRejectsReadOnlyWithoutSideEffects) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    // 0x0008 is read-only; the valid 0x0001 write must be rolled back with it.
    const auto app = set_app({{0x0001U, migris_dp_u32(7U)}, {0x0008U, migris_dp_u32(7U)}});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_READ_ONLY);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 0x11223344U);  // unchanged
}

TEST(Pus20, SetRequestRejectsTruncatedAndTrailingBytes) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    // Count 1, id 0x0001 (u32 → 4 value bytes) but only 2 value bytes given.
    const std::array<std::uint8_t, 5U> truncated{0x01U, 0x00U, 0x01U, 0xAAU, 0xBBU};
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   truncated.data(),
                                   truncated.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_MALFORMED);

    // Count 0 but a trailing byte the walk cannot account for.
    const std::array<std::uint8_t, 2U> trailing{0x00U, 0xFFU};
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   trailing.data(),
                                   trailing.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_MALFORMED);
}

TEST(Pus20, SetRequestRepeatedIdIsLastWriterWins) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const auto app = set_app({{0x0001U, migris_dp_u32(111U)}, {0x0001U, migris_dp_u32(222U)}});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    ASSERT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_OK);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 222U);
}

TEST(Pus20, F32ParameterSetsAndReportsBitExact) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    const auto set = set_app({{0x0007U, migris_dp_f32(-2.5F)}});
    ASSERT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                                   test_source_id,
                                   set.data(),
                                   set.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_OK);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0007U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_FLOAT_EQ(migris_dp_as_f32(&got), -2.5F);

    const auto rep = report_app({0x0007U});
    ASSERT_GT(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   rep.data(),
                                   rep.size(),
                                   tm.data(),
                                   tm.size()),
              0);
    // -2.5F == 0xC0200000 in IEEE-754 single precision.
    EXPECT_EQ(tm[udf + 3U], 0xC0U);
    EXPECT_EQ(tm[udf + 4U], 0x20U);
    EXPECT_EQ(tm[udf + 5U], 0x00U);
    EXPECT_EQ(tm[udf + 6U], 0x00U);
}

TEST(Pus20, RejectsUnsupportedSubtype) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const auto app = report_app({0x0001U});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    // Subtype 2 is the [20,2] report — a TM subtype, never inbound.
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_VALUE_REPORT,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_BAD_SUBTYPE);
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   99U,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_BAD_SUBTYPE);
}

TEST(Pus20, RejectsNullArguments) {
    migris_datapool_t dp = make_pool();
    migris_pus20_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const auto app = report_app({0x0001U});
    std::array<std::uint8_t, MIGRIS_PUS20_TM_MAX_PACKET_SIZE> tm{};

    EXPECT_EQ(migris_pus20_execute(nullptr,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   nullptr,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   app.data(),
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus20_execute(&ctx,
                                   &dp,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                                   test_source_id,
                                   nullptr,
                                   app.size(),
                                   tm.data(),
                                   tm.size()),
              MIGRIS_PUS20_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
