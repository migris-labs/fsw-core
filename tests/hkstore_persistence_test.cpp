// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board housekeeping-structure store — flash-backed persistence
// (slice fsw-17). Pure (gtest-free) builders keep each test body
// inside the clang-tidy cognitive-complexity budget; the tests
// exercise the per-structure codec, the round-trip across re-init,
// the truncation / over-capacity / over-max-params / SID-floor /
// duplicate-SID guards, the last_emit_sec re-arm contract, and the
// generation counter's mutation-vs-restore contract.

#include "migris/fsw/hkstore/hkstore.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::hkstore::test {
namespace {

int create_struct(migris_hkstore_t& store,
                  std::uint16_t sid,
                  const std::vector<std::uint16_t>& ids,
                  std::uint32_t interval = 5U) {
    return migris_hkstore_create(&store, sid, ids.data(), ids.size(), interval);
}

// Seed `n` structures with SIDs 0x0100, 0x0101, ..., each sampling
// (i+1) parameter IDs, periodicities `(i+1)*10` seconds. The first
// structure starts ENABLED (so the round-trip exercises the on-wire
// enabled bit in both states).
void seed(migris_hkstore_t& store, std::size_t n) {
    migris_hkstore_init(&store);
    for (std::size_t i = 0U; i < n; ++i) {
        std::vector<std::uint16_t> ids;
        for (std::size_t j = 0U; j <= i; ++j) {
            ids.push_back(static_cast<std::uint16_t>(0x0010U + (i * 8U) + j));
        }
        const auto sid = static_cast<std::uint16_t>(MIGRIS_HKSTORE_SID_MIN + i);
        const auto interval = static_cast<std::uint32_t>((i + 1U) * 10U);
        ASSERT_EQ(create_struct(store, sid, ids, interval), MIGRIS_HKSTORE_OK);
        if (i == 0U) {
            ASSERT_EQ(migris_hkstore_set_enabled(&store, sid, 1), MIGRIS_HKSTORE_OK);
        }
    }
}

// Walk `store.structures[0..count]` looking for `sid`. The serializer
// packs in-use slots into the low indices, so this is a direct
// post-deserialize check (find() does the same scan with an in_use
// gate, but we want to assert the packing too).
const migris_hk_structure_t* find_low(const migris_hkstore_t& store, std::uint16_t sid) {
    for (std::size_t i = 0U; i < store.count; ++i) {
        if (store.structures[i].in_use != 0 && store.structures[i].sid == sid) {
            return &store.structures[i];
        }
    }
    return nullptr;
}

// True iff the structure restored at the seed's i-th position matches
// what `seed()` would have written. Inverse of `seed()`'s per-element
// invariants. Wrapped in one bool so the round-trip test body sees one
// EXPECT_TRUE rather than a per-field loop — gtest macro expansion at
// the call site dominates cognitive complexity in clang-tidy.
bool structure_matches_seed(const migris_hk_structure_t& s, std::size_t i) {
    if (s.interval_sec != (i + 1U) * 10U) {
        return false;
    }
    if (s.enabled != ((i == 0U) ? 1 : 0)) {
        return false;
    }
    if (s.param_count != i + 1U) {
        return false;
    }
    for (std::size_t j = 0U; j <= i; ++j) {
        if (s.param_ids[j] != 0x0010U + (i * 8U) + j) {
            return false;
        }
    }
    return true;
}

// Apply structure_matches_seed across the first `n` SIDs seeded.
bool first_n_structures_match_seed(const migris_hkstore_t& store, std::size_t n) {
    for (std::size_t i = 0U; i < n; ++i) {
        const auto sid = static_cast<std::uint16_t>(MIGRIS_HKSTORE_SID_MIN + i);
        const migris_hk_structure_t* s = find_low(store, sid);
        if (s == nullptr || !structure_matches_seed(*s, i)) {
            return false;
        }
    }
    return true;
}

TEST(HkStorePersistence, EmptyRoundTrip) {
    migris_hkstore_t src{};
    migris_hkstore_init(&src);
    std::array<std::uint8_t, 16U> buf{};
    const int n = migris_hkstore_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);
    EXPECT_EQ(n, 2);  // header is just count(2 BE)

    migris_hkstore_t dst{};
    ASSERT_EQ(migris_hkstore_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&dst), 0U);
}

TEST(HkStorePersistence, RoundTripPreservesStructuresAndEnabledFlags) {
    migris_hkstore_t src{};
    seed(src, 3U);

    std::array<std::uint8_t, 256U> buf{};
    const int n = migris_hkstore_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_hkstore_t dst{};
    ASSERT_EQ(migris_hkstore_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&dst), 3U);
    EXPECT_TRUE(first_n_structures_match_seed(dst, 3U));
}

TEST(HkStorePersistence, RestoredStructuresReArmFromZero) {
    // A structure persisted with last_emit_sec = 100 must come back
    // with last_emit_sec = 0 — otherwise (now=10 - last=100) underflows
    // as uint32_t and the structure fires on every tick at boot.
    migris_hkstore_t src{};
    seed(src, 1U);
    src.structures[0].last_emit_sec = 100U;

    std::array<std::uint8_t, 64U> buf{};
    const int n = migris_hkstore_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_hkstore_t dst{};
    ASSERT_EQ(migris_hkstore_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_HKSTORE_OK);
    const auto sid = static_cast<std::uint16_t>(MIGRIS_HKSTORE_SID_MIN);
    const migris_hk_structure_t* s = find_low(dst, sid);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->last_emit_sec, 0U);
}

TEST(HkStorePersistence, DeserializeCompactsIntoLowSlots) {
    // Seed two structures, delete the first to leave a sparse layout,
    // then round-trip — the restored store must pack in_use slots into
    // the lowest indices (compaction-on-restore is part of the contract).
    migris_hkstore_t src{};
    migris_hkstore_init(&src);
    ASSERT_EQ(create_struct(src, 0x0100U, {0x0001U}), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(create_struct(src, 0x0101U, {0x0002U}), MIGRIS_HKSTORE_OK);
    ASSERT_EQ(migris_hkstore_delete(&src, 0x0100U), MIGRIS_HKSTORE_OK);

    // After delete, slot 0 is free and slot 1 holds SID 0x0101.
    EXPECT_EQ(src.structures[0].in_use, 0);
    EXPECT_EQ(src.structures[1].in_use, 1);

    std::array<std::uint8_t, 64U> buf{};
    const int n = migris_hkstore_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_hkstore_t dst{};
    ASSERT_EQ(migris_hkstore_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_count(&dst), 1U);
    // The surviving structure now lives at slot 0.
    ASSERT_EQ(dst.structures[0].in_use, 1);
    EXPECT_EQ(dst.structures[0].sid, 0x0101U);
}

TEST(HkStorePersistence, SerializeRejectsTooSmallBuffer) {
    migris_hkstore_t src{};
    seed(src, 1U);
    std::array<std::uint8_t, 4U> tiny{};
    EXPECT_EQ(migris_hkstore_serialize(&src, tiny.data(), tiny.size()),
              MIGRIS_HKSTORE_ERR_BUF_TOO_SMALL);
}

TEST(HkStorePersistence, DeserializeRejectsTruncatedHeader) {
    migris_hkstore_t dst{};
    const std::array<std::uint8_t, 1U> too_short{0x00U};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, too_short.data(), too_short.size()),
              MIGRIS_HKSTORE_ERR_TRUNCATED);
}

TEST(HkStorePersistence, DeserializeRejectsTruncatedStructure) {
    // count=1 but only 4 of the 8 per-structure header bytes follow.
    const std::array<std::uint8_t, 6U> image{
        0x00U,
        0x01U,  // count = 1
        0x01U,
        0x00U,
        0x00U,
        0x00U,  // 4 of 8 header bytes
    };
    migris_hkstore_t dst{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, image.data(), image.size()),
              MIGRIS_HKSTORE_ERR_TRUNCATED);
}

TEST(HkStorePersistence, DeserializeRejectsParamCountOverMax) {
    // sid = 0x0100, interval = 0, enabled = 0, param_count = MAX + 1.
    std::array<std::uint8_t, 10U> image{
        0x00U,
        0x01U,  // count = 1
        0x01U,
        0x00U,  // sid = 0x0100
        0x00U,
        0x00U,
        0x00U,
        0x00U,  // interval_sec = 0
        0x00U,  // enabled = 0
        static_cast<std::uint8_t>(MIGRIS_HKSTORE_MAX_PARAMS + 1U),
    };
    migris_hkstore_t dst{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, image.data(), image.size()),
              MIGRIS_HKSTORE_ERR_TOO_MANY);
}

TEST(HkStorePersistence, DeserializeRejectsCountOverCapacity) {
    const auto overcount = static_cast<std::uint16_t>(MIGRIS_HKSTORE_CAPACITY + 1U);
    const std::array<std::uint8_t, 2U> image{
        static_cast<std::uint8_t>(overcount >> 8),
        static_cast<std::uint8_t>(overcount & 0xFFU),
    };
    migris_hkstore_t dst{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, image.data(), image.size()),
              MIGRIS_HKSTORE_ERR_FULL);
}

TEST(HkStorePersistence, DeserializeRejectsFrameworkRangeSid) {
    // sid = 0x0001 (in the reserved framework block).
    const std::array<std::uint8_t, 10U> image{
        0x00U,
        0x01U,
        0x00U,
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };
    migris_hkstore_t dst{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, image.data(), image.size()),
              MIGRIS_HKSTORE_ERR_BAD_ARG);
}

TEST(HkStorePersistence, DeserializeRejectsDuplicateSid) {
    // Two structures with the same SID — the on-flash image must be
    // rejected, mirroring migris_hkstore_create's duplicate guard.
    const std::array<std::uint8_t, 18U> image{
        0x00U,
        0x02U,  // count = 2
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,  // sid 0x0100
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,  // sid 0x0100 again
    };
    migris_hkstore_t dst{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, image.data(), image.size()),
              MIGRIS_HKSTORE_ERR_DUPLICATE);
}

TEST(HkStorePersistence, GenerationStartsAtZeroAndBumpsOnMutations) {
    migris_hkstore_t store{};
    migris_hkstore_init(&store);
    EXPECT_EQ(migris_hkstore_generation(&store), 0U);

    const std::vector<std::uint16_t> ids{0x0010U};
    ASSERT_EQ(create_struct(store, 0x0100U, ids), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_generation(&store), 1U);

    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_generation(&store), 2U);
    // No-op set_enabled must NOT bump.
    ASSERT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_generation(&store), 2U);

    // due() emits but doesn't bump — last_emit_sec is not persisted.
    (void)migris_hkstore_due(&store, 100U);
    EXPECT_EQ(migris_hkstore_generation(&store), 2U);

    ASSERT_EQ(migris_hkstore_delete(&store, 0x0100U), MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_generation(&store), 3U);

    // A rejected mutation must NOT bump.
    EXPECT_EQ(migris_hkstore_set_enabled(&store, 0x0100U, 1), MIGRIS_HKSTORE_ERR_NOT_FOUND);
    EXPECT_EQ(migris_hkstore_generation(&store), 3U);
}

TEST(HkStorePersistence, DeserializeDoesNotBumpGeneration) {
    migris_hkstore_t src{};
    seed(src, 2U);
    std::array<std::uint8_t, 128U> buf{};
    const int n = migris_hkstore_serialize(&src, buf.data(), buf.size());
    ASSERT_GT(n, 0);

    migris_hkstore_t dst{};
    ASSERT_EQ(migris_hkstore_deserialize(&dst, buf.data(), static_cast<std::size_t>(n)),
              MIGRIS_HKSTORE_OK);
    EXPECT_EQ(migris_hkstore_generation(&dst), 0U);
}

TEST(HkStorePersistence, DeserializeFailureLeavesStoreEmpty) {
    migris_hkstore_t dst{};
    seed(dst, 2U);  // pre-populated to verify the stateless-failure contract
    const std::array<std::uint8_t, 1U> bad{};
    EXPECT_EQ(migris_hkstore_deserialize(&dst, bad.data(), bad.size()),
              MIGRIS_HKSTORE_ERR_TRUNCATED);
    EXPECT_EQ(migris_hkstore_count(&dst), 0U);
}

TEST(HkStorePersistence, RejectsNullArgs) {
    migris_hkstore_t store{};
    std::array<std::uint8_t, 4U> buf{};
    EXPECT_EQ(migris_hkstore_serialize(nullptr, buf.data(), buf.size()),
              MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_serialize(&store, nullptr, buf.size()), MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_deserialize(nullptr, buf.data(), buf.size()),
              MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_deserialize(&store, nullptr, buf.size()), MIGRIS_HKSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_hkstore_generation(nullptr), 0U);
}

}  // namespace
}  // namespace migris::fsw::hkstore::test
