// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// PUS-13 large data transfer (downlink) — part-packet codec.
// Pure (gtest-free) helpers build ramp payloads and decode a part
// packet, keeping the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise the part-header
// layout, the subtype derived from the part's position, verbatim
// payload copy, per-subtype message counters, the shared sequence
// count, and the buffer / argument rejections.

#include "migris/fsw/pus/pus13.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;
constexpr std::uint16_t test_dest = 0xBEEFU;
constexpr std::uint16_t test_txn = 0x1234U;

// Part-header offset: primary (6) + PUS-C TM secondary (10). The part
// payload follows the 6-byte part header.
constexpr std::size_t part_hdr_off =
    MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
constexpr std::size_t payload_off = part_hdr_off + MIGRIS_PUS13_PART_HEADER_SIZE;

// A `len`-byte ramp payload: byte i is `base + i`, so a decoded part
// payload is identifiable.
std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t base = 0U) {
    std::vector<std::uint8_t> v(len);
    for (std::size_t i = 0U; i < len; ++i) {
        v[i] = static_cast<std::uint8_t>(base + i);
    }
    return v;
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

std::uint16_t u16_be(const std::uint8_t* pkt, std::size_t at) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[at]) << 8) |
                                      static_cast<std::uint16_t>(pkt[at + 1U]));
}

// Build one part against the codec under test.
int build_part(migris_pus13_ctx_t& ctx,
               std::uint16_t& seq,
               std::uint16_t part_number,
               std::uint16_t total_parts,
               const std::vector<std::uint8_t>& payload,
               std::uint8_t* out,
               std::size_t out_cap) {
    return migris_pus13_build_part(&ctx,
                                   test_apid,
                                   &seq,
                                   0x01020304U,
                                   test_dest,
                                   test_txn,
                                   part_number,
                                   total_parts,
                                   payload.data(),
                                   payload.size(),
                                   out,
                                   out_cap);
}

TEST(Pus13, FirstPartHasTheExpectedLayout) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 7U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};

    const int rc =
        build_part(ctx, seq, 0U, 4U, ramp(MIGRIS_PUS13_PART_SIZE), out.data(), out.size());
    ASSERT_EQ(rc, static_cast<int>(24U + MIGRIS_PUS13_PART_SIZE));

    const migris_pus_tm_secondary_header_t s = secondary_of(out.data());
    EXPECT_EQ(s.service_type, MIGRIS_PUS_SERVICE_LARGE_DATA);
    EXPECT_EQ(s.service_subtype, MIGRIS_PUS13_SUBTYPE_FIRST_PART);
    EXPECT_EQ(s.destination_id, test_dest);
    EXPECT_EQ(u16_be(out.data(), part_hdr_off), test_txn);
    EXPECT_EQ(u16_be(out.data(), part_hdr_off + 2U), 0U);  // part number
    EXPECT_EQ(u16_be(out.data(), part_hdr_off + 4U), 4U);  // total parts
    EXPECT_TRUE(crc_ok(out.data(), static_cast<std::size_t>(rc)));
    EXPECT_EQ(seq, 8U);  // one packet emitted → sequence advanced
}

TEST(Pus13, PartSubtypeFollowsThePartPosition) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    const auto payload = ramp(8U);

    build_part(ctx, seq, 0U, 4U, payload, out.data(), out.size());
    EXPECT_EQ(secondary_of(out.data()).service_subtype, MIGRIS_PUS13_SUBTYPE_FIRST_PART);
    build_part(ctx, seq, 2U, 4U, payload, out.data(), out.size());
    EXPECT_EQ(secondary_of(out.data()).service_subtype, MIGRIS_PUS13_SUBTYPE_INTERMEDIATE_PART);
    build_part(ctx, seq, 3U, 4U, payload, out.data(), out.size());
    EXPECT_EQ(secondary_of(out.data()).service_subtype, MIGRIS_PUS13_SUBTYPE_LAST_PART);
}

TEST(Pus13, SinglePartTransferIsOneLastPart) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};

    build_part(ctx, seq, 0U, 1U, ramp(8U), out.data(), out.size());
    EXPECT_EQ(secondary_of(out.data()).service_subtype, MIGRIS_PUS13_SUBTYPE_LAST_PART);
    EXPECT_EQ(u16_be(out.data(), part_hdr_off + 2U), 0U);  // part number
    EXPECT_EQ(u16_be(out.data(), part_hdr_off + 4U), 1U);  // total parts
}

TEST(Pus13, PayloadIsCopiedVerbatim) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    const auto payload = ramp(MIGRIS_PUS13_PART_SIZE, 0x40U);

    const int rc = build_part(ctx, seq, 1U, 4U, payload, out.data(), out.size());
    ASSERT_GT(rc, 0);
    EXPECT_TRUE(std::equal(
        payload.begin(), payload.end(), out.begin() + static_cast<std::ptrdiff_t>(payload_off)));
}

TEST(Pus13, MessageCountersAreIndependentPerSubtype) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 100U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    const auto payload = ramp(8U);

    // A three-part transfer emits one of each subtype.
    build_part(ctx, seq, 0U, 3U, payload, out.data(), out.size());  // [13,1]
    build_part(ctx, seq, 1U, 3U, payload, out.data(), out.size());  // [13,2]
    build_part(ctx, seq, 2U, 3U, payload, out.data(), out.size());  // [13,3]
    EXPECT_EQ(ctx.msg_counter[0], 1U);
    EXPECT_EQ(ctx.msg_counter[1], 1U);
    EXPECT_EQ(ctx.msg_counter[2], 1U);
    EXPECT_EQ(seq, 103U);  // three packets share the per-APID sequence
}

TEST(Pus13, RejectsAnUndersizedBuffer) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, 20U> small{};
    EXPECT_EQ(build_part(ctx, seq, 0U, 1U, ramp(8U), small.data(), small.size()),
              MIGRIS_PUS13_ERR_BUF_TOO_SMALL);
}

TEST(Pus13, RejectsBadPartIndicesAndPayloadLengths) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};

    EXPECT_EQ(build_part(ctx, seq, 0U, 0U, ramp(8U), out.data(), out.size()),
              MIGRIS_PUS13_ERR_BAD_ARG);  // total_parts 0
    EXPECT_EQ(build_part(ctx, seq, 4U, 4U, ramp(8U), out.data(), out.size()),
              MIGRIS_PUS13_ERR_BAD_ARG);  // part_number not below total_parts
    EXPECT_EQ(build_part(ctx, seq, 0U, 1U, ramp(0U), out.data(), out.size()),
              MIGRIS_PUS13_ERR_BAD_ARG);  // empty payload
    EXPECT_EQ(
        build_part(ctx, seq, 0U, 1U, ramp(MIGRIS_PUS13_PART_SIZE + 1U), out.data(), out.size()),
        MIGRIS_PUS13_ERR_BAD_ARG);  // payload over the part size
}

TEST(Pus13, RejectsNullArguments) {
    migris_pus13_ctx_t ctx{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    const auto payload = ramp(8U);

    EXPECT_EQ(migris_pus13_build_part(nullptr,
                                      test_apid,
                                      &seq,
                                      0U,
                                      test_dest,
                                      test_txn,
                                      0U,
                                      1U,
                                      payload.data(),
                                      payload.size(),
                                      out.data(),
                                      out.size()),
              MIGRIS_PUS13_ERR_BAD_ARG);
    EXPECT_EQ(migris_pus13_build_part(&ctx,
                                      test_apid,
                                      &seq,
                                      0U,
                                      test_dest,
                                      test_txn,
                                      0U,
                                      1U,
                                      nullptr,
                                      8U,
                                      out.data(),
                                      out.size()),
              MIGRIS_PUS13_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
