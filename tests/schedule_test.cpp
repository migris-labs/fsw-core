// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board schedule — bounded, time-tagged TC store.
// Pure (gtest-free) helpers build fake telecommands and keep the test
// bodies inside the clang-tidy cognitive-complexity budget; the tests
// exercise insert / delete / find, the capacity / duplicate / size
// guards, the enable gate, and the earliest-release-first pop order.

#include "migris/fsw/schedule/schedule.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::schedule::test {
namespace {

// A fake telecommand: a 4-byte big-endian request-id prefix followed
// by filler. The schedule store treats the TC as opaque except for
// tc[0..3] (the request identifier).
std::vector<std::uint8_t> fake_tc(std::uint32_t request_id, std::size_t len) {
    std::vector<std::uint8_t> tc(len, 0xEEU);
    tc[0] = static_cast<std::uint8_t>(request_id >> 24);
    tc[1] = static_cast<std::uint8_t>(request_id >> 16);
    tc[2] = static_cast<std::uint8_t>(request_id >> 8);
    tc[3] = static_cast<std::uint8_t>(request_id & 0xFFU);
    return tc;
}

// The 4-byte request identifier matching fake_tc(request_id, ...).
std::array<std::uint8_t, 4U> req_id(std::uint32_t request_id) {
    return {
        static_cast<std::uint8_t>(request_id >> 24),
        static_cast<std::uint8_t>(request_id >> 16),
        static_cast<std::uint8_t>(request_id >> 8),
        static_cast<std::uint8_t>(request_id & 0xFFU),
    };
}

// Insert fake_tc(request_id, len) at release_time; return the status.
int insert(migris_schedule_t& sched,
           std::uint32_t request_id,
           std::uint32_t release_time,
           std::size_t len = 8U) {
    const auto tc = fake_tc(request_id, len);
    return migris_schedule_insert(&sched, release_time, tc.data(), tc.size());
}

TEST(Schedule, InitIsEmptyAndDisabled) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 0);
}

TEST(Schedule, SetEnabledToggles) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 1);
    migris_schedule_set_enabled(&sched, 0);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 0);
}

TEST(Schedule, InsertStoresActivityAndFindRetrievesIt) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    ASSERT_EQ(insert(sched, 0xAABBCCDDU, 1000U), MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);

    const auto id = req_id(0xAABBCCDDU);
    const migris_schedule_activity_t* found = migris_schedule_find(&sched, id.data());
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->release_time, 1000U);
    EXPECT_EQ(found->tc_len, 8U);
}

TEST(Schedule, InsertRejectsBadArguments) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    const auto tc = fake_tc(0x1U, 8U);
    EXPECT_EQ(migris_schedule_insert(nullptr, 0U, tc.data(), tc.size()),
              MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_insert(&sched, 0U, nullptr, 8U), MIGRIS_SCHEDULE_ERR_BAD_ARG);
    // A TC shorter than the 4-byte request identifier.
    EXPECT_EQ(migris_schedule_insert(&sched, 0U, tc.data(), 3U), MIGRIS_SCHEDULE_ERR_BAD_ARG);
}

TEST(Schedule, InsertRejectsOversizedTc) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    EXPECT_EQ(insert(sched, 0x1U, 0U, MIGRIS_SCHEDULE_TC_MAX + 1U),
              MIGRIS_SCHEDULE_ERR_TC_TOO_LARGE);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);
}

TEST(Schedule, InsertRejectsDuplicateRequestId) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    ASSERT_EQ(insert(sched, 0x4242U, 100U), MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(insert(sched, 0x4242U, 200U), MIGRIS_SCHEDULE_ERR_DUPLICATE);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);
}

TEST(Schedule, InsertRejectsWhenFull) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    for (std::uint32_t i = 0U; i < MIGRIS_SCHEDULE_CAPACITY; ++i) {
        ASSERT_EQ(insert(sched, 0x1000U + i, 10U * i), MIGRIS_SCHEDULE_OK);
    }
    EXPECT_EQ(migris_schedule_count(&sched), static_cast<std::size_t>(MIGRIS_SCHEDULE_CAPACITY));
    EXPECT_EQ(insert(sched, 0x9999U, 0U), MIGRIS_SCHEDULE_ERR_FULL);
}

TEST(Schedule, FindReturnsNullForUnknownRequestId) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    ASSERT_EQ(insert(sched, 0x1U, 0U), MIGRIS_SCHEDULE_OK);
    const auto missing = req_id(0x2U);
    EXPECT_EQ(migris_schedule_find(&sched, missing.data()), nullptr);
    EXPECT_EQ(migris_schedule_find(nullptr, missing.data()), nullptr);
}

TEST(Schedule, DeleteRemovesActivity) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    ASSERT_EQ(insert(sched, 0xAU, 10U), MIGRIS_SCHEDULE_OK);
    ASSERT_EQ(insert(sched, 0xBU, 20U), MIGRIS_SCHEDULE_OK);

    const auto id_a = req_id(0xAU);
    ASSERT_EQ(migris_schedule_delete(&sched, id_a.data()), MIGRIS_SCHEDULE_OK);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);
    EXPECT_EQ(migris_schedule_find(&sched, id_a.data()), nullptr);
    // The surviving activity is still retrievable (gap-fill kept it).
    const auto id_b = req_id(0xBU);
    EXPECT_NE(migris_schedule_find(&sched, id_b.data()), nullptr);
}

TEST(Schedule, DeleteRejectsUnknownRequestId) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    ASSERT_EQ(insert(sched, 0xAU, 10U), MIGRIS_SCHEDULE_OK);
    const auto missing = req_id(0xFFU);
    EXPECT_EQ(migris_schedule_delete(&sched, missing.data()), MIGRIS_SCHEDULE_ERR_NOT_FOUND);
    EXPECT_EQ(migris_schedule_delete(&sched, nullptr), MIGRIS_SCHEDULE_ERR_BAD_ARG);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);
}

TEST(Schedule, ResetClearsActivitiesButKeepsEnabledState) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    ASSERT_EQ(insert(sched, 0xAU, 10U), MIGRIS_SCHEDULE_OK);
    ASSERT_EQ(insert(sched, 0xBU, 20U), MIGRIS_SCHEDULE_OK);

    migris_schedule_reset(&sched);
    EXPECT_EQ(migris_schedule_count(&sched), 0U);
    EXPECT_EQ(migris_schedule_is_enabled(&sched), 1);  // enable state untouched
}

TEST(Schedule, PopDueReturnsZeroWhileDisabled) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);  // disabled by default
    ASSERT_EQ(insert(sched, 0xAU, 10U), MIGRIS_SCHEDULE_OK);

    std::array<std::uint8_t, MIGRIS_SCHEDULE_TC_MAX> out{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_schedule_pop_due(&sched, 1000U, out.data(), out.size(), &out_len), 0);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);  // retained
}

TEST(Schedule, PopDueReturnsZeroWhenNothingIsDue) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    ASSERT_EQ(insert(sched, 0xAU, 5000U), MIGRIS_SCHEDULE_OK);

    std::array<std::uint8_t, MIGRIS_SCHEDULE_TC_MAX> out{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_schedule_pop_due(&sched, 4999U, out.data(), out.size(), &out_len), 0);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);
}

TEST(Schedule, PopDueReleasesAndRemovesADueActivity) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    const auto tc = fake_tc(0xCAFEU, 12U);
    ASSERT_EQ(migris_schedule_insert(&sched, 100U, tc.data(), tc.size()), MIGRIS_SCHEDULE_OK);

    std::array<std::uint8_t, MIGRIS_SCHEDULE_TC_MAX> out{};
    std::size_t out_len = 0U;
    // now == release time → due.
    ASSERT_EQ(migris_schedule_pop_due(&sched, 100U, out.data(), out.size(), &out_len), 1);
    EXPECT_EQ(out_len, tc.size());
    EXPECT_TRUE(std::equal(tc.begin(), tc.end(), out.begin()));
    EXPECT_EQ(migris_schedule_count(&sched), 0U);  // released → removed
}

TEST(Schedule, PopDueReleasesEarliestReleaseTimeFirst) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    // Inserted out of time order.
    ASSERT_EQ(insert(sched, 0x30U, 300U), MIGRIS_SCHEDULE_OK);
    ASSERT_EQ(insert(sched, 0x10U, 100U), MIGRIS_SCHEDULE_OK);
    ASSERT_EQ(insert(sched, 0x20U, 200U), MIGRIS_SCHEDULE_OK);

    std::array<std::uint8_t, MIGRIS_SCHEDULE_TC_MAX> out{};
    std::size_t out_len = 0U;
    std::vector<std::uint32_t> released;
    while (migris_schedule_pop_due(&sched, 1000U, out.data(), out.size(), &out_len) == 1) {
        released.push_back((static_cast<std::uint32_t>(out[0]) << 24) |
                           (static_cast<std::uint32_t>(out[1]) << 16) |
                           (static_cast<std::uint32_t>(out[2]) << 8) |
                           static_cast<std::uint32_t>(out[3]));
    }
    EXPECT_EQ(released, (std::vector<std::uint32_t>{0x10U, 0x20U, 0x30U}));
}

TEST(Schedule, PopDueTreatsAPastReleaseTimeAsDue) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    ASSERT_EQ(insert(sched, 0xAU, 50U), MIGRIS_SCHEDULE_OK);  // release time already past

    std::array<std::uint8_t, MIGRIS_SCHEDULE_TC_MAX> out{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_schedule_pop_due(&sched, 10000U, out.data(), out.size(), &out_len), 1);
}

TEST(Schedule, PopDueRejectsAnUndersizedBufferAndKeepsTheActivity) {
    migris_schedule_t sched{};
    migris_schedule_init(&sched);
    migris_schedule_set_enabled(&sched, 1);
    ASSERT_EQ(insert(sched, 0xAU, 100U, 20U), MIGRIS_SCHEDULE_OK);

    std::array<std::uint8_t, 10U> small{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_schedule_pop_due(&sched, 200U, small.data(), small.size(), &out_len),
              MIGRIS_SCHEDULE_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(migris_schedule_count(&sched), 1U);  // left scheduled
}

}  // namespace
}  // namespace migris::fsw::schedule::test
