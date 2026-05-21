// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// PUS-11 on-board time-based scheduling — service handler.
// Pure (gtest-free) helpers build the TC application data (including
// the embedded telecommands an insert carries) and decode the [11,12]
// summary report, keeping the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise enable/disable,
// reset, the all-or-nothing insert and delete paths, the summary
// report, and the malformed / unknown-id / bad-subtype rejections.

#include "migris/fsw/pus/pus11.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"
#include "migris/fsw/schedule/schedule.h"

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

// A minimal embedded telecommand: a correct 6-byte CCSDS primary
// header (so the scheduler reads its declared length) followed by
// `body_len` filler bytes. `seq_count` varies the packet sequence
// control, so it forms part of the 4-byte request identifier (the
// telecommand's first four bytes).
std::vector<std::uint8_t> embedded_tc(std::uint16_t seq_count, std::size_t body_len) {
    std::vector<std::uint8_t> tc(MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + body_len, 0x5AU);
    migris_ccsds_primary_header_t hdr{};
    hdr.version = 0U;
    hdr.type = MIGRIS_CCSDS_PACKET_TYPE_TC;
    hdr.sec_hdr_flag = 1U;
    hdr.apid = test_apid;
    hdr.seq_flags = MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED;
    hdr.seq_count = seq_count;
    hdr.data_length = static_cast<std::uint16_t>(body_len - 1U);
    migris_ccsds_primary_pack(&hdr, tc.data(), tc.size());
    return tc;
}

// The 4-byte request identifier of an embedded telecommand.
std::array<std::uint8_t, 4U> request_id_of(const std::vector<std::uint8_t>& tc) {
    return {tc[0], tc[1], tc[2], tc[3]};
}

// [11,4] application data: 1-byte count + (4-byte release time,
// embedded TC) pairs.
std::vector<std::uint8_t>
insert_app(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& items) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(items.size()));
    for (const auto& item : items) {
        app.push_back(static_cast<std::uint8_t>(item.first >> 24));
        app.push_back(static_cast<std::uint8_t>(item.first >> 16));
        app.push_back(static_cast<std::uint8_t>(item.first >> 8));
        app.push_back(static_cast<std::uint8_t>(item.first & 0xFFU));
        app.insert(app.end(), item.second.begin(), item.second.end());
    }
    return app;
}

// [11,5] / [11,11] application data: 1-byte count + 4-byte request ids.
std::vector<std::uint8_t> id_list_app(const std::vector<std::array<std::uint8_t, 4U>>& ids) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(ids.size()));
    for (const auto& id : ids) {
        app.insert(app.end(), id.begin(), id.end());
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

std::uint32_t u32_be(const std::uint8_t* pkt, std::size_t at) {
    return (static_cast<std::uint32_t>(pkt[at]) << 24) |
           (static_cast<std::uint32_t>(pkt[at + 1U]) << 16) |
           (static_cast<std::uint32_t>(pkt[at + 2U]) << 8) |
           static_cast<std::uint32_t>(pkt[at + 3U]);
}

// Execute one PUS-11 TC against a fresh, disabled schedule fixture.
int execute(migris_schedule_t& sched,
            migris_pus11_ctx_t& ctx,
            std::uint16_t& seq,
            std::uint8_t subtype,
            const std::vector<std::uint8_t>& app,
            std::uint8_t* out,
            std::size_t out_cap) {
    return migris_pus11_execute(&ctx,
                                &sched,
                                test_apid,
                                &seq,
                                0x01020304U,
                                subtype,
                                test_source_id,
                                app.data(),
                                app.size(),
                                out,
                                out_cap);
}

TEST(Pus11, EnableAndDisableToggleTheSchedule) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_ENABLE, {}, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 1);
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_DISABLE, {}, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 0);
    EXPECT_EQ(seq, 0U);  // no telemetry → no sequence advance
}

TEST(Pus11, EnableRejectsTrailingApplicationData) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};
    const std::vector<std::uint8_t> trailing{0xFFU};
    EXPECT_EQ(
        execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_ENABLE, trailing, out.data(), out.size()),
        MIGRIS_PUS11_ERR_MALFORMED);
}

TEST(Pus11, ResetClearsEveryActivity) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto app = insert_app({{1000U, embedded_tc(1U, 7U)}, {2000U, embedded_tc(2U, 7U)}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, app, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    ASSERT_EQ(migris_schedule_count(&sched), 2U);

    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_RESET, {}, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);
}

TEST(Pus11, InsertSchedulesActivities) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto tc_a = embedded_tc(7U, 7U);
    const auto app = insert_app({{0xABCDU, tc_a}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, app, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);

    const auto id = request_id_of(tc_a);
    const migris_schedule_activity_t* found = migris_schedule_find(&sched, id.data());
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->release_time, 0xABCDU);
    EXPECT_EQ(found->tc_len, tc_a.size());
}

TEST(Pus11, InsertIsAtomicWhenABatchHasADuplicate) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    // Both items carry the same embedded TC → same request identifier.
    const auto dup = embedded_tc(9U, 7U);
    const auto app = insert_app({{100U, dup}, {200U, dup}});
    EXPECT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, app, out.data(), out.size()),
              MIGRIS_PUS11_ERR_DUPLICATE);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);  // nothing inserted
}

TEST(Pus11, InsertRejectsAnOversizedEmbeddedTc) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    // Embedded TC larger than MIGRIS_SCHEDULE_TC_MAX.
    const auto big = embedded_tc(1U, MIGRIS_SCHEDULE_TC_MAX);
    const auto app = insert_app({{100U, big}});
    EXPECT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, app, out.data(), out.size()),
              MIGRIS_PUS11_ERR_TC_TOO_LARGE);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);
}

TEST(Pus11, InsertRejectsMalformedAndOversizedRequests) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    // Count claims one activity but the release time is truncated.
    const std::vector<std::uint8_t> short_app{0x01U, 0x00U, 0x00U};
    EXPECT_EQ(
        execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, short_app, out.data(), out.size()),
        MIGRIS_PUS11_ERR_MALFORMED);

    // Count over the per-TC maximum.
    const std::vector<std::uint8_t> too_many{
        static_cast<std::uint8_t>(MIGRIS_PUS11_MAX_PER_TC + 1U)};
    EXPECT_EQ(
        execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, too_many, out.data(), out.size()),
        MIGRIS_PUS11_ERR_TOO_MANY);
}

TEST(Pus11, DeleteRemovesNamedActivities) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto tc_a = embedded_tc(1U, 7U);
    const auto tc_b = embedded_tc(2U, 7U);
    const auto ins = insert_app({{100U, tc_a}, {200U, tc_b}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, ins, out.data(), out.size()),
              MIGRIS_PUS11_OK);

    const auto del = id_list_app({request_id_of(tc_a)});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_DELETE, del, out.data(), out.size()),
              MIGRIS_PUS11_OK);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);
    const auto id_b = request_id_of(tc_b);
    EXPECT_NE(migris_schedule_find(&sched, id_b.data()), nullptr);  // survivor kept
}

TEST(Pus11, DeleteIsAtomicWhenAnIdIsUnknown) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto tc_a = embedded_tc(1U, 7U);
    const auto ins = insert_app({{100U, tc_a}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, ins, out.data(), out.size()),
              MIGRIS_PUS11_OK);

    // First id is scheduled, second is not → all-or-nothing.
    const auto del = id_list_app({request_id_of(tc_a), {0xDEU, 0xADU, 0xBEU, 0xEFU}});
    EXPECT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_DELETE, del, out.data(), out.size()),
              MIGRIS_PUS11_ERR_NOT_FOUND);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);  // nothing deleted
}

TEST(Pus11, SummaryReportListsRequestedActivities) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto tc_a = embedded_tc(1U, 7U);
    const auto tc_b = embedded_tc(2U, 7U);
    const auto ins = insert_app({{0x11111111U, tc_a}, {0x22222222U, tc_b}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, ins, out.data(), out.size()),
              MIGRIS_PUS11_OK);

    const auto req = id_list_app({request_id_of(tc_a), request_id_of(tc_b)});
    const int rc = execute(
        sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT_REQUEST, req, out.data(), out.size());
    // primary 6 + TM sec 10 + source (1 + 2*(4+4)) + CRC 2 = 35.
    ASSERT_EQ(rc, 35);

    const migris_pus_tm_secondary_header_t s = secondary_of(out.data());
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_SCHEDULING);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT);
    EXPECT_EQ(s.destination_id, test_source_id);
    EXPECT_EQ(primary_of(out.data()).seq_count, 0U);
    EXPECT_TRUE(crc_ok(out.data(), static_cast<std::size_t>(rc)));

    EXPECT_EQ(out[udf], 2U);                               // count
    EXPECT_EQ(u32_be(out.data(), udf + 1U), 0x11111111U);  // activity 1 release time
    EXPECT_EQ(u32_be(out.data(), udf + 9U), 0x22222222U);  // activity 2 release time
    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.msg_counter[0], 1U);
}

TEST(Pus11, SummaryReportOmitsUnknownIds) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    const auto tc_a = embedded_tc(1U, 7U);
    const auto ins = insert_app({{0x5000U, tc_a}});
    ASSERT_EQ(execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_INSERT, ins, out.data(), out.size()),
              MIGRIS_PUS11_OK);

    // One scheduled id, one that is not — the report carries only the
    // scheduled one (a report is a query, not all-or-nothing).
    const auto req = id_list_app({request_id_of(tc_a), {0x00U, 0x00U, 0x00U, 0x00U}});
    const int rc = execute(
        sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT_REQUEST, req, out.data(), out.size());
    ASSERT_GT(rc, 0);
    EXPECT_EQ(out[udf], 1U);  // count = 1, the unknown id omitted
    EXPECT_EQ(u32_be(out.data(), udf + 1U), 0x5000U);
}

TEST(Pus11, RejectsUnsupportedSubtype) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};

    // Subtype 12 is the [11,12] report — a TM subtype, never inbound.
    EXPECT_EQ(
        execute(sched, ctx, seq, MIGRIS_PUS11_SUBTYPE_SUMMARY_REPORT, {}, out.data(), out.size()),
        MIGRIS_PUS11_ERR_BAD_SUBTYPE);
    EXPECT_EQ(execute(sched, ctx, seq, 99U, {}, out.data(), out.size()),
              MIGRIS_PUS11_ERR_BAD_SUBTYPE);
}

TEST(Pus11, RejectsNullArguments) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_pus11_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS11_TM_MAX_PACKET_SIZE> out{};
    const std::array<std::uint8_t, 1U> app{0U};

    EXPECT_EQ(migris_pus11_execute(nullptr,
                                   &sched,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS11_SUBTYPE_ENABLE,
                                   test_source_id,
                                   app.data(),
                                   0U,
                                   out.data(),
                                   out.size()),
              MIGRIS_PUS11_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus11_execute(&ctx,
                                   nullptr,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS11_SUBTYPE_ENABLE,
                                   test_source_id,
                                   app.data(),
                                   0U,
                                   out.data(),
                                   out.size()),
              MIGRIS_PUS11_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
