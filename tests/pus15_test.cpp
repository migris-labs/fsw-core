// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// PUS-15 on-board storage and retrieval — service handler.
// Pure (gtest-free) helpers build the TC application data and decode
// the [15,13] report, keeping the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise enable/disable,
// the by-time-period downlink (arming a retrieval), delete-up-to-time,
// the packet store report, and the malformed / retrieval-active /
// bad-subtype rejections against a pre-loaded packet store fixture.

#include "migris/fsw/pus/pus15.h"

#include "migris/fsw/pktstore/pktstore.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;
constexpr std::uint16_t test_source_id = 0xCAFEU;

// Source-data offset: primary (6) + PUS-C TM secondary (10).
constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

// Store one fake telemetry packet (8 filler bytes) at `storage_time`.
void preload(migris_pktstore_t& store, std::uint32_t storage_time) {
    const std::array<std::uint8_t, 8U> pkt{0xEEU, 0xEEU, 0xEEU, 0xEEU, 0xEEU, 0xEEU, 0xEEU, 0xEEU};
    migris_pktstore_store(&store, pkt.data(), pkt.size(), storage_time);
}

// [15,9] application data: 4-byte from-time + 4-byte to-time.
std::vector<std::uint8_t> window_app(std::uint32_t from_time, std::uint32_t to_time) {
    return {
        static_cast<std::uint8_t>(from_time >> 24),
        static_cast<std::uint8_t>(from_time >> 16),
        static_cast<std::uint8_t>(from_time >> 8),
        static_cast<std::uint8_t>(from_time & 0xFFU),
        static_cast<std::uint8_t>(to_time >> 24),
        static_cast<std::uint8_t>(to_time >> 16),
        static_cast<std::uint8_t>(to_time >> 8),
        static_cast<std::uint8_t>(to_time & 0xFFU),
    };
}

// [15,11] application data: a single 4-byte time.
std::vector<std::uint8_t> time_app(std::uint32_t time_seconds) {
    return {
        static_cast<std::uint8_t>(time_seconds >> 24),
        static_cast<std::uint8_t>(time_seconds >> 16),
        static_cast<std::uint8_t>(time_seconds >> 8),
        static_cast<std::uint8_t>(time_seconds & 0xFFU),
    };
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

// Execute one PUS-15 TC against the store fixture.
int execute(migris_pktstore_t& store,
            migris_pus15_ctx_t& ctx,
            std::uint16_t& seq,
            std::uint8_t subtype,
            const std::vector<std::uint8_t>& app,
            std::uint8_t* out,
            std::size_t out_cap) {
    return migris_pus15_execute(&ctx,
                                &store,
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

TEST(Pus15, EnableAndDisableStorage) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    ASSERT_EQ(
        execute(store, ctx, seq, MIGRIS_PUS15_SUBTYPE_DISABLE_STORAGE, {}, out.data(), out.size()),
        MIGRIS_PUS15_OK);
    EXPECT_EQ(migris_pktstore_is_enabled(&store), 0);
    ASSERT_EQ(
        execute(store, ctx, seq, MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE, {}, out.data(), out.size()),
        MIGRIS_PUS15_OK);
    EXPECT_EQ(migris_pktstore_is_enabled(&store), 1);
    EXPECT_EQ(seq, 0U);  // no telemetry → no sequence advance
}

TEST(Pus15, EnableRejectsTrailingApplicationData) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};
    const std::vector<std::uint8_t> trailing{0xFFU};
    EXPECT_EQ(
        execute(
            store, ctx, seq, MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE, trailing, out.data(), out.size()),
        MIGRIS_PUS15_ERR_MALFORMED);
}

TEST(Pus15, DownlinkArmsARetrieval) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    preload(store, 100U);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    ASSERT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      window_app(0U, 1000U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_OK);
    EXPECT_EQ(migris_pktstore_retrieval_active(&store), 1);  // armed, not drained here
}

TEST(Pus15, DownlinkRejectsMalformedAndInvertedWindows) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    // Application data shorter than the two 4-byte times.
    const std::vector<std::uint8_t> short_app{0x00U, 0x00U, 0x00U};
    EXPECT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      short_app,
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_ERR_MALFORMED);
    // from-time after to-time — an inverted window.
    EXPECT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      window_app(500U, 100U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_ERR_MALFORMED);
}

TEST(Pus15, DownlinkRejectedWhileARetrievalIsActive) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    preload(store, 100U);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    ASSERT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      window_app(0U, 1000U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_OK);
    // A second downlink while the first is still in progress.
    EXPECT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      window_app(0U, 1000U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_ERR_RETRIEVAL_ACTIVE);
}

TEST(Pus15, DeleteRemovesPacketsUpToTime) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    preload(store, 10U);
    preload(store, 20U);
    preload(store, 30U);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    ASSERT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DELETE_RANGE,
                      time_app(20U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_OK);
    EXPECT_EQ(migris_pktstore_count(&store), 1U);  // times 10 and 20 removed
}

TEST(Pus15, DeleteRejectsMalformedApplicationData) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};
    const std::vector<std::uint8_t> short_app{0x00U, 0x00U};
    EXPECT_EQ(
        execute(
            store, ctx, seq, MIGRIS_PUS15_SUBTYPE_DELETE_RANGE, short_app, out.data(), out.size()),
        MIGRIS_PUS15_ERR_MALFORMED);
}

TEST(Pus15, DeleteRejectedWhileARetrievalIsActive) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    preload(store, 100U);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    ASSERT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DOWNLINK_RANGE,
                      window_app(0U, 1000U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_OK);
    EXPECT_EQ(execute(store,
                      ctx,
                      seq,
                      MIGRIS_PUS15_SUBTYPE_DELETE_RANGE,
                      time_app(500U),
                      out.data(),
                      out.size()),
              MIGRIS_PUS15_ERR_RETRIEVAL_ACTIVE);
}

TEST(Pus15, ReportReflectsTheStoreState) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    preload(store, 0x00001000U);
    preload(store, 0x00002000U);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    const int rc =
        execute(store, ctx, seq, MIGRIS_PUS15_SUBTYPE_REPORT_REQUEST, {}, out.data(), out.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE));

    const migris_pus_tm_secondary_header_t s = secondary_of(out.data());
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_STORAGE);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS15_SUBTYPE_STORE_REPORT);
    EXPECT_EQ(s.destination_id, test_source_id);
    EXPECT_TRUE(crc_ok(out.data(), static_cast<std::size_t>(rc)));

    EXPECT_EQ(out[udf], 1U);  // storage enabled
    EXPECT_EQ((static_cast<std::uint16_t>(out[udf + 1U]) << 8) | out[udf + 2U], 2U);  // count
    EXPECT_EQ(u32_be(out.data(), udf + 3U), 0x00001000U);                             // oldest
    EXPECT_EQ(u32_be(out.data(), udf + 7U), 0x00002000U);                             // newest
    EXPECT_EQ(seq, 1U);
    EXPECT_EQ(ctx.msg_counter[0], 1U);
}

TEST(Pus15, ReportOnAnEmptyStore) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    const int rc =
        execute(store, ctx, seq, MIGRIS_PUS15_SUBTYPE_REPORT_REQUEST, {}, out.data(), out.size());
    ASSERT_EQ(rc, static_cast<int>(MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE));
    EXPECT_EQ(out[udf], 1U);  // enabled by default
    EXPECT_EQ((static_cast<std::uint16_t>(out[udf + 1U]) << 8) | out[udf + 2U], 0U);  // count 0
    EXPECT_EQ(u32_be(out.data(), udf + 3U), 0U);  // oldest 0 on an empty store
    EXPECT_EQ(u32_be(out.data(), udf + 7U), 0U);  // newest 0
}

TEST(Pus15, RejectsUnsupportedSubtype) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};

    // Subtype 13 is the [15,13] report — a TM subtype, never inbound.
    EXPECT_EQ(
        execute(store, ctx, seq, MIGRIS_PUS15_SUBTYPE_STORE_REPORT, {}, out.data(), out.size()),
        MIGRIS_PUS15_ERR_BAD_SUBTYPE);
    EXPECT_EQ(execute(store, ctx, seq, 99U, {}, out.data(), out.size()),
              MIGRIS_PUS15_ERR_BAD_SUBTYPE);
}

TEST(Pus15, RejectsNullArguments) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pus15_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS15_STORE_REPORT_PACKET_SIZE> out{};
    const std::array<std::uint8_t, 1U> app{0U};

    EXPECT_EQ(migris_pus15_execute(nullptr,
                                   &store,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE,
                                   test_source_id,
                                   app.data(),
                                   0U,
                                   out.data(),
                                   out.size()),
              MIGRIS_PUS15_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus15_execute(&ctx,
                                   nullptr,
                                   test_apid,
                                   &seq,
                                   0U,
                                   MIGRIS_PUS15_SUBTYPE_ENABLE_STORAGE,
                                   test_source_id,
                                   app.data(),
                                   0U,
                                   out.data(),
                                   out.size()),
              MIGRIS_PUS15_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
