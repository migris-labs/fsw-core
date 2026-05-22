// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// Large-data downlink session — the stateful half of PUS-13.
// Pure (gtest-free) helpers drain a session and reassemble the unit,
// keeping the test bodies inside the clang-tidy cognitive-complexity
// budget; the tests exercise the part-count arithmetic, in-order part
// emission, the round-trip reassembly of a borrowed data unit, the
// return-to-IDLE on completion, message counters that persist across
// transfers, and the argument / buffer / busy rejections.

#include "migris/fsw/largedata/largedata.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus13.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::largedata::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// A part's payload begins after primary (6) + TM secondary (10) +
// part header (6); a part packet ends with a 2-byte CRC.
constexpr std::size_t payload_off = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE +
                                    MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE +
                                    MIGRIS_PUS13_PART_HEADER_SIZE;

// A `len`-byte ramp: byte i is `base + i`, so a reassembled unit is
// verifiable against the original.
std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t base = 0U) {
    std::vector<std::uint8_t> v(len);
    for (std::size_t i = 0U; i < len; ++i) {
        v[i] = static_cast<std::uint8_t>(base + i);
    }
    return v;
}

// Drain an active session to completion, returning the concatenation
// of every emitted part's payload — the reassembled data unit.
std::vector<std::uint8_t> drain_reassemble(migris_largedata_session_t& session) {
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    std::uint16_t seq = 0U;
    std::vector<std::uint8_t> unit;
    for (;;) {
        const int rc =
            migris_largedata_next_part(&session, test_apid, &seq, 0U, 0U, out.data(), out.size());
        if (rc <= 0) {
            break;
        }
        const std::size_t payload_len = static_cast<std::size_t>(rc) - payload_off - 2U;
        for (std::size_t i = 0U; i < payload_len; ++i) {
            unit.push_back(out[payload_off + i]);
        }
    }
    return unit;
}

// Drain an active session, returning each emitted part's part number
// (part header bytes [2:4]) in delivery order.
std::vector<int> drain_part_numbers(migris_largedata_session_t& session) {
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    std::uint16_t seq = 0U;
    std::vector<int> parts;
    constexpr std::size_t pn_off =
        MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + 2U;
    for (;;) {
        const int rc =
            migris_largedata_next_part(&session, test_apid, &seq, 0U, 0U, out.data(), out.size());
        if (rc <= 0) {
            break;
        }
        parts.push_back((static_cast<int>(out[pn_off]) << 8) | static_cast<int>(out[pn_off + 1U]));
    }
    return parts;
}

TEST(LargeData, InitLeavesTheSessionIdle) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    EXPECT_EQ(migris_largedata_active(&session), 0);
}

TEST(LargeData, StartComputesThePartCount) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(200U);  // 200 / 64 → ceil 4

    ASSERT_EQ(migris_largedata_start(&session, 1U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(session.total_parts, 4U);
    EXPECT_EQ(migris_largedata_active(&session), 1);
}

TEST(LargeData, PartCountForAnExactMultipleAndASinglePart) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);

    const auto exact = ramp(static_cast<std::size_t>(2U) * MIGRIS_PUS13_PART_SIZE);
    ASSERT_EQ(migris_largedata_start(&session, 1U, exact.data(), exact.size()),
              MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(session.total_parts, 2U);

    migris_largedata_init(&session);
    const auto single = ramp(10U);
    ASSERT_EQ(migris_largedata_start(&session, 1U, single.data(), single.size()),
              MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(session.total_parts, 1U);
}

TEST(LargeData, PartsAreEmittedInOrder) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(200U);

    ASSERT_EQ(migris_largedata_start(&session, 1U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(drain_part_numbers(session), (std::vector<int>{0, 1, 2, 3}));
}

TEST(LargeData, RoundTripReassemblesTheBorrowedUnit) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(200U, 0x11U);

    ASSERT_EQ(migris_largedata_start(&session, 0xABCDU, unit.data(), unit.size()),
              MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(drain_reassemble(session), unit);
    EXPECT_EQ(migris_largedata_active(&session), 0);  // last part returned it to IDLE
}

TEST(LargeData, NextPartReturnsZeroWhenIdle) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    std::array<std::uint8_t, MIGRIS_PUS13_PART_PACKET_MAX> out{};
    std::uint16_t seq = 0U;
    EXPECT_EQ(migris_largedata_next_part(&session, test_apid, &seq, 0U, 0U, out.data(), out.size()),
              0);
}

TEST(LargeData, NextPartRejectsAnUndersizedBufferAndKeepsTheSession) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(200U);
    ASSERT_EQ(migris_largedata_start(&session, 1U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);

    std::array<std::uint8_t, 20U> small{};
    std::uint16_t seq = 0U;
    EXPECT_EQ(
        migris_largedata_next_part(&session, test_apid, &seq, 0U, 0U, small.data(), small.size()),
        MIGRIS_LARGEDATA_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(migris_largedata_active(&session), 1);  // unchanged — caller may retry
}

TEST(LargeData, StartRejectsNullAndEmptyArguments) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(64U);

    EXPECT_EQ(migris_largedata_start(nullptr, 1U, unit.data(), unit.size()),
              MIGRIS_LARGEDATA_ERR_BAD_ARG);
    EXPECT_EQ(migris_largedata_start(&session, 1U, nullptr, 8U), MIGRIS_LARGEDATA_ERR_BAD_ARG);
    EXPECT_EQ(migris_largedata_start(&session, 1U, unit.data(), 0U), MIGRIS_LARGEDATA_ERR_BAD_ARG);
}

TEST(LargeData, StartRejectsAnOversizedUnitAndABusySession) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);

    const std::vector<std::uint8_t> oversized(MIGRIS_LARGEDATA_UNIT_MAX + 1U, 0U);
    EXPECT_EQ(migris_largedata_start(&session, 1U, oversized.data(), oversized.size()),
              MIGRIS_LARGEDATA_ERR_UNIT_TOO_LARGE);

    const auto unit = ramp(200U);
    ASSERT_EQ(migris_largedata_start(&session, 1U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);
    EXPECT_EQ(migris_largedata_start(&session, 2U, unit.data(), unit.size()),
              MIGRIS_LARGEDATA_ERR_BUSY);
}

TEST(LargeData, MessageCountersPersistAcrossTransfers) {
    migris_largedata_session_t session{};
    migris_largedata_init(&session);
    const auto unit = ramp(200U);  // 4 parts: [13,1], [13,2], [13,2], [13,3]

    ASSERT_EQ(migris_largedata_start(&session, 1U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);
    (void)drain_reassemble(session);
    ASSERT_EQ(migris_largedata_start(&session, 2U, unit.data(), unit.size()), MIGRIS_LARGEDATA_OK);
    (void)drain_reassemble(session);

    EXPECT_EQ(session.pus13.msg_counter[0], 2U);  // [13,1] — once per transfer
    EXPECT_EQ(session.pus13.msg_counter[1], 4U);  // [13,2] — twice per transfer
    EXPECT_EQ(session.pus13.msg_counter[2], 2U);  // [13,3] — once per transfer
}

}  // namespace
}  // namespace migris::fsw::largedata::test
