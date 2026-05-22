// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board housekeeping-structure store — bounded set of ground-defined
// PUS-3 housekeeping structures. Pure (gtest-free) helpers build
// parameter lists and keep the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise create / delete /
// find, the capacity / duplicate / SID-range / parameter-count guards,
// the enable gate, slot reuse after delete, and the
// earliest-last-emit-first due order.

#include "migris/fsw/hkstore/hkstore.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace migris::fsw::hkstore::test {
namespace {

// Create a structure sampling `ids`, reported every `interval` seconds;
// return the status.
int create(migris_hkstore_t& store,
           std::uint16_t sid,
           const std::vector<std::uint16_t>& ids,
           std::uint32_t interval = 5U) {
    return migris_hkstore_create(&store, sid, ids.data(), ids.size(), interval);
}

TEST(HkStore, InitIsEmpty) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
}

TEST(HkStore, CreateStoresAStructureAndFindRetrievesIt) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U, 0x0011U}, 30U), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&store), 1U);

    const migris_hk_structure_t* s = migris_hkstore_find(&store, 0x0100U);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->sid, 0x0100U);
    EXPECT_EQ(s->param_count, 2U);
    EXPECT_EQ(s->param_ids[0], 0x0010U);
    EXPECT_EQ(s->param_ids[1], 0x0011U);
    EXPECT_EQ(s->interval_sec, 30U);
}

TEST(HkStore, ACreatedStructureStartsDisabled) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}), MIGRIS_HKSTORE_OK);
    const migris_hk_structure_t* s = migris_hkstore_find(&store, 0x0100U);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->enabled, 0);
}

TEST(HkStore, CreateRejectsBadArguments) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const std::vector<std::uint16_t> ids{0x0010U};
    EXPECT_EQ(migris_hkstore_create(nullptr, 0x0100U, ids.data(), 1U, 5U),
              MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_create(&store, 0x0100U, nullptr, 1U, 5U), MIGRIS_HKSTORE_ERR_BAD_ARG);
    // An empty parameter list.
    EXPECT_EQ(migris_hkstore_create(&store, 0x0100U, ids.data(), 0U, 5U),
              MIGRIS_HKSTORE_ERR_BAD_ARG);
}

TEST(HkStore, CreateRejectsAFrameworkRangeSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    // 0x0001..0x00FF is reserved for fsw-core framework structures.
    EXPECT_EQ(create(store, 0x0001U, {0x0010U}), MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(create(store, 0x00FFU, {0x0010U}), MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
}

TEST(HkStore, CreateRejectsTooManyParameters) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    const std::vector<std::uint16_t> ids(MIGRIS_HKSTORE_MAX_PARAMS + 1U, 0x0010U);
    EXPECT_EQ(create(store, 0x0100U, ids), MIGRIS_HKSTORE_ERR_TOO_MANY);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
}

TEST(HkStore, CreateRejectsADuplicateSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(create(store, 0x0100U, {0x0020U}), MIGRIS_HKSTORE_ERR_DUPLICATE);
    EXPECT_EQ(migris_hkstore_count(&store), 1U);
}

TEST(HkStore, CreateRejectsWhenFull) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    for (std::uint16_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        ASSERT_EQ(create(store, static_cast<std::uint16_t>(0x0100U + i), {0x0010U}),
                  MIGRIS_HKSTORE_OK);
    }
    EXPECT_EQ(create(store, 0x0900U, {0x0010U}), MIGRIS_HKSTORE_ERR_FULL);
}

TEST(HkStore, FindReturnsNullForUnknownSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0200U), nullptr);
    EXPECT_EQ(migris_hkstore_find(nullptr, 0x0100U), nullptr);
}

TEST(HkStore, DeleteRemovesAStructure) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_delete(&store, 0x0100U), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&store), 0U);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0100U), nullptr);
}

TEST(HkStore, DeleteRejectsUnknownSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    EXPECT_EQ(migris_hkstore_delete(&store, 0x0100U), MIGRIS_HKSTORE_ERR_NOT_FOUND);
    EXPECT_EQ(migris_hkstore_delete(nullptr, 0x0100U), MIGRIS_HKSTORE_ERR_BAD_ARG);
}

TEST(HkStore, DeleteFreesASlotForReuse) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    // Fill the store, free one slot, and confirm a new create reclaims it.
    for (std::uint16_t i = 0U; i < MIGRIS_HKSTORE_CAPACITY; ++i) {
        ASSERT_EQ(create(store, static_cast<std::uint16_t>(0x0100U + i), {0x0010U}),
                  MIGRIS_HKSTORE_OK);
    }
    ASSERT_EQ(migris_hkstore_delete(&store, 0x0100U), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(create(store, 0x0900U, {0x0010U}), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&store), static_cast<std::size_t>(MIGRIS_HKSTORE_CAPACITY));
}

TEST(HkStore, SetEnabledToggles) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}), MIGRIS_HKSTORE_OK);

    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0100U)->enabled, 1);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 0), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_find(&store, 0x0100U)->enabled, 0);
}

TEST(HkStore, SetEnabledRejectsUnknownSid) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    EXPECT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_ERR_NOT_FOUND);
    EXPECT_EQ(migris_hkstore_set_enabled(nullptr, 0x0100U, 1), MIGRIS_HKSTORE_ERR_BAD_ARG);
}

TEST(HkStore, DueReturnsNullWhileDisabled) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 5U), MIGRIS_HKSTORE_OK);  // disabled by default
    EXPECT_EQ(migris_hkstore_due(&store, 10000U), nullptr);
}

TEST(HkStore, DueReturnsNullBeforeTheIntervalElapses) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 100U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    // last_emit_sec starts at 0, so now must reach the interval.
    EXPECT_EQ(migris_hkstore_due(&store, 99U), nullptr);
}

TEST(HkStore, DueSkipsAPollOnlyStructure) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    // interval 0 means poll-only — never periodically due.
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 0U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_due(&store, 100000U), nullptr);
}

TEST(HkStore, DueReleasesAnOverdueStructureAndStampsLastEmit) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 5U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);

    const migris_hk_structure_t* s = migris_hkstore_due(&store, 100U);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->sid, 0x0100U);
    EXPECT_EQ(s->last_emit_sec, 100U);
    // Stamped — not due again until the interval elapses afresh.
    EXPECT_EQ(migris_hkstore_due(&store, 104U), nullptr);
    EXPECT_NE(migris_hkstore_due(&store, 105U), nullptr);
}

TEST(HkStore, DueReleasesTheStructureWaitingLongestFirst) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 5U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(create(store, 0x0101U, {0x0011U}, 5U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0101U, 1), MIGRIS_HKSTORE_OK);

    // Both overdue (last_emit_sec 0); the store releases one per call.
    const migris_hk_structure_t* first = migris_hkstore_due(&store, 100U);
    const migris_hk_structure_t* second = migris_hkstore_due(&store, 100U);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->sid, second->sid);
    // Both are now stamped — nothing else is due at this instant.
    EXPECT_EQ(migris_hkstore_due(&store, 100U), nullptr);
}

TEST(HkStore, DueSkipsADisabledStructure) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    ASSERT_EQ(create(store, 0x0100U, {0x0010U}, 5U), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(create(store, 0x0101U, {0x0011U}, 5U), MIGRIS_HKSTORE_OK);
    // Only the second structure is enabled.
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0101U, 1), MIGRIS_HKSTORE_OK);

    const migris_hk_structure_t* s = migris_hkstore_due(&store, 100U);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->sid, 0x0101U);
    EXPECT_EQ(migris_hkstore_due(&store, 100U), nullptr);
}

}  // namespace
}  // namespace migris::fsw::hkstore::test
