// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board operating-mode manager — flash-backed persistence (slice
// fsw-17). Pure (gtest-free) helpers keep each test body inside the
// clang-tidy cognitive-complexity budget; the tests exercise the
// one-byte codec, the round-trip across re-init, the unknown- and
// out-of-range-ID fallback contracts, the no-MODE_CHANGED-on-restore
// rule, and the generation counter's mutation-vs-restore contract.

#include "migris/fsw/event_sink.h"
#include "migris/fsw/mode/mode.h"
#include "migris/fsw/pus/pus5.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::mode::test {
namespace {

constexpr migris_mode_id_t m_boot = 0U;
constexpr migris_mode_id_t m_nominal = 1U;
constexpr migris_mode_id_t m_safe = 2U;

// BOOT -> {NOMINAL, SAFE}; NOMINAL -> {SAFE}; SAFE -> {NOMINAL}.
std::array<migris_mode_def_t, 3> standard_defs() {
    return {{
        {m_boot, (1U << m_nominal) | (1U << m_safe)},
        {m_nominal, (1U << m_safe)},
        {m_safe, (1U << m_nominal)},
    }};
}

struct CapturedEvent {
    std::uint16_t event_id = 0U;
};

struct SinkSpy {
    std::vector<CapturedEvent> events;
};

extern "C" {
int mode_persistence_spy_report(void* self,
                                std::uint32_t /*now_seconds*/,
                                migris_pus5_severity_t /*severity*/,
                                std::uint16_t event_id,
                                const std::uint8_t* /*aux*/,
                                std::size_t /*aux_len*/) {
    auto* spy = static_cast<SinkSpy*>(self);
    spy->events.push_back(CapturedEvent{event_id});
    return 0;
}
}

migris_event_sink_t spy_sink(SinkSpy& spy) {
    migris_event_sink_t sink;
    sink.report = mode_persistence_spy_report;
    sink.self = &spy;
    return sink;
}

int init_standard(migris_mode_manager_t& mgr,
                  const migris_event_sink_t* sink = nullptr,
                  migris_mode_id_t initial = m_boot) {
    const auto defs = standard_defs();
    return migris_mode_init(&mgr, defs.data(), defs.size(), initial, sink);
}

TEST(ModePersistence, SerializeProducesOneByteCurrentId) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr), MIGRIS_MODE_OK);
    ASSERT_EQ(migris_mode_request(&mgr, m_safe, 100U), MIGRIS_MODE_OK);

    std::array<std::uint8_t, 4U> buf{};
    const int n = migris_mode_serialize(&mgr, buf.data(), buf.size());
    ASSERT_EQ(n, 1);
    EXPECT_EQ(buf[0], m_safe);
}

TEST(ModePersistence, DeserializeRestoresCurrentMode) {
    SinkSpy spy{};
    const auto sink = spy_sink(spy);
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr, &sink), MIGRIS_MODE_OK);
    ASSERT_EQ(migris_mode_current(&mgr), m_boot);

    const std::array<std::uint8_t, 1U> image{m_safe};
    ASSERT_EQ(migris_mode_deserialize_current(&mgr, image.data(), image.size()), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_current(&mgr), m_safe);
}

TEST(ModePersistence, DeserializeDoesNotEmitModeChanged) {
    SinkSpy spy{};
    const auto sink = spy_sink(spy);
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr, &sink), MIGRIS_MODE_OK);

    const std::array<std::uint8_t, 1U> image{m_safe};
    ASSERT_EQ(migris_mode_deserialize_current(&mgr, image.data(), image.size()), MIGRIS_MODE_OK);
    // A boot restore is NOT a runtime transition — no event.
    EXPECT_EQ(spy.events.size(), 0U);
}

TEST(ModePersistence, DeserializeIgnoresUnknownIdAndKeepsCurrent) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr, nullptr, m_nominal), MIGRIS_MODE_OK);
    // ID 7 is in range (< 32) but is not a declared mode.
    const std::array<std::uint8_t, 1U> image{7U};
    EXPECT_EQ(migris_mode_deserialize_current(&mgr, image.data(), image.size()), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_current(&mgr), m_nominal);
}

TEST(ModePersistence, DeserializeIgnoresOutOfRangeIdAndKeepsCurrent) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr, nullptr, m_nominal), MIGRIS_MODE_OK);
    // ID >= MIGRIS_MODE_ID_MAX is rejected without an error.
    const std::array<std::uint8_t, 1U> image{
        static_cast<std::uint8_t>(MIGRIS_MODE_ID_MAX),
    };
    EXPECT_EQ(migris_mode_deserialize_current(&mgr, image.data(), image.size()), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_current(&mgr), m_nominal);
}

TEST(ModePersistence, DeserializeRejectsEmptyImage) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr), MIGRIS_MODE_OK);
    const std::uint8_t* dummy = nullptr;
    // pass a real pointer with zero len
    const std::array<std::uint8_t, 1U> dummy_buf{};
    dummy = dummy_buf.data();
    EXPECT_EQ(migris_mode_deserialize_current(&mgr, dummy, 0U), MIGRIS_MODE_ERR_TRUNCATED);
    EXPECT_EQ(migris_mode_current(&mgr), m_boot);
}

TEST(ModePersistence, GenerationStartsAtZeroAndBumpsOnSuccessfulRequest) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_generation(&mgr), 0U);

    ASSERT_EQ(migris_mode_request(&mgr, m_safe, 100U), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_generation(&mgr), 1U);

    // A forbidden transition does NOT bump.
    EXPECT_EQ(migris_mode_request(&mgr, m_boot, 110U), MIGRIS_MODE_ERR_FORBIDDEN);
    EXPECT_EQ(migris_mode_generation(&mgr), 1U);

    ASSERT_EQ(migris_mode_request(&mgr, m_nominal, 120U), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_generation(&mgr), 2U);
}

TEST(ModePersistence, DeserializeDoesNotBumpGeneration) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr), MIGRIS_MODE_OK);

    const std::array<std::uint8_t, 1U> image{m_safe};
    ASSERT_EQ(migris_mode_deserialize_current(&mgr, image.data(), image.size()), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_generation(&mgr), 0U);
}

TEST(ModePersistence, SerializeRejectsTooSmallBuffer) {
    migris_mode_manager_t mgr{};
    ASSERT_EQ(init_standard(mgr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_serialize(&mgr, nullptr, 0U), MIGRIS_MODE_ERR_BAD_ARG);
    std::array<std::uint8_t, 1U> buf{};
    EXPECT_EQ(migris_mode_serialize(&mgr, buf.data(), 0U), MIGRIS_MODE_ERR_BUF_TOO_SMALL);
}

TEST(ModePersistence, RejectsNullArgs) {
    migris_mode_manager_t mgr{};
    std::array<std::uint8_t, 4U> buf{};
    EXPECT_EQ(migris_mode_serialize(nullptr, buf.data(), buf.size()), MIGRIS_MODE_ERR_BAD_ARG);
    EXPECT_EQ(migris_mode_deserialize_current(nullptr, buf.data(), buf.size()),
              MIGRIS_MODE_ERR_BAD_ARG);
    EXPECT_EQ(migris_mode_deserialize_current(&mgr, nullptr, buf.size()), MIGRIS_MODE_ERR_BAD_ARG);
    EXPECT_EQ(migris_mode_generation(nullptr), 0U);
}

}  // namespace
}  // namespace migris::fsw::mode::test
