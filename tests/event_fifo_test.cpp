// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// Bounded event FIFO — drop-newest, single-context, non-atomic.
// Pure (gtest-free) helpers do the looping so the test bodies stay
// inside the clang-tidy cognitive-complexity budget; the tests
// exercise FIFO order, the capacity / drop-newest contract, the aux
// bound, and wraparound across the ring's modulo boundary.

#include "migris/fsw/fdir/event_fifo.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace migris::fsw::pus::test {
namespace {

// Pure: a record tagged by event_id so FIFO order is observable. aux is
// the first `aux_len` bytes of an ascending pattern, so the round-trip
// also proves the payload is copied by value (the producer's buffer
// does not outlive the push).
migris_fdir_event_t make_ev(std::uint16_t id, std::uint8_t aux_len = 0U) {
    migris_fdir_event_t ev{};
    ev.event_id = id;
    ev.severity = 1U;
    ev.aux_len = aux_len;
    for (std::uint8_t i = 0U; i < aux_len; ++i) {
        ev.aux[i] = static_cast<std::uint8_t>(0xA0U + i);
    }
    ev.t_seconds = 0x01020304U + id;
    return ev;
}

bool same(const migris_fdir_event_t& a, const migris_fdir_event_t& b) {
    if (a.event_id != b.event_id || a.severity != b.severity || a.aux_len != b.aux_len ||
        a.t_seconds != b.t_seconds) {
        return false;
    }
    for (std::uint8_t i = 0U; i < a.aux_len; ++i) {
        if (a.aux[i] != b.aux[i]) {
            return false;
        }
    }
    return true;
}

// Push ids [start, start+n). True iff every push enqueued.
bool push_seq(migris_event_fifo_t& fifo, std::uint16_t start, std::uint16_t n) {
    for (std::uint16_t i = 0U; i < n; ++i) {
        const auto ev = make_ev(static_cast<std::uint16_t>(start + i));
        if (migris_event_fifo_push(&fifo, &ev) != MIGRIS_EVENT_FIFO_OK) {
            return false;
        }
    }
    return true;
}

// Pop n records, requiring each event_id == start+i (strict FIFO).
bool pop_expect_seq(migris_event_fifo_t& fifo, std::uint16_t start, std::uint16_t n) {
    migris_fdir_event_t out{};
    for (std::uint16_t i = 0U; i < n; ++i) {
        if (migris_event_fifo_pop(&fifo, &out) != 1) {
            return false;
        }
        if (out.event_id != static_cast<std::uint16_t>(start + i)) {
            return false;
        }
    }
    return true;
}

// `cycles` matched push/pop pairs, each pop required to yield the next
// id in strict FIFO order — drives head and tail repeatedly past the
// ring's modulo boundary.
bool cycle_push_pop(migris_event_fifo_t& fifo,
                    std::uint16_t push_start,
                    std::uint16_t expect_start,
                    std::uint16_t cycles) {
    migris_fdir_event_t out{};
    for (std::uint16_t i = 0U; i < cycles; ++i) {
        const auto ev = make_ev(static_cast<std::uint16_t>(push_start + i));
        if (migris_event_fifo_push(&fifo, &ev) != MIGRIS_EVENT_FIFO_OK) {
            return false;
        }
        if (migris_event_fifo_pop(&fifo, &out) != 1) {
            return false;
        }
        if (out.event_id != static_cast<std::uint16_t>(expect_start + i)) {
            return false;
        }
    }
    return true;
}

TEST(EventFifo, PopOnEmptyReturnsZero) {
    migris_event_fifo_t fifo{};
    migris_event_fifo_init(&fifo);
    migris_fdir_event_t out{};
    EXPECT_EQ(migris_event_fifo_pop(&fifo, &out), 0);
    EXPECT_EQ(migris_event_fifo_count(&fifo), 0U);
}

TEST(EventFifo, PreservesFifoOrderAndCopiesByValue) {
    migris_event_fifo_t fifo{};
    const auto a = make_ev(0x0002U, 3U);
    const auto b = make_ev(0x0003U, 4U);

    EXPECT_EQ(migris_event_fifo_push(&fifo, &a), MIGRIS_EVENT_FIFO_OK);
    EXPECT_EQ(migris_event_fifo_push(&fifo, &b), MIGRIS_EVENT_FIFO_OK);
    EXPECT_EQ(migris_event_fifo_count(&fifo), 2U);

    migris_fdir_event_t out{};
    ASSERT_EQ(migris_event_fifo_pop(&fifo, &out), 1);
    EXPECT_TRUE(same(out, a));
    ASSERT_EQ(migris_event_fifo_pop(&fifo, &out), 1);
    EXPECT_TRUE(same(out, b));
    EXPECT_EQ(migris_event_fifo_pop(&fifo, &out), 0);
}

TEST(EventFifo, DropNewestWhenFullPreservesCausalHead) {
    migris_event_fifo_t fifo{};
    ASSERT_TRUE(push_seq(fifo, 0U, MIGRIS_FDIR_EVENT_FIFO_CAPACITY));
    EXPECT_EQ(migris_event_fifo_count(&fifo),
              static_cast<std::size_t>(MIGRIS_FDIR_EVENT_FIFO_CAPACITY));

    // Two further pushes are rejected; dropped advances; head intact.
    const auto over1 = make_ev(0xFFF0U);
    const auto over2 = make_ev(0xFFF1U);
    EXPECT_EQ(migris_event_fifo_push(&fifo, &over1), MIGRIS_EVENT_FIFO_ERR_FULL);
    EXPECT_EQ(migris_event_fifo_push(&fifo, &over2), MIGRIS_EVENT_FIFO_ERR_FULL);
    EXPECT_EQ(migris_event_fifo_dropped(&fifo), 2U);

    // The causal head is intact: the first CAPACITY ids drain in order.
    EXPECT_TRUE(pop_expect_seq(fifo, 0U, MIGRIS_FDIR_EVENT_FIFO_CAPACITY));
}

TEST(EventFifo, AuxOverMaxIsBadArgAndDoesNotCountAsDrop) {
    migris_event_fifo_t fifo{};
    migris_fdir_event_t ev{};
    ev.event_id = 0x0009U;
    ev.aux_len = MIGRIS_FDIR_EVENT_AUX_MAX + 1U;
    EXPECT_EQ(migris_event_fifo_push(&fifo, &ev), MIGRIS_EVENT_FIFO_ERR_BAD_ARG);
    EXPECT_EQ(migris_event_fifo_count(&fifo), 0U);
    EXPECT_EQ(migris_event_fifo_dropped(&fifo), 0U);  // a producer bug, not a capacity loss
}

TEST(EventFifo, NullArgumentsAreRejectedNotCrash) {
    migris_event_fifo_t fifo{};
    const auto ev = make_ev(0x0001U);
    migris_fdir_event_t out{};
    EXPECT_EQ(migris_event_fifo_push(nullptr, &ev), MIGRIS_EVENT_FIFO_ERR_BAD_ARG);
    EXPECT_EQ(migris_event_fifo_push(&fifo, nullptr), MIGRIS_EVENT_FIFO_ERR_BAD_ARG);
    EXPECT_EQ(migris_event_fifo_pop(nullptr, &out), 0);
    EXPECT_EQ(migris_event_fifo_pop(&fifo, nullptr), 0);
    EXPECT_EQ(migris_event_fifo_count(nullptr), 0U);
    EXPECT_EQ(migris_event_fifo_dropped(nullptr), 0U);
}

TEST(EventFifo, WrapsAroundTheModuloBoundary) {
    migris_event_fifo_t fifo{};

    // Prime with 6 in / 4 out so head sits mid-ring with 2 queued
    // (ids 4 and 5 remain).
    ASSERT_TRUE(push_seq(fifo, 0U, 6U));
    ASSERT_TRUE(pop_expect_seq(fifo, 0U, 4U));

    const auto cycles = static_cast<std::uint16_t>(3U * MIGRIS_FDIR_EVENT_FIFO_CAPACITY + 5U);
    ASSERT_TRUE(cycle_push_pop(fifo, /*push_start=*/6U, /*expect_start=*/4U, cycles));

    // The two still queued from the priming burst drain last, in order.
    ASSERT_TRUE(pop_expect_seq(fifo, static_cast<std::uint16_t>(4U + cycles), 2U));
    EXPECT_EQ(migris_event_fifo_count(&fifo), 0U);
    EXPECT_EQ(migris_event_fifo_dropped(&fifo), 0U);
}

}  // namespace
}  // namespace migris::fsw::pus::test
