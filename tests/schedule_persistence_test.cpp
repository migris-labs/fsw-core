// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board schedule — flash-backed persistence (slice fsw-17).
// Pure (gtest-free) builders keep each test body inside the clang-tidy
// cognitive-complexity budget; the tests exercise the variable-length
// per-entry codec, the round-trip across re-init, the truncation /
// over-capacity / out-of-range tc_len guards, and the generation
// counter's mutation-vs-restore contract.

#include "migris/fsw/schedule/schedule.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::schedule::test {
namespace {

// A fake telecommand with the request identifier `request_id` packed
// big-endian into the first 4 bytes, the rest filled with `fill`.
std::vector<std::uint8_t>
make_tc(std::uint32_t request_id, std::size_t len, std::uint8_t fill = 0xCDU) {
    std::vector<std::uint8_t> tc(len, fill);
    tc[0] = static_cast<std::uint8_t>(request_id >> 24);
    tc[1] = static_cast<std::uint8_t>(request_id >> 16);
    tc[2] = static_cast<std::uint8_t>(request_id >> 8);
    tc[3] = static_cast<std::uint8_t>(request_id & 0xFFU);
    return tc;
}

// Build a schedule with `n` activities at release times `(i + 1) * 100`
// and TC lengths `4 + (i % 5)` — enough variation to exercise the
// per-entry tc_len encoding without saturating the capacity.
void seed(migris_schedule_t& sched, std::size_t n) {
    migris_schedule_init(&sched);
    for (std::size_t i = 0U; i < n; ++i) {
        const auto tc = make_tc(0x10000000U + static_cast<std::uint32_t>(i),
                                4U + (i % 5U),
                                static_cast<std::uint8_t>(0x10U + i));
        const std::uint32_t release_time = (static_cast<std::uint32_t>(i) + 1U) * 100U;
        ASSERT_EQ(migris_schedule_insert(&sched, release_time, tc.data(), tc.size()),
                  MIGRIS_SCHEDULE_OK);
    }
}

bool activities_equal(const migris_schedule_activity_t& a, const migris_schedule_activity_t& b) {
    if (a.release_time != b.release_time || a.tc_len != b.tc_len) {
        return false;
    }
    for (std::size_t i = 0U; i < a.tc_len; ++i) {
        if (a.tc[i] != b.tc[i]) {
            return false;
        }
    }
    return true;
}

// True iff `dst.activities[0..n]` matches `src.activities[0..n]`.
// Wraps the per-entry comparison loop in a single helper so the test
// body sees one EXPECT_TRUE rather than a per-entry assertion — the
// gtest macro expansion at the call site dominates cognitive
// complexity in clang-tidy, so a single call keeps the test body
// inside the budget.
bool first_n_activities_equal(const migris_schedule_t& dst,
                              const migris_schedule_t& src,
                              std::size_t n) {
    for (std::size_t i = 0U; i < n; ++i) {
        if (!activities_equal(dst.activities[i], src.activities[i])) {
            return false;
        }
    }
    return true;
}

TEST(SchedulePersistence, EmptyRoundTrip) {
    migris_schedule_t src{};
    migris_schedule_init(&src);
    std::array<std::uint8_t, 16U> buf{};
    const int n = migris_schedule_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);
    EXPECT_EQ(n, 3);  // count(2) + enabled(1) = 3 bytes for an empty schedule

    migris_schedule_t dst{};
    migris_schedule_init(&dst);
    ASSERT_EQ(migris_schedule_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(migris_schedule_count(&dst), 0U);
    EXPECT_EQ(migris_schedule_is_enabled(&dst), 0);
}

TEST(SchedulePersistence, RoundTripPreservesActivitiesAndEnabledFlag) {
    migris_schedule_t src{};
    seed(src, 3U);
    migris_schedule_set_enabled(&src, 1);
    ASSERT_EQ(migris_schedule_count(&src), 3U);

    std::array<std::uint8_t, 256U> buf{};
    const int n = migris_schedule_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_schedule_t dst{};
    migris_schedule_init(&dst);
    ASSERT_EQ(migris_schedule_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(migris_schedule_count(&dst), 3U);
    EXPECT_EQ(migris_schedule_is_enabled(&dst), 1);
    EXPECT_TRUE(first_n_activities_equal(dst, src, 3U));
}

TEST(SchedulePersistence, RoundTripPreservesDisabledState) {
    migris_schedule_t src{};
    seed(src, 1U);  // enabled stays at the post-init 0
    std::array<std::uint8_t, 64U> buf{};
    const int n = migris_schedule_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_schedule_t dst{};
    migris_schedule_init(&dst);
    migris_schedule_set_enabled(&dst, 1);  // pre-existing enabled state
    ASSERT_EQ(migris_schedule_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_SCHEDULE_OK);
    // Deserialize must overwrite, not OR-in, the enabled flag.
    EXPECT_EQ(migris_schedule_is_enabled(&dst), 0);
}

TEST(SchedulePersistence, SerializeRejectsTooSmallBuffer) {
    migris_schedule_t src{};
    seed(src, 2U);
    std::array<std::uint8_t, 4U> tiny{};  // not even header + first entry
    EXPECT_EQ(migris_schedule_serialize(&src, tiny.data(), tiny.size()),
              MIGRIS_SCHEDULE_ERR_BUF_TOO_SMALL);
}

TEST(SchedulePersistence, DeserializeRejectsTruncatedHeader) {
    migris_schedule_t dst{};
    const std::array<std::uint8_t, 2U> too_short{};  // header is 3 bytes (count + enabled)
    EXPECT_EQ(migris_schedule_deserialize(&dst, too_short.data(), too_short.size()),
              MIGRIS_SCHEDULE_ERR_TRUNCATED);
}

TEST(SchedulePersistence, DeserializeRejectsTruncatedEntryHeader) {
    // count=1, enabled=0, but no per-entry bytes follow.
    const std::array<std::uint8_t, 3U> image{0x00U, 0x01U, 0x00U};
    migris_schedule_t dst{};
    EXPECT_EQ(migris_schedule_deserialize(&dst, image.data(), image.size()),
              MIGRIS_SCHEDULE_ERR_TRUNCATED);
}

TEST(SchedulePersistence, DeserializeRejectsTruncatedEntryBody) {
    // count=1, enabled=0, release_time=0, tc_len=8 — but only 4 TC
    // bytes follow.
    const std::array<std::uint8_t, 15U> image{
        0x00U,
        0x01U,  // count = 1
        0x00U,  // enabled = 0
        0x00U,
        0x00U,
        0x00U,
        0x00U,  // release_time = 0
        0x00U,
        0x08U,  // tc_len = 8
        0xDEU,
        0xADU,
        0xBEU,
        0xEFU,
        0x00U,
        0x00U,  // 6 TC bytes (4 short of 8)
    };
    migris_schedule_t dst{};
    EXPECT_EQ(migris_schedule_deserialize(&dst, image.data(), image.size()),
              MIGRIS_SCHEDULE_ERR_TRUNCATED);
}

TEST(SchedulePersistence, DeserializeRejectsTcLenOverMax) {
    // count=1, enabled=0, release_time=0, tc_len = TC_MAX + 1.
    std::array<std::uint8_t, 9U> image{
        0x00U,
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };
    const auto tc_len = static_cast<std::uint16_t>(MIGRIS_SCHEDULE_TC_MAX + 1U);
    image[7] = static_cast<std::uint8_t>(tc_len >> 8);
    image[8] = static_cast<std::uint8_t>(tc_len & 0xFFU);
    migris_schedule_t dst{};
    EXPECT_EQ(migris_schedule_deserialize(&dst, image.data(), image.size()),
              MIGRIS_SCHEDULE_ERR_TC_TOO_LARGE);
}

TEST(SchedulePersistence, DeserializeRejectsTcLenBelowRequestIdSize) {
    // tc_len = 3 — below the 4-byte request-id floor.
    const std::array<std::uint8_t, 9U> image{
        0x00U,
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x03U,
    };
    migris_schedule_t dst{};
    EXPECT_EQ(migris_schedule_deserialize(&dst, image.data(), image.size()),
              MIGRIS_SCHEDULE_ERR_TC_TOO_LARGE);
}

TEST(SchedulePersistence, DeserializeRejectsCountOverCapacity) {
    // count = MIGRIS_SCHEDULE_CAPACITY + 1 — refused before any per-entry parsing.
    const auto overcount = static_cast<std::uint16_t>(MIGRIS_SCHEDULE_CAPACITY + 1U);
    const std::array<std::uint8_t, 3U> image{
        static_cast<std::uint8_t>(overcount >> 8),
        static_cast<std::uint8_t>(overcount & 0xFFU),
        0x00U,
    };
    migris_schedule_t dst{};
    EXPECT_EQ(migris_schedule_deserialize(&dst, image.data(), image.size()),
              MIGRIS_SCHEDULE_ERR_FULL);
}

TEST(SchedulePersistence, GenerationStartsAtZeroAndBumpsOnMutations) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    EXPECT_EQ(migris_schedule_generation(&sched), 0U);

    const auto tc = make_tc(0xAAAAAAAAU, 8U);
    ASSERT_EQ(migris_schedule_insert(&sched, 100U, tc.data(), tc.size()), MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(migris_schedule_generation(&sched), 1U);

    migris_schedule_set_enabled(&sched, 1);
    EXPECT_EQ(migris_schedule_generation(&sched), 2U);

    // A no-op set_enabled (same value) must NOT bump.
    migris_schedule_set_enabled(&sched, 1);
    EXPECT_EQ(migris_schedule_generation(&sched), 2U);

    // pop_due of a due activity bumps (it mutates the persisted set).
    std::array<std::uint8_t, 16U> out{};
    std::size_t out_len = 0U;
    ASSERT_EQ(migris_schedule_pop_due(&sched, 200U, out.data(), out.size(), &out_len), 1);
    EXPECT_EQ(migris_schedule_generation(&sched), 3U);

    // pop_due that releases nothing must NOT bump.
    EXPECT_EQ(migris_schedule_pop_due(&sched, 200U, out.data(), out.size(), &out_len), 0);
    EXPECT_EQ(migris_schedule_generation(&sched), 3U);

    // reset bumps.
    migris_schedule_reset(&sched);
    EXPECT_EQ(migris_schedule_generation(&sched), 4U);
}

TEST(SchedulePersistence, DeserializeDoesNotBumpGeneration) {
    migris_schedule_t src{};
    seed(src, 2U);
    std::array<std::uint8_t, 128U> buf{};
    const int n = migris_schedule_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_schedule_t dst{};
    migris_schedule_init(&dst);
    ASSERT_EQ(migris_schedule_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_SCHEDULE_OK);
    // Restore must not look like a mutation — the sample's "have I
    // changed since the last save?" loop would double-save on every boot.
    EXPECT_EQ(migris_schedule_generation(&dst), 0U);
}

TEST(SchedulePersistence, DeserializeFailureLeavesScheduleEmpty) {
    migris_schedule_t dst{};
    seed(dst, 2U);  // pre-populated to verify the stateless-failure contract
    const std::array<std::uint8_t, 2U> bad{};  // truncated header
    EXPECT_EQ(migris_schedule_deserialize(&dst, bad.data(), bad.size()),
              MIGRIS_SCHEDULE_ERR_TRUNCATED);
    EXPECT_EQ(migris_schedule_count(&dst), 0U);
    EXPECT_EQ(migris_schedule_is_enabled(&dst), 0);
}

TEST(SchedulePersistence, RejectsNullArgs) {
    migris_schedule_t sched{};
    std::array<std::uint8_t, 4U> buf{};
    EXPECT_EQ(migris_schedule_serialize(nullptr, buf.data(), buf.size()),
              MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_serialize(&sched, nullptr, buf.size()), MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_deserialize(nullptr, buf.data(), buf.size()),
              MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_deserialize(&sched, nullptr, buf.size()),
              MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_generation(nullptr), 0U);
}

}  // namespace
}  // namespace migris::fsw::schedule::test
