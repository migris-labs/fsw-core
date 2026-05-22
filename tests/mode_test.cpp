// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// On-board operating-mode manager — the generic mode state machine.
// A pure (gtest-free) mock event sink and a standard three-mode set
// keep the test bodies inside the clang-tidy cognitive-complexity
// budget; the tests exercise init validation, allowed and forbidden
// transitions, the self-transition rule, the PUS-5 MODE_CHANGED event
// emitted on a successful change, and the NULL / bad-argument paths.

#include "migris/fsw/mode/mode.h"

#include "migris/fsw/event_sink.h"
#include "migris/fsw/pus/pus5.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::mode::test {
namespace {

// A standard sample-like mode set used across the tests.
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

// One report() call recorded by the mock sink.
struct CapturedEvent {
    std::uint32_t now = 0U;
    int severity = -1;
    std::uint16_t event_id = 0U;
    std::vector<std::uint8_t> aux;
};

struct SinkSpy {
    std::vector<CapturedEvent> events;
};

// The thunk is `extern "C"` so it is assignment-compatible with the
// C-linkage function pointer in migris_event_sink_t.
extern "C" {
int mode_test_spy_report(void* self,
                         std::uint32_t now_seconds,
                         migris_pus5_severity_t severity,
                         std::uint16_t event_id,
                         const std::uint8_t* aux,
                         std::size_t aux_len) {
    auto* spy = static_cast<SinkSpy*>(self);
    CapturedEvent event;
    event.now = now_seconds;
    event.severity = static_cast<int>(severity);
    event.event_id = event_id;
    event.aux.assign(aux, aux + aux_len);
    spy->events.push_back(event);
    return 0;
}
}

migris_event_sink_t spy_sink(SinkSpy& spy) {
    migris_event_sink_t sink;
    sink.report = mode_test_spy_report;
    sink.self = &spy;
    return sink;
}

TEST(Mode, InitSucceedsAndSetsTheInitialMode) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_boot, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_current(&mgr), m_boot);
}

TEST(Mode, InitRejectsMoreModesThanCapacity) {
    migris_mode_manager_t mgr{};
    std::vector<migris_mode_def_t> too_many;
    too_many.reserve(MIGRIS_MODE_CAPACITY + 1U);
    for (std::uint32_t i = 0U; i < MIGRIS_MODE_CAPACITY + 1U; ++i) {
        too_many.push_back({static_cast<migris_mode_id_t>(i), 0U});
    }
    EXPECT_EQ(migris_mode_init(&mgr, too_many.data(), too_many.size(), 0U, nullptr),
              MIGRIS_MODE_ERR_CAPACITY);
    EXPECT_EQ(mgr.count, 0U);  // stateless failure
}

TEST(Mode, InitRejectsADuplicateModeId) {
    migris_mode_manager_t mgr{};
    const std::array<migris_mode_def_t, 2> dup{{{m_boot, 0U}, {m_boot, 0U}}};
    EXPECT_EQ(migris_mode_init(&mgr, dup.data(), dup.size(), m_boot, nullptr),
              MIGRIS_MODE_ERR_DUPLICATE);
}

TEST(Mode, InitRejectsAnOutOfRangeModeId) {
    migris_mode_manager_t mgr{};
    const std::array<migris_mode_def_t, 1> bad{
        {{static_cast<migris_mode_id_t>(MIGRIS_MODE_ID_MAX), 0U}}};
    EXPECT_EQ(migris_mode_init(&mgr, bad.data(), bad.size(), 0U, nullptr), MIGRIS_MODE_ERR_RANGE);
}

TEST(Mode, InitRejectsATargetBitNamingAnUndeclaredMode) {
    migris_mode_manager_t mgr{};
    // Mode 0 declares a transition to mode 5, which is not in the set.
    const std::array<migris_mode_def_t, 1> bad{{{m_boot, 1U << 5U}}};
    EXPECT_EQ(migris_mode_init(&mgr, bad.data(), bad.size(), m_boot, nullptr),
              MIGRIS_MODE_ERR_RANGE);
}

TEST(Mode, InitRejectsAnUndeclaredInitialMode) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    EXPECT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), 9U, nullptr),
              MIGRIS_MODE_ERR_NOT_FOUND);
}

TEST(Mode, AnAllowedTransitionSucceeds) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_boot, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_is_allowed(&mgr, m_nominal), 1);
    EXPECT_EQ(migris_mode_request(&mgr, m_nominal, 100U), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_current(&mgr), m_nominal);
}

TEST(Mode, AForbiddenTransitionIsRejectedAndTheModeUnchanged) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_nominal, nullptr), MIGRIS_MODE_OK);
    // NOMINAL may go only to SAFE — not back to BOOT.
    EXPECT_EQ(migris_mode_is_allowed(&mgr, m_boot), 0);
    EXPECT_EQ(migris_mode_request(&mgr, m_boot, 0U), MIGRIS_MODE_ERR_FORBIDDEN);
    EXPECT_EQ(migris_mode_current(&mgr), m_nominal);
}

TEST(Mode, ARequestToAnUndeclaredModeIsNotFound) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_boot, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr, 9U, 0U), MIGRIS_MODE_ERR_NOT_FOUND);
    EXPECT_EQ(migris_mode_current(&mgr), m_boot);
}

TEST(Mode, ASelfTransitionFollowsTheRules) {
    migris_mode_manager_t mgr{};
    // Mode 0 has its own bit set (self-transition allowed); mode 1 does not.
    const std::array<migris_mode_def_t, 2> defs{{{0U, 1U << 0U}, {1U, 0U}}};
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), 0U, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr, 0U, 0U), MIGRIS_MODE_OK);
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), 1U, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr, 1U, 0U), MIGRIS_MODE_ERR_FORBIDDEN);
}

TEST(Mode, ASuccessfulTransitionEmitsAModeChangedEvent) {
    migris_mode_manager_t mgr{};
    SinkSpy spy;
    const migris_event_sink_t sink = spy_sink(spy);
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_boot, &sink), MIGRIS_MODE_OK);

    ASSERT_EQ(migris_mode_request(&mgr, m_nominal, 0x12345678U), MIGRIS_MODE_OK);
    ASSERT_EQ(spy.events.size(), 1U);
    EXPECT_EQ(spy.events[0].event_id, MIGRIS_PUS5_EVT_MODE_CHANGED);
    EXPECT_EQ(spy.events[0].severity, static_cast<int>(MIGRIS_PUS5_SEV_INFO));
    EXPECT_EQ(spy.events[0].now, 0x12345678U);
    EXPECT_EQ(spy.events[0].aux, (std::vector<std::uint8_t>{m_boot, m_nominal}));
}

TEST(Mode, ARejectedTransitionEmitsNoEvent) {
    migris_mode_manager_t mgr{};
    SinkSpy spy;
    const migris_event_sink_t sink = spy_sink(spy);
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_nominal, &sink), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr, m_boot, 0U), MIGRIS_MODE_ERR_FORBIDDEN);
    EXPECT_TRUE(spy.events.empty());
}

TEST(Mode, ANullOrPartialSinkIsToleratedOnTransition) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    ASSERT_EQ(migris_mode_init(&mgr, defs.data(), defs.size(), m_boot, nullptr), MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr, m_nominal, 0U), MIGRIS_MODE_OK);

    // A sink whose report pointer is NULL must not be called.
    migris_mode_manager_t mgr2{};
    const migris_event_sink_t empty_sink{};
    ASSERT_EQ(migris_mode_init(&mgr2, defs.data(), defs.size(), m_boot, &empty_sink),
              MIGRIS_MODE_OK);
    EXPECT_EQ(migris_mode_request(&mgr2, m_nominal, 0U), MIGRIS_MODE_OK);
}

TEST(Mode, RejectsNullArguments) {
    migris_mode_manager_t mgr{};
    const auto defs = standard_defs();
    EXPECT_EQ(migris_mode_init(nullptr, defs.data(), defs.size(), m_boot, nullptr),
              MIGRIS_MODE_ERR_BAD_ARG);
    EXPECT_EQ(migris_mode_init(&mgr, nullptr, 1U, m_boot, nullptr), MIGRIS_MODE_ERR_BAD_ARG);
    EXPECT_EQ(migris_mode_current(nullptr), 0U);
    EXPECT_EQ(migris_mode_is_allowed(nullptr, m_boot), 0);
    EXPECT_EQ(migris_mode_request(nullptr, m_boot, 0U), MIGRIS_MODE_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::mode::test
