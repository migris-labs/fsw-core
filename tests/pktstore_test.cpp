// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board packet store — bounded, RAM-backed, circular TM buffer.
// Pure (gtest-free) helpers build fake packets and keep the test
// bodies inside the clang-tidy cognitive-complexity budget; the tests
// exercise store / delete / span, the circular overwrite-oldest
// policy, the enable gate, time-window retrieval, and the
// retrieval-freeze contract.

#include "migris/fsw/pktstore/pktstore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::pktstore::test {
namespace {

// A fake telemetry packet: `len` bytes all equal to `tag`, so a
// retrieved packet is identifiable by its first byte.
std::vector<std::uint8_t> fake_packet(std::uint8_t tag, std::size_t len) {
    std::vector<std::uint8_t> packet(len, tag);
    return packet;
}

// Store a fake packet tagged `tag` at `storage_time`; return the
// migris_pktstore_store result.
int put(migris_pktstore_t& store,
        std::uint8_t tag,
        std::uint32_t storage_time,
        std::size_t len = 8U) {
    const auto pkt = fake_packet(tag, len);
    return migris_pktstore_store(&store, pkt.data(), pkt.size(), storage_time);
}

// Drain an armed retrieval to completion, returning each delivered
// packet's first byte (its identifying tag) in delivery order. A
// gtest-free helper so the retrieval loop stays out of the test body's
// clang-tidy cognitive-complexity budget.
std::vector<std::uint8_t> drain_tags(migris_pktstore_t& store) {
    std::array<std::uint8_t, MIGRIS_PKTSTORE_PACKET_MAX> out{};
    std::size_t out_len = 0U;
    std::vector<std::uint8_t> tags;
    while (migris_pktstore_retrieve_next(&store, out.data(), out.size(), &out_len) == 1) {
        tags.push_back(out[0]);
    }
    return tags;
}

TEST(PktStore, InitIsEmptyAndEnabled) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    EXPECT_EQ(migris_pktstore_count(&store), 0U);
    EXPECT_EQ(migris_pktstore_is_enabled(&store), 1);  // enabled by default
    EXPECT_EQ(migris_pktstore_retrieval_active(&store), 0);
}

TEST(PktStore, SetEnabledToggles) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pktstore_set_enabled(&store, 0);
    EXPECT_EQ(migris_pktstore_is_enabled(&store), 0);
    migris_pktstore_set_enabled(&store, 1);
    EXPECT_EQ(migris_pktstore_is_enabled(&store), 1);
}

TEST(PktStore, StoreCapturesAPacket) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    EXPECT_EQ(put(store, 0xAAU, 100U), 1);
    EXPECT_EQ(migris_pktstore_count(&store), 1U);
}

TEST(PktStore, StoreRejectsBadArguments) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    const auto pkt = fake_packet(0x1U, 8U);
    EXPECT_EQ(migris_pktstore_store(nullptr, pkt.data(), pkt.size(), 0U),
              MIGRIS_PKTSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_pktstore_store(&store, nullptr, 8U, 0U), MIGRIS_PKTSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_pktstore_store(&store, pkt.data(), 0U, 0U), MIGRIS_PKTSTORE_ERR_BAD_ARG);
}

TEST(PktStore, StoreRejectsOversizedPacket) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    EXPECT_EQ(put(store, 0x1U, 0U, MIGRIS_PKTSTORE_PACKET_MAX + 1U),
              MIGRIS_PKTSTORE_ERR_PACKET_TOO_LARGE);
    EXPECT_EQ(migris_pktstore_count(&store), 0U);
}

TEST(PktStore, StoreIsANoOpWhileDisabled) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    migris_pktstore_set_enabled(&store, 0);
    EXPECT_EQ(put(store, 0x1U, 100U), 0);  // not stored — not an error
    EXPECT_EQ(migris_pktstore_count(&store), 0U);
}

TEST(PktStore, StoreOverwritesOldestWhenFull) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    // Store CAPACITY + 5 packets; storage time == tag, so time order
    // tracks insertion order.
    for (std::uint32_t i = 0U; i < MIGRIS_PKTSTORE_CAPACITY + 5U; ++i) {
        ASSERT_EQ(put(store, static_cast<std::uint8_t>(i), i), 1);
    }
    EXPECT_EQ(migris_pktstore_count(&store), static_cast<std::size_t>(MIGRIS_PKTSTORE_CAPACITY));

    std::uint32_t oldest = 0U;
    std::uint32_t newest = 0U;
    ASSERT_EQ(migris_pktstore_span(&store, &oldest, &newest), 1);
    EXPECT_EQ(oldest, 5U);  // the first 5 were overwritten
    EXPECT_EQ(newest, MIGRIS_PKTSTORE_CAPACITY + 4U);
}

TEST(PktStore, DeleteUpToRemovesTheLeadingRun) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    ASSERT_EQ(put(store, 0x1U, 10U), 1);
    ASSERT_EQ(put(store, 0x2U, 20U), 1);
    ASSERT_EQ(put(store, 0x3U, 30U), 1);

    EXPECT_EQ(migris_pktstore_delete_up_to(&store, 20U), 2);  // times 10 and 20
    EXPECT_EQ(migris_pktstore_count(&store), 1U);

    std::uint32_t oldest = 0U;
    std::uint32_t newest = 0U;
    ASSERT_EQ(migris_pktstore_span(&store, &oldest, &newest), 1);
    EXPECT_EQ(oldest, 30U);
}

TEST(PktStore, DeleteUpToRemovesNothingWhenNoneMatch) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    ASSERT_EQ(put(store, 0x1U, 100U), 1);
    EXPECT_EQ(migris_pktstore_delete_up_to(&store, 50U), 0);
    EXPECT_EQ(migris_pktstore_count(&store), 1U);
}

TEST(PktStore, SpanReturnsZeroWhenEmpty) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    std::uint32_t oldest = 0xDEADU;
    std::uint32_t newest = 0xBEEFU;
    EXPECT_EQ(migris_pktstore_span(&store, &oldest, &newest), 0);
}

TEST(PktStore, ArmRetrievalRejectsBadArguments) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    EXPECT_EQ(migris_pktstore_arm_retrieval(nullptr, 0U, 100U), MIGRIS_PKTSTORE_ERR_BAD_ARG);
    // from after to — an inverted window.
    EXPECT_EQ(migris_pktstore_arm_retrieval(&store, 200U, 100U), MIGRIS_PKTSTORE_ERR_BAD_ARG);
}

TEST(PktStore, RetrieveNextReturnsZeroWhenNoRetrievalArmed) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    ASSERT_EQ(put(store, 0x1U, 10U), 1);
    std::array<std::uint8_t, MIGRIS_PKTSTORE_PACKET_MAX> out{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_pktstore_retrieve_next(&store, out.data(), out.size(), &out_len), 0);
}

TEST(PktStore, RetrieveDeliversTheTimeWindowInOrder) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    put(store, 0x10U, 10U);
    put(store, 0x20U, 20U);
    put(store, 0x30U, 30U);
    put(store, 0x40U, 40U);
    ASSERT_EQ(migris_pktstore_count(&store), 4U);

    // Window [15, 35] selects the times-20 and times-30 packets.
    ASSERT_EQ(migris_pktstore_arm_retrieval(&store, 15U, 35U), MIGRIS_PKTSTORE_OK);
    EXPECT_EQ(migris_pktstore_retrieval_active(&store), 1);

    EXPECT_EQ(drain_tags(store), (std::vector<std::uint8_t>{0x20U, 0x30U}));
    EXPECT_EQ(migris_pktstore_retrieval_active(&store), 0);  // exhausted → cleared
    // The retrieval is non-destructive — every packet is still stored.
    EXPECT_EQ(migris_pktstore_count(&store), 4U);
}

TEST(PktStore, StoreAndDeleteAreFrozenDuringARetrieval) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    ASSERT_EQ(put(store, 0x1U, 10U), 1);
    ASSERT_EQ(migris_pktstore_arm_retrieval(&store, 0U, 1000U), MIGRIS_PKTSTORE_OK);

    // Storing is suspended (returns 0, not an error); delete and a
    // second arm are rejected outright.
    EXPECT_EQ(put(store, 0x2U, 20U), 0);
    EXPECT_EQ(migris_pktstore_count(&store), 1U);
    EXPECT_EQ(migris_pktstore_delete_up_to(&store, 100U), MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE);
    EXPECT_EQ(migris_pktstore_arm_retrieval(&store, 0U, 10U), MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE);
}

TEST(PktStore, RetrievedPacketBytesAreVerbatim) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    const auto original = fake_packet(0x7EU, 19U);
    ASSERT_EQ(migris_pktstore_store(&store, original.data(), original.size(), 50U), 1);

    ASSERT_EQ(migris_pktstore_arm_retrieval(&store, 0U, 100U), MIGRIS_PKTSTORE_OK);
    std::array<std::uint8_t, MIGRIS_PKTSTORE_PACKET_MAX> out{};
    std::size_t out_len = 0U;
    ASSERT_EQ(migris_pktstore_retrieve_next(&store, out.data(), out.size(), &out_len), 1);
    EXPECT_EQ(out_len, original.size());
    EXPECT_TRUE(std::equal(original.begin(), original.end(), out.begin()));
}

TEST(PktStore, RetrieveNextRejectsAnUndersizedBuffer) {
    migris_pktstore_t store{};
    migris_pktstore_init(&store);
    ASSERT_EQ(put(store, 0x1U, 10U, 20U), 1);
    ASSERT_EQ(migris_pktstore_arm_retrieval(&store, 0U, 100U), MIGRIS_PKTSTORE_OK);

    std::array<std::uint8_t, 10U> small{};
    std::size_t out_len = 0U;
    EXPECT_EQ(migris_pktstore_retrieve_next(&store, small.data(), small.size(), &out_len),
              MIGRIS_PKTSTORE_ERR_BUF_TOO_SMALL);
}

}  // namespace
}  // namespace migris::fsw::pktstore::test
