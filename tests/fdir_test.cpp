// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// FDIR primitives — anomaly registry, the generic event sink, and the
// PUS-5 drain. A pure (gtest-free) PUS-5 decoder (independent of the C
// encoder, meeting it only on the wire) checks that a reported anomaly
// drains into a byte-exact, CRC-valid spontaneous PUS-5 report with the
// registry's severity / event ID / aux.

#include "migris/fsw/fdir/fdir.h"

#include "migris/fsw/event_sink.h"
#include "migris/fsw/fdir/event_fifo.h"
#include "migris/fsw/mode/mode.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus5.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;
constexpr std::size_t udf = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;

struct Pus5 {
    migris_ccsds_primary_header_t primary{};
    migris_pus_tm_secondary_header_t secondary{};
    std::uint16_t event_id = 0U;
    std::vector<std::uint8_t> aux;
    bool crc_ok = false;
    std::size_t size = 0U;
};

// Pure: decode one PUS-5 packet at the buffer head.
Pus5 decode(const std::uint8_t* pkt) {
    Pus5 d;
    migris_ccsds_primary_unpack(&d.primary, pkt, MIGRIS_CCSDS_PRIMARY_HEADER_SIZE);
    d.size = migris_ccsds_packet_total_size(d.primary.data_length);
    migris_pus_tm_secondary_unpack(
        &d.secondary, &pkt[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE], MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE);
    d.event_id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[udf]) << 8) |
                                            static_cast<std::uint16_t>(pkt[udf + 1U]));
    for (std::size_t i = udf + MIGRIS_PUS5_EVENT_ID_SIZE; i < d.size - 2U; ++i) {
        d.aux.push_back(pkt[i]);
    }
    const std::uint16_t computed = migris_crc16_ccitt_false(pkt, d.size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(pkt[d.size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(pkt[d.size - 1U]));
    d.crc_ok = (computed == on_wire);
    return d;
}

migris_fdir_ctx_t make_fdir() {
    migris_fdir_ctx_t f{};
    migris_fdir_init(&f);
    return f;
}

// --- Isolation/recovery helpers (slice fsw-14) ----------------------

// Standard sample-like mode set: BOOT -> {NOMINAL, SAFE};
// NOMINAL -> {SAFE}; SAFE -> {NOMINAL}.
constexpr migris_mode_id_t m_boot = 0U;
constexpr migris_mode_id_t m_nominal = 1U;
constexpr migris_mode_id_t m_safe = 2U;

std::array<migris_mode_def_t, 3> mode_defs() {
    return {{
        {m_boot, (1U << m_nominal) | (1U << m_safe)},
        {m_nominal, (1U << m_safe)},
        {m_safe, (1U << m_nominal)},
    }};
}

// Wire `mode` (starting in NOMINAL) and arm `fdir` to recover to SAFE
// after `threshold` TC rejections. `fdir_sink` is caller-owned and
// must outlive `mode` — pass the object the test body holds.
void arm_tc_rejected(migris_fdir_ctx_t& fdir,
                     migris_mode_manager_t& mode,
                     const migris_event_sink_t& fdir_sink,
                     std::uint16_t threshold) {
    const auto defs = mode_defs();
    migris_mode_init(&mode, defs.data(), defs.size(), m_nominal, &fdir_sink);
    const std::array<migris_fdir_confirm_def_t, 1> confirms = {
        {{MIGRIS_FDIR_ANOM_TC_REJECTED, threshold}}};
    (void)migris_fdir_arm_recovery(&fdir, &mode, m_safe, confirms.data(), confirms.size());
}

// Report `n` TC-rejected anomalies through the typed producer API.
void reject_n(migris_fdir_ctx_t& fdir, int n) {
    for (int k = 0; k < n; ++k) {
        (void)migris_fdir_report_anomaly(&fdir, MIGRIS_FDIR_ANOM_TC_REJECTED, 1000U, 0U);
    }
}

// Report `n` TC rejections through the generic event sink — the path
// the TC router takes (an already-classified event, not the typed API).
void reject_n_via_sink(migris_fdir_ctx_t& fdir, int n) {
    const migris_event_sink_t sink = migris_fdir_event_sink(&fdir);
    const std::array<std::uint8_t, 3> aux = {0U, 0U, 0U};
    for (int k = 0; k < n; ++k) {
        (void)sink.report(
            sink.self, 1000U, MIGRIS_PUS5_SEV_LOW, MIGRIS_PUS5_EVT_TC_REJECTED, aux.data(), 3U);
    }
}

// Drain every queued event into a decoded list — gtest-free so the
// drain loop stays out of the test bodies' complexity budget.
std::vector<Pus5> drain_all(migris_fdir_ctx_t& fdir) {
    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};
    std::vector<Pus5> events;
    while (migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()) > 0) {
        events.push_back(decode(out.data()));
    }
    return events;
}

// Count decoded events carrying `event_id`.
int count_event(const std::vector<Pus5>& events, std::uint16_t event_id) {
    int n = 0;
    for (const Pus5& e : events) {
        if (e.event_id == event_id) {
            ++n;
        }
    }
    return n;
}

TEST(Fdir, TcRejectedAnomalyDrainsToLowSeverityPus5) {
    auto fdir = make_fdir();
    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};

    // detail = fc<<16 | service_type<<8 | service_subtype.
    const std::uint32_t detail =
        (static_cast<std::uint32_t>(MIGRIS_PUS1_FC_CRC_FAILURE) << 16) | (17U << 8) | 1U;
    ASSERT_EQ(migris_fdir_report_anomaly(&fdir, MIGRIS_FDIR_ANOM_TC_REJECTED, 0x11223344U, detail),
              MIGRIS_EVENT_FIFO_OK);

    const int n = migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size());
    ASSERT_GT(n, 0);
    const Pus5 d = decode(out.data());

    EXPECT_EQ(d.primary.type, MIGRIS_CCSDS_PACKET_TYPE_TM);
    EXPECT_EQ(d.primary.apid, test_apid);
    EXPECT_EQ(d.primary.seq_count, 0U);
    EXPECT_EQ(d.secondary.service_type, MIGRIS_PUS_SERVICE_EVENT_REPORTING);
    EXPECT_EQ(d.secondary.service_subtype, MIGRIS_PUS5_SUBTYPE_LOW);  // severity LOW
    EXPECT_EQ(d.secondary.destination_id, 0U);                        // spontaneous
    EXPECT_EQ(d.secondary.time_seconds, 0x11223344U);                 // detection time
    EXPECT_EQ(d.event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(d.aux,
              (std::vector<std::uint8_t>{
                  static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE), 17U, 1U}));
    EXPECT_TRUE(d.crc_ok);
    EXPECT_EQ(seq, 1U);                  // shared sequence advanced once
    EXPECT_EQ(pus5.msg_counter[1], 1U);  // low-severity counter advanced once
}

TEST(Fdir, RxOverflowAnomalyDrainsToMediumSeverityPus5) {
    auto fdir = make_fdir();
    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 7U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};

    ASSERT_EQ(
        migris_fdir_report_anomaly(&fdir, MIGRIS_FDIR_ANOM_RX_OVERFLOW, 0x00C0FFEEU, 0xDEADBEEFU),
        MIGRIS_EVENT_FIFO_OK);
    const int n = migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size());
    ASSERT_GT(n, 0);
    const Pus5 d = decode(out.data());

    EXPECT_EQ(d.secondary.service_subtype, MIGRIS_PUS5_SUBTYPE_MEDIUM);  // severity MEDIUM
    EXPECT_EQ(d.event_id, MIGRIS_PUS5_EVT_RX_OVERFLOW);
    EXPECT_EQ(d.aux, (std::vector<std::uint8_t>{0xDEU, 0xADU, 0xBEU, 0xEFU}));  // u32 BE
    EXPECT_EQ(d.secondary.time_seconds, 0x00C0FFEEU);
    EXPECT_TRUE(d.crc_ok);
    EXPECT_EQ(seq, 8U);
    EXPECT_EQ(pus5.msg_counter[2], 1U);  // medium-severity counter
}

TEST(Fdir, GenericSinkRecordsAlreadyClassifiedEvent) {
    auto fdir = make_fdir();
    const migris_event_sink_t sink = migris_fdir_event_sink(&fdir);
    ASSERT_NE(sink.report, nullptr);
    ASSERT_EQ(sink.self, &fdir);

    const std::array<std::uint8_t, 3> aux = {0xAAU, 0xBBU, 0xCCU};
    ASSERT_EQ(sink.report(sink.self,
                          0x01020304U,
                          MIGRIS_PUS5_SEV_LOW,
                          MIGRIS_PUS5_EVT_TC_REJECTED,
                          aux.data(),
                          aux.size()),
              0);

    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};
    ASSERT_GT(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
    const Pus5 d = decode(out.data());
    EXPECT_EQ(d.secondary.service_subtype, MIGRIS_PUS5_SUBTYPE_LOW);
    EXPECT_EQ(d.event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(d.aux, (std::vector<std::uint8_t>{0xAAU, 0xBBU, 0xCCU}));
    EXPECT_TRUE(d.crc_ok);
}

TEST(Fdir, DrainOnEmptyReturnsZeroAndDoesNotTouchState) {
    auto fdir = make_fdir();
    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 3U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};
    EXPECT_EQ(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
    EXPECT_EQ(seq, 3U);
    EXPECT_EQ(pus5.msg_counter[0], 0U);
}

TEST(Fdir, DrainWithTooSmallBufferPreservesTheEvent) {
    auto fdir = make_fdir();
    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 0U;
    ASSERT_EQ(migris_fdir_report_anomaly(&fdir, MIGRIS_FDIR_ANOM_RX_OVERFLOW, 0U, 1U),
              MIGRIS_EVENT_FIFO_OK);

    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE - 1U> small{};
    EXPECT_EQ(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, small.data(), small.size()),
              MIGRIS_PUS5_ERR_BUF_TOO_SMALL);

    // Nothing was consumed: a correctly-sized drain still finds it.
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};
    EXPECT_GT(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
}

// Note: the registry's default-arm guard (an unknown anomaly id →
// MIGRIS_EVENT_FIFO_ERR_BAD_ARG, nothing enqueued) is defensive depth
// for a corrupted C caller. It is not exercised here: constructing an
// out-of-range value of an unscoped enum without a fixed underlying
// type is unspecified in C++ and flagged by clang-analyzer
// EnumCastOutOfRange, so a "bad anomaly id" case cannot be expressed
// portably from this C++ harness. This mirrors the identical decision
// for the bad-severity guard in tests/pus5_test.cpp.

TEST(Fdir, EventsDrainInFifoOrderAcrossBothProducers) {
    auto fdir = make_fdir();
    const migris_event_sink_t sink = migris_fdir_event_sink(&fdir);

    // Producer A (typed): a TC rejection. Producer B (generic sink): an
    // RX overflow already classified. Drained oldest-first regardless
    // of which producer enqueued it.
    ASSERT_EQ(migris_fdir_report_anomaly(&fdir, MIGRIS_FDIR_ANOM_TC_REJECTED, 1U, 0U),
              MIGRIS_EVENT_FIFO_OK);
    const std::array<std::uint8_t, 4> aux = {0x00U, 0x00U, 0x00U, 0x2AU};
    ASSERT_EQ(sink.report(sink.self,
                          2U,
                          MIGRIS_PUS5_SEV_MEDIUM,
                          MIGRIS_PUS5_EVT_RX_OVERFLOW,
                          aux.data(),
                          aux.size()),
              0);

    migris_pus5_ctx_t pus5{};
    std::uint16_t seq = 0U;
    std::array<std::uint8_t, MIGRIS_PUS5_TM_MAX_PACKET_SIZE> out{};

    ASSERT_GT(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
    const Pus5 first = decode(out.data());
    ASSERT_GT(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
    const Pus5 second = decode(out.data());

    EXPECT_EQ(first.event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(second.event_id, MIGRIS_PUS5_EVT_RX_OVERFLOW);
    EXPECT_EQ(first.primary.seq_count, 0U);
    EXPECT_EQ(second.primary.seq_count, 1U);  // one shared monotonic per-APID space
    EXPECT_EQ(migris_fdir_drain(&fdir, test_apid, &seq, &pus5, out.data(), out.size()), 0);
}

// --- Isolation / recovery (slice fsw-14) ----------------------------

TEST(FdirRecovery, BelowThresholdDoesNotConfirm) {
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 3U);

    reject_n(fdir, 2);  // two transients — below the threshold of 3
    EXPECT_EQ(migris_mode_current(&mode), m_nominal);  // not safed
    EXPECT_EQ(count_event(drain_all(fdir), MIGRIS_PUS5_EVT_FDIR_RECOVERY), 0);
}

TEST(FdirRecovery, AtThresholdConfirmsAndCommandsSafeMode) {
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 3U);

    reject_n(fdir, 3);                              // the third rejection confirms the fault
    EXPECT_EQ(migris_mode_current(&mode), m_safe);  // autonomously safed

    const auto events = drain_all(fdir);
    ASSERT_EQ(events.size(), 5U);  // 3x TC_REJECTED, FDIR_RECOVERY, MODE_CHANGED
    EXPECT_EQ(events[3].event_id, MIGRIS_PUS5_EVT_FDIR_RECOVERY);
    EXPECT_EQ(events[3].secondary.service_subtype, MIGRIS_PUS5_SUBTYPE_HIGH);
    // aux: anomaly TC_REJECTED (0), safe mode, occurrence count 3 (BE u16).
    EXPECT_EQ(events[3].aux, (std::vector<std::uint8_t>{0U, m_safe, 0U, 3U}));
    EXPECT_EQ(events[4].event_id, MIGRIS_PUS5_EVT_MODE_CHANGED);
    EXPECT_EQ(events[4].aux, (std::vector<std::uint8_t>{m_nominal, m_safe}));
}

TEST(FdirRecovery, SinkReportedRejectionsConfirmToo) {
    // The TC router reports a rejection through the generic event sink,
    // not the typed API — confirmation must count it all the same.
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 3U);

    reject_n_via_sink(fdir, 3);
    EXPECT_EQ(migris_mode_current(&mode), m_safe);
    EXPECT_EQ(count_event(drain_all(fdir), MIGRIS_PUS5_EVT_FDIR_RECOVERY), 1);
}

TEST(FdirRecovery, RecoveryLatchesAndFiresOnce) {
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 3U);

    reject_n(fdir, 6);  // three past the threshold
    const auto events = drain_all(fdir);
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_FDIR_RECOVERY), 1);  // latched — once only
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_MODE_CHANGED), 1);
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_TC_REJECTED), 6);  // detection continues
}

TEST(FdirRecovery, DisabledRecoverySuppressesTheTransition) {
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 3U);
    migris_fdir_set_enabled(&fdir, 0);
    EXPECT_EQ(migris_fdir_is_enabled(&fdir), 0);

    reject_n(fdir, 3);  // crosses the threshold while recovery is suppressed
    EXPECT_EQ(migris_mode_current(&mode), m_nominal);  // not safed
    const auto events = drain_all(fdir);
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_TC_REJECTED), 3);    // still detected
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_FDIR_RECOVERY), 0);  // no recovery
}

TEST(FdirRecovery, ThresholdZeroNeverConfirms) {
    migris_fdir_ctx_t fdir = make_fdir();
    migris_mode_manager_t mode{};
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    arm_tc_rejected(fdir, mode, fdir_sink, 0U);  // threshold 0 = detection-only

    reject_n(fdir, 20);
    EXPECT_EQ(migris_mode_current(&mode), m_nominal);
    EXPECT_EQ(count_event(drain_all(fdir), MIGRIS_PUS5_EVT_FDIR_RECOVERY), 0);
}

TEST(FdirRecovery, NullModeManagerIsToleratedAtThreshold) {
    migris_fdir_ctx_t fdir = make_fdir();
    const std::array<migris_fdir_confirm_def_t, 1> confirms = {
        {{MIGRIS_FDIR_ANOM_TC_REJECTED, 2U}}};
    ASSERT_EQ(migris_fdir_arm_recovery(&fdir, nullptr, m_safe, confirms.data(), confirms.size()),
              MIGRIS_EVENT_FIFO_OK);

    reject_n(fdir, 2);  // confirms, but there is no mode manager to command
    const auto events = drain_all(fdir);
    EXPECT_EQ(count_event(events, MIGRIS_PUS5_EVT_FDIR_RECOVERY), 0);  // no target → no recovery
    EXPECT_EQ(fdir.confirmed[0], 1U);  // the fault is still latched as confirmed
}

TEST(FdirRecovery, OccurrenceCountSaturatesAtMax) {
    migris_fdir_ctx_t fdir = make_fdir();
    fdir.occurrences[0] = 0xFFFEU;  // one below the u16 cap
    reject_n(fdir, 5);              // would wrap without the saturate guard
    EXPECT_EQ(fdir.occurrences[0], 0xFFFFU);
}

TEST(FdirRecovery, ArmRecoveryRejectsNullArguments) {
    migris_fdir_ctx_t fdir = make_fdir();
    const std::array<migris_fdir_confirm_def_t, 1> confirms = {
        {{MIGRIS_FDIR_ANOM_TC_REJECTED, 3U}}};
    EXPECT_EQ(migris_fdir_arm_recovery(nullptr, nullptr, m_safe, confirms.data(), confirms.size()),
              MIGRIS_EVENT_FIFO_ERR_BAD_ARG);
    EXPECT_EQ(migris_fdir_arm_recovery(&fdir, nullptr, m_safe, nullptr, 1U),
              MIGRIS_EVENT_FIFO_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::pus::test
