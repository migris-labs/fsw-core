// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs

#include "migris/fsw/pus/pus3.h"

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/hkstore/hkstore.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

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

// --- Structure management (slice fsw-15) ---------------------------

// Build [3,1] create application data: SID(2) + parameter-count(1) +
// parameter ID(2 each) + interval(4), all big-endian.
std::vector<std::uint8_t>
create_app(std::uint16_t sid, const std::vector<std::uint16_t>& ids, std::uint32_t interval) {
    std::vector<std::uint8_t> a;
    a.push_back(static_cast<std::uint8_t>(sid >> 8));
    a.push_back(static_cast<std::uint8_t>(sid & 0xFFU));
    a.push_back(static_cast<std::uint8_t>(ids.size()));
    for (const std::uint16_t id : ids) {
        a.push_back(static_cast<std::uint8_t>(id >> 8));
        a.push_back(static_cast<std::uint8_t>(id & 0xFFU));
    }
    a.push_back(static_cast<std::uint8_t>(interval >> 24));
    a.push_back(static_cast<std::uint8_t>(interval >> 16));
    a.push_back(static_cast<std::uint8_t>(interval >> 8));
    a.push_back(static_cast<std::uint8_t>(interval & 0xFFU));
    return a;
}

// SID-only application data — the payload of [3,2] / [3,5] / [3,6].
std::array<std::uint8_t, 2U> sid_app(std::uint16_t sid) {
    return {static_cast<std::uint8_t>(sid >> 8), static_cast<std::uint8_t>(sid & 0xFFU)};
}

migris_dp_param_t
dp_param(migris_dp_param_id_t id, migris_dp_access_t access, migris_dp_value_t value) {
    migris_dp_param_t out{};
    out.id = id;
    out.access = access;
    out.value = value;
    return out;
}

// A datapool with two read-only parameters of distinct types and
// sentinel values, so a layout regression in the dynamic encoder is
// unambiguous.
migris_datapool_t dynamic_datapool() {
    const std::array<migris_dp_param_t, 2U> params{
        dp_param(0x0010U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_u32(0xAABBCCDDU)),
        dp_param(0x0011U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_u16(0x1234U)),
    };
    migris_datapool_t dp{};
    migris_datapool_init(&dp, params.data(), params.size());
    return dp;
}

// A structure sampling the two dynamic_datapool() parameters.
migris_hk_structure_t dynamic_structure() {
    migris_hk_structure_t s{};
    s.sid = 0x0100U;
    s.param_ids[0] = 0x0010U;
    s.param_ids[1] = 0x0011U;
    s.param_count = 2U;
    return s;
}

TEST(Pus3Execute, CreateAddsAStructureToTheStore) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto app = create_app(0x0100U, {0x0010U, 0x0011U}, 20U);
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE, app.data(), app.size()),
        MIGRIS_PUS3_OK);
    const migris_hk_structure_t* s = migris_hkstore_find(&store, 0x0100U);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->param_count, 2U);
    EXPECT_EQ(s->interval_sec, 20U);
}

TEST(Pus3Execute, CreateRejectsMalformedApplicationData) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    auto app = create_app(0x0100U, {0x0010U}, 20U);
    app.pop_back();  // one byte short of the declared layout
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE, app.data(), app.size()),
        MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
}

TEST(Pus3Execute, CreateRejectsAFrameworkRangeSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto app = create_app(0x0001U, {0x0010U}, 20U);  // 0x0001 is reserved
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE, app.data(), app.size()),
        MIGRIS_PUS3_ERR_EXEC_FAILED);
}

TEST(Pus3Execute, DeleteRemovesAStructure) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto created = create_app(0x0100U, {0x0010U}, 20U);
    ASSERT_EQ(migris_pus3_execute(
                  &store, MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE, created.data(), created.size()),
              MIGRIS_PUS3_OK);
    const auto sid = sid_app(0x0100U);
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE, sid.data(), sid.size()),
        MIGRIS_PUS3_OK);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
}

TEST(Pus3Execute, EnableAndDisableToggleAStructure) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto created = create_app(0x0100U, {0x0010U}, 20U);
    ASSERT_EQ(migris_pus3_execute(
                  &store, MIGRIS_PUS3_SUBTYPE_CREATE_STRUCTURE, created.data(), created.size()),
              MIGRIS_PUS3_OK);
    const auto sid = sid_app(0x0100U);
    ASSERT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_ENABLE_STRUCTURE, sid.data(), sid.size()),
        MIGRIS_PUS3_OK);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0100U)->enabled, 1);
    ASSERT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_DISABLE_STRUCTURE, sid.data(), sid.size()),
        MIGRIS_PUS3_OK);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0100U)->enabled, 0);
}

TEST(Pus3Execute, DeleteOfUnknownSidFails) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto sid = sid_app(0x0100U);
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE, sid.data(), sid.size()),
        MIGRIS_PUS3_ERR_EXEC_FAILED);
}

TEST(Pus3Execute, RejectsNonManagementSubtypeAndNullArguments) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const auto sid = sid_app(0x0100U);
    // [3,27] poll is not a management subtype, nor is an unknown one.
    EXPECT_EQ(
        migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL, sid.data(), sid.size()),
        MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus3_execute(&store, 99U, sid.data(), sid.size()), MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(
        migris_pus3_execute(nullptr, MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE, sid.data(), sid.size()),
        MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus3_execute(&store, MIGRIS_PUS3_SUBTYPE_DELETE_STRUCTURE, nullptr, 2U),
              MIGRIS_PUS3_ERR_BAD_ARG);
}

TEST(Pus3Dynamic, ReportIsWellFormed) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_datapool_t dp = dynamic_datapool();
    const migris_hk_structure_t s = dynamic_structure();
    std::array<std::uint8_t, 64U> tm{};

    // primary 6 + TM sec 10 + source (SID 2 + u32 4 + u16 2) + CRC 2 = 26.
    const int rc = migris_pus3_build_dynamic_hk_report(
        &ctx, &dp, &s, test_apid, &seq, 0x01020304U, 0U, tm.data(), tm.size());
    ASSERT_EQ(rc, 26);

    const migris_ccsds_primary_header_t hp = primary_of(tm.data());
    const migris_pus_tm_secondary_header_t sec = secondary_of(tm.data());
    EXPECT_EQ(hp.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(hp.apid, test_apid);
    EXPECT_EQ(sec.service_type, MIGRIS_PUS_SERVICE_HOUSEKEEPING);
    EXPECT_EQ(sec.service_subtype, MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT);
    EXPECT_EQ(sec.time_seconds, 0x01020304U);
    EXPECT_TRUE(crc_ok(tm.data(), 26U));
}

TEST(Pus3Dynamic, SourceDataIsSidThenConcatenatedValues) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_datapool_t dp = dynamic_datapool();
    const migris_hk_structure_t s = dynamic_structure();
    std::array<std::uint8_t, 64U> tm{};

    ASSERT_GT(migris_pus3_build_dynamic_hk_report(
                  &ctx, &dp, &s, test_apid, &seq, 0U, 0x4321U, tm.data(), tm.size()),
              0);
    EXPECT_EQ(u16_be(tm.data(), 0U), 0x0100U);      // Structure ID
    EXPECT_EQ(u32_be(tm.data(), 2U), 0xAABBCCDDU);  // first parameter (u32)
    EXPECT_EQ(u16_be(tm.data(), 6U), 0x1234U);      // second parameter (u16)
    EXPECT_EQ(secondary_of(tm.data()).destination_id, 0x4321U);
}

TEST(Pus3Dynamic, RejectsAStructureNamingAnUnknownParameter) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 9U;
    const migris_datapool_t dp = dynamic_datapool();
    migris_hk_structure_t s = dynamic_structure();
    s.param_ids[1] = 0x0099U;  // not defined in the datapool
    std::array<std::uint8_t, 64U> tm{};
    EXPECT_EQ(migris_pus3_build_dynamic_hk_report(
                  &ctx, &dp, &s, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS3_ERR_UNKNOWN_PARAM);
    EXPECT_EQ(seq, 9U);  // failed before any state advance
}

TEST(Pus3Dynamic, RejectsATooSmallBuffer) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 3U;
    const migris_datapool_t dp = dynamic_datapool();
    const migris_hk_structure_t s = dynamic_structure();
    std::array<std::uint8_t, 25U> tm{};  // one short of the 26-byte report
    EXPECT_EQ(migris_pus3_build_dynamic_hk_report(
                  &ctx, &dp, &s, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS3_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(seq, 3U);
}

TEST(Pus3Dynamic, AdvancesSequenceAndMessageCounterOnSuccess) {
    migris_pus3_ctx_t ctx{};
    ctx.msg_counter[0] = 0x40U;
    std::uint16_t seq = 0x0ABCU;
    const migris_datapool_t dp = dynamic_datapool();
    const migris_hk_structure_t s = dynamic_structure();
    std::array<std::uint8_t, 64U> tm{};
    ASSERT_GT(migris_pus3_build_dynamic_hk_report(
                  &ctx, &dp, &s, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              0);
    EXPECT_EQ(primary_of(tm.data()).seq_count, 0x0ABCU);  // pre-advance value on the wire
    EXPECT_EQ(seq, 0x0ABDU);
    EXPECT_EQ(ctx.msg_counter[0], 0x41U);
}

TEST(Pus3Dynamic, RejectsNullArguments) {
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_datapool_t dp = dynamic_datapool();
    const migris_hk_structure_t s = dynamic_structure();
    std::array<std::uint8_t, 64U> tm{};
    EXPECT_EQ(migris_pus3_build_dynamic_hk_report(
                  nullptr, &dp, &s, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus3_build_dynamic_hk_report(
                  &ctx, nullptr, &s, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS3_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus3_build_dynamic_hk_report(
                  &ctx, &dp, nullptr, test_apid, &seq, 0U, 0U, tm.data(), tm.size()),
              MIGRIS_PUS3_ERR_BAD_ARG);
}

TEST(Pus3, FrameworkDiagReportStaysFrozenAcrossFsw15) {
    // fsw-15 adds dynamic structures but must not perturb the frozen
    // FRAMEWORK_DIAG layout — migris_pus3_build_hk_report is unchanged.
    // The eight tests above pin every field; this is the size sentinel.
    migris_pus3_ctx_t ctx{};
    std::uint16_t seq = 0U;
    const migris_pus3_hk_params_t p = sentinel_params();
    std::array<std::uint8_t, MIGRIS_PUS3_HK_TM_PACKET_SIZE> tm{};
    const int rc = migris_pus3_build_hk_report(
        &ctx, test_apid, &seq, 0U, MIGRIS_PUS3_SID_FRAMEWORK_DIAG, &p, 0U, tm.data(), tm.size());
    EXPECT_EQ(rc, 47);
    EXPECT_EQ(MIGRIS_PUS3_HK_SOURCE_DATA_SIZE, 29U);
    EXPECT_EQ(secondary_of(tm.data()).service_subtype, MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT);
}

}  // namespace
}  // namespace migris::fsw::pus::test
