// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// TC reception / acceptance / routing — the production TC path as of
// slice fsw-5. A self-contained ground-side TC encoder (pinned to
// docs/wire/pus-1.md and pus-17.md, independent of the FSW decoder)
// builds the stimulus; pure (gtest-free) helpers decode the
// back-to-back TM packets the router emits.

#include "migris/fsw/pus/tc_router.h"

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/event_sink.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus17.h"
#include "migris/fsw/pus/pus20.h"
#include "migris/fsw/pus/pus3.h"
#include "migris/fsw/pus/pus5.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

struct TcOpts {
    std::uint8_t service_type = MIGRIS_PUS_SERVICE_TEST;
    std::uint8_t service_subtype = MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC;
    std::uint8_t ack_flags = 0U;
    std::uint16_t source_id = 0U;
    std::uint16_t seq_count = 0U;
    std::uint16_t apid = test_apid;
    bool corrupt_crc = false;
    std::uint8_t pus_version = MIGRIS_PUS_VERSION_C;
    int data_length_override = -1;  // < 0 → correct value
};

// Ground-side PUS-C TC encoder. Pure: any structural problem is
// exercised through TcOpts, not asserted here. `app_data` is a
// separate defaulted argument rather than a TcOpts member: a member
// without a default member initialiser trips GCC
// -Werror=missing-field-initializers at every designated-init call
// site, while giving it a `{}` initialiser trips clang-tidy
// readability-redundant-member-init — a genuine two-linter conflict
// the parameter form sidesteps. Empty app_data → byte-identical to
// the original 13-byte PUS-17[1] builder.
std::vector<std::uint8_t> build_tc(const TcOpts& o,
                                   const std::vector<std::uint8_t>& app_data = {}) {
    const std::size_t tc_size = MIGRIS_CCSDS_PRIMARY_HEADER_SIZE +
                                MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + app_data.size() + 2U;
    std::vector<std::uint8_t> tc(tc_size, 0U);

    const std::uint16_t dl = (o.data_length_override >= 0)
                                 ? static_cast<std::uint16_t>(o.data_length_override)
                                 : static_cast<std::uint16_t>(MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE +
                                                              app_data.size() + 2U - 1U);

    const migris_ccsds_primary_header_t primary = {0U,
                                                   MIGRIS_CCSDS_PACKET_TYPE_TC,
                                                   1U,
                                                   o.apid,
                                                   MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
                                                   o.seq_count,
                                                   dl};
    migris_ccsds_primary_pack(&primary, tc.data(), tc.size());

    const migris_pus_tc_secondary_header_t sec = {
        o.pus_version, o.ack_flags, o.service_type, o.service_subtype, o.source_id};
    migris_pus_tc_secondary_pack(
        &sec, &tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE], tc.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE);

    if (!app_data.empty()) {
        std::memcpy(&tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE],
                    app_data.data(),
                    app_data.size());
    }

    const std::size_t crc_off = tc.size() - 2U;
    std::uint16_t crc = migris_crc16_ccitt_false(tc.data(), crc_off);
    if (o.corrupt_crc) {
        crc = static_cast<std::uint16_t>(crc ^ 0xFFFFU);
    }
    tc[crc_off] = static_cast<std::uint8_t>(crc >> 8);
    tc[crc_off + 1U] = static_cast<std::uint8_t>(crc & 0xFFU);
    return tc;
}

struct Tm {
    migris_ccsds_primary_header_t primary{};
    migris_pus_tm_secondary_header_t secondary{};
    std::array<std::uint8_t, MIGRIS_PUS1_REQUEST_ID_SIZE> request_id{};
    int failure_code = -1;  // -1 → success / non-PUS-1 packet
    bool crc_ok = false;
    std::size_t size = 0U;
};

// Pure: decodes one packet at `buf+pos` using the CCSDS length so a
// back-to-back burst can be walked.
Tm decode_at(const std::uint8_t* buf, std::size_t pos) {
    Tm tm{};
    migris_ccsds_primary_unpack(&tm.primary, &buf[pos], MIGRIS_CCSDS_PRIMARY_HEADER_SIZE);
    tm.size = migris_ccsds_packet_total_size(tm.primary.data_length);
    migris_pus_tm_secondary_unpack(&tm.secondary,
                                   &buf[pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                   MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE);
    const std::size_t udf =
        pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    std::memcpy(tm.request_id.data(), &buf[udf], MIGRIS_PUS1_REQUEST_ID_SIZE);
    if (tm.size == MIGRIS_PUS1_FAILURE_TM_PACKET_SIZE) {
        tm.failure_code = buf[udf + MIGRIS_PUS1_REQUEST_ID_SIZE];
    }
    const std::uint16_t computed = migris_crc16_ccitt_false(&buf[pos], tm.size - 2U);
    const auto on_wire =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(buf[pos + tm.size - 2U]) << 8) |
                                   static_cast<std::uint16_t>(buf[pos + tm.size - 1U]));
    tm.crc_ok = (computed == on_wire);
    return tm;
}

std::vector<Tm> decode_all(const std::uint8_t* buf, std::size_t total) {
    std::vector<Tm> out;
    std::size_t pos = 0U;
    while (pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE <= total) {
        const Tm tm = decode_at(buf, pos);
        pos += tm.size;
        out.push_back(tm);
    }
    return out;
}

// Compact identity of a TM: service_type and subtype in one int, so a
// whole emitted sequence can be asserted in a single comparison.
int key(const Tm& tm) {
    return static_cast<int>(tm.secondary.service_type) * 256 +
           static_cast<int>(tm.secondary.service_subtype);
}

constexpr int pus1_accept_key =
    MIGRIS_PUS_SERVICE_VERIFICATION * 256 + MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS;
constexpr int pus1_accept_fail_key =
    MIGRIS_PUS_SERVICE_VERIFICATION * 256 + MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE;
constexpr int pus1_complete_key =
    MIGRIS_PUS_SERVICE_VERIFICATION * 256 + MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS;
constexpr int pus1_complete_fail_key =
    MIGRIS_PUS_SERVICE_VERIFICATION * 256 + MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE;
constexpr int pus17_tm_key = MIGRIS_PUS_SERVICE_TEST * 256 + MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM;
constexpr int pus3_hk_key =
    MIGRIS_PUS_SERVICE_HOUSEKEEPING * 256 + MIGRIS_PUS3_SUBTYPE_HK_PARAM_REPORT;

// Param-block offsets within a PUS-3[25] report's *source data*
// (docs/wire/pus-3.md); source data starts after primary(6)+TMsec(10).
constexpr std::size_t hk_off_accepted = 17U;
constexpr std::size_t hk_off_drops = 25U;

// One-shot-poll TC application data = the Structure ID, big-endian.
std::vector<std::uint8_t> sid_app(std::uint16_t sid) {
    return {static_cast<std::uint8_t>(sid >> 8), static_cast<std::uint8_t>(sid & 0xFFU)};
}

// Big-endian u32 from a PUS-3[25] packet at `pkt`, `src_off` bytes
// into its source data.
std::uint32_t hk_u32(const std::uint8_t* pkt, std::size_t src_off) {
    const std::size_t at =
        MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + src_off;
    return (static_cast<std::uint32_t>(pkt[at]) << 24) |
           (static_cast<std::uint32_t>(pkt[at + 1U]) << 16) |
           (static_cast<std::uint32_t>(pkt[at + 2U]) << 8) |
           static_cast<std::uint32_t>(pkt[at + 3U]);
}

migris_tc_router_ctx_t make_ctx() {
    migris_tc_router_ctx_t ctx{};
    ctx.apid = test_apid;
    return ctx;
}

// Source-data offset of the four PUS-5 message counters within a
// PUS-3[25] report (docs/wire/pus-3.md wire bytes 28..31).
constexpr std::size_t hk_off_pus5 = 12U;

std::uint8_t hk_byte(const std::uint8_t* pkt, std::size_t src_off) {
    return pkt[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE + src_off];
}

// Mock FDIR event sink: records what the router reported. The thunk is
// `extern "C"` so it is assignment-compatible with the C-linkage
// function pointer in migris_event_sink_t.
struct SinkSpy {
    int calls = 0;
    std::uint32_t last_now = 0U;
    int last_severity = -1;
    std::uint16_t last_event_id = 0U;
    std::vector<std::uint8_t> last_aux;
};

extern "C" {
int tc_router_test_spy_report(void* self,
                              std::uint32_t now_seconds,
                              migris_pus5_severity_t severity,
                              std::uint16_t event_id,
                              const std::uint8_t* aux,
                              std::size_t aux_len) {
    auto* spy = static_cast<SinkSpy*>(self);
    spy->calls++;
    spy->last_now = now_seconds;
    spy->last_severity = static_cast<int>(severity);
    spy->last_event_id = event_id;
    spy->last_aux.assign(aux, aux + aux_len);
    return 0;
}
}

migris_event_sink_t spy_sink(SinkSpy& spy) {
    migris_event_sink_t sink;
    sink.report = tc_router_test_spy_report;
    sink.self = &spy;
    return sink;
}

TEST(TcRouter, AcceptedNoAckFlagsEmitsOnlyServiceTm) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.source_id = 0xCAFEU});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    ASSERT_EQ(n, static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus17_tm_key);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, AcceptanceFlagEmitsPus1ThenServiceTm) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE, .source_id = 0xCAFEU});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);

    const std::array<int, 2> seq = {tms[0].primary.seq_count, tms[1].primary.seq_count};
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus17_tm_key}));
    EXPECT_EQ(seq, (std::array<int, 2>{0, 1}));
    EXPECT_EQ(std::memcmp(tms[0].request_id.data(), tc.data(), MIGRIS_PUS1_REQUEST_ID_SIZE), 0);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);
    EXPECT_TRUE(tms[0].crc_ok && tms[1].crc_ok);
}

TEST(TcRouter, AcceptanceAndCompletionEmitThreePackets) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x1234U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 3U);

    EXPECT_EQ((std::array<int, 3>{key(tms[0]), key(tms[1]), key(tms[2])}),
              (std::array<int, 3>{pus1_accept_key, pus17_tm_key, pus1_complete_key}));
    EXPECT_EQ((std::array<int, 3>{
                  tms[0].primary.seq_count, tms[1].primary.seq_count, tms[2].primary.seq_count}),
              (std::array<int, 3>{0, 1, 2}));
    EXPECT_TRUE(tms[0].crc_ok && tms[1].crc_ok && tms[2].crc_ok);
    EXPECT_EQ(tms[2].secondary.destination_id, 0x1234U);
}

TEST(TcRouter, CompletionFlagOnlyEmitsServiceThenCompletion) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_COMPLETION, .source_id = 0x77U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus17_tm_key, pus1_complete_key}));
    EXPECT_EQ((std::array<int, 2>{tms[0].primary.seq_count, tms[1].primary.seq_count}),
              (std::array<int, 2>{0, 1}));
}

TEST(TcRouter, BadCrcWithAcceptanceEmitsOnlyFailureReport) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0xAA55U,
                  .corrupt_crc = true});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);  // rejected: no routing, no completion
    EXPECT_EQ(key(tms[0]), pus1_accept_fail_key);
    EXPECT_EQ(tms[0].failure_code, static_cast<int>(MIGRIS_PUS1_FC_CRC_FAILURE));
    EXPECT_EQ(tms[0].secondary.destination_id, 0xAA55U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, BadCrcWithoutAckFlagsIsSilent) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.corrupt_crc = true});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size()), 0);
}

TEST(TcRouter, UnknownServiceFailsAcceptance) {
    auto ctx = make_ctx();
    // Service 99 is not routable on this AP (17 and 3 are). Service 3
    // is now a valid service — see the PUS-3 tests below.
    const auto tc = build_tc({.service_type = 99U,
                              .service_subtype = 1U,
                              .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE,
                              .source_id = 0x42U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus1_accept_fail_key);
    EXPECT_EQ(tms[0].failure_code, static_cast<int>(MIGRIS_PUS1_FC_UNKNOWN_SERVICE));
}

TEST(TcRouter, UnknownSubtypeAcceptedButCompletionFails) {
    auto ctx = make_ctx();
    // Service 17 is routable; subtype 3 is not implemented → accepted,
    // then completion fails with UNKNOWN_SUBTYPE. No PUS-17[2] emitted.
    const auto tc =
        build_tc({.service_subtype = 3U,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x9001U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_fail_key}));
    EXPECT_EQ(tms[1].failure_code, static_cast<int>(MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE));
    EXPECT_EQ((std::array<int, 2>{tms[0].primary.seq_count, tms[1].primary.seq_count}),
              (std::array<int, 2>{0, 1}));
}

TEST(TcRouter, LengthErrorIsReportedEvenWithoutAckFlags) {
    auto ctx = make_ctx();
    // Forge a Packet Data Length that disagrees with the bytes sent.
    const auto tc = build_tc({.source_id = 0x55U, .data_length_override = 99});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus1_accept_fail_key);
    EXPECT_EQ(tms[0].failure_code, static_cast<int>(MIGRIS_PUS1_FC_LENGTH_ERROR));
    EXPECT_EQ(tms[0].secondary.destination_id, 0U);  // source unparsed
}

TEST(TcRouter, BadPusVersionFailsAcceptance) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE, .source_id = 0x1U, .pus_version = 1U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(tms[0].failure_code, static_cast<int>(MIGRIS_PUS1_FC_BAD_PUS_VERSION));
}

TEST(TcRouter, WrongApidProducesNoOutput) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE, .apid = 0x101U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size()), 0);
}

TEST(TcRouter, NonTcPacketProducesNoOutput) {
    auto ctx = make_ctx();
    auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE});
    tc[0] = static_cast<std::uint8_t>(tc[0] & 0xEFU);  // clear TC type bit → looks like TM
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size()), 0);
}

TEST(TcRouter, SharedSeqCounterIsCoherentAcrossConsecutiveTcs) {
    auto ctx = make_ctx();
    const std::uint8_t flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION;
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const auto tc0 = build_tc({.ack_flags = flags, .source_id = 0xBEEFU, .seq_count = 0U});
    const int n0 =
        migris_tc_router_dispatch(&ctx, 0U, tc0.data(), tc0.size(), out.data(), out.size());
    const auto a = decode_all(out.data(), static_cast<std::size_t>(n0));

    const auto tc1 = build_tc({.ack_flags = flags, .source_id = 0xBEEFU, .seq_count = 1U});
    const int n1 =
        migris_tc_router_dispatch(&ctx, 0U, tc1.data(), tc1.size(), out.data(), out.size());
    const auto b = decode_all(out.data(), static_cast<std::size_t>(n1));

    ASSERT_EQ(a.size(), 3U);
    ASSERT_EQ(b.size(), 3U);
    // Two TCs × three packets: one shared, strictly monotonic per-APID
    // sequence space across PUS-1 and PUS-17.
    const std::array<int, 6> seqs = {a[0].primary.seq_count,
                                     a[1].primary.seq_count,
                                     a[2].primary.seq_count,
                                     b[0].primary.seq_count,
                                     b[1].primary.seq_count,
                                     b[2].primary.seq_count};
    EXPECT_EQ(seqs, (std::array<int, 6>{0, 1, 2, 3, 4, 5}));
}

TEST(TcRouter, RejectsTooSmallOutputBuffer) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM - 1> out{};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size()),
              MIGRIS_TC_ROUTER_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(ctx.tm_seq_count, 0U);  // nothing emitted, no state change
}

TEST(TcRouterAccept, ClassifiesAddressedGoodTc) {
    migris_tc_accept_result_t r{};
    const auto good = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE, .source_id = 0xCAFEU});
    migris_tc_accept(good.data(), good.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_NONE);
    EXPECT_EQ(r.ack_flags, MIGRIS_PUS_TC_ACK_ACCEPTANCE);
    EXPECT_EQ(r.source_id, 0xCAFEU);
    EXPECT_EQ(r.service_type, MIGRIS_PUS_SERVICE_TEST);
}

TEST(TcRouterAccept, IgnoresWrongApidAndFlagsBadCrc) {
    migris_tc_accept_result_t r{};
    const auto wrong_apid = build_tc({.apid = 0x102U});
    migris_tc_accept(wrong_apid.data(), wrong_apid.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 0);

    const auto bad_crc = build_tc({.corrupt_crc = true});
    migris_tc_accept(bad_crc.data(), bad_crc.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_CRC_FAILURE);
}

// --- PUS-3 housekeeping: the router is now multi-service ---------------

TEST(TcRouterAccept, ClassifiesPus3OneShotPollAsAccepted) {
    migris_tc_accept_result_t r{};
    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                              .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                              .source_id = 0x55U},
                             sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    migris_tc_accept(tc.data(), tc.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_NONE);
    EXPECT_EQ(r.service_type, MIGRIS_PUS_SERVICE_HOUSEKEEPING);
    EXPECT_EQ(r.service_subtype, MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL);
    EXPECT_EQ(r.source_id, 0x55U);
}

TEST(TcRouter, Pus3OneShotPollEmitsHkReport) {
    auto ctx = make_ctx();
    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                              .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                              .source_id = 0xCAFEU},
                             sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    ASSERT_EQ(n, static_cast<int>(MIGRIS_PUS3_HK_TM_PACKET_SIZE));
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus3_hk_key);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);  // echoes the poll source
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, Pus3PollAcceptanceAndCompletionEmitThreePackets) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                  .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x1234U},
                 sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    // 22 + 47 + 22 = 91 — proves the MIGRIS_TC_ROUTER_MAX_TM bump.
    ASSERT_EQ(n,
              static_cast<int>(MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE + MIGRIS_PUS3_HK_TM_PACKET_SIZE +
                               MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE));
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 3U);
    EXPECT_EQ((std::array<int, 3>{key(tms[0]), key(tms[1]), key(tms[2])}),
              (std::array<int, 3>{pus1_accept_key, pus3_hk_key, pus1_complete_key}));
    EXPECT_EQ((std::array<int, 3>{
                  tms[0].primary.seq_count, tms[1].primary.seq_count, tms[2].primary.seq_count}),
              (std::array<int, 3>{0, 1, 2}));
    EXPECT_TRUE(tms[0].crc_ok && tms[1].crc_ok && tms[2].crc_ok);
    EXPECT_EQ(tms[2].secondary.destination_id, 0x1234U);
}

TEST(TcRouter, Pus3UnknownSidAcceptedButCompletionFails) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                  .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x9001U},
                 sid_app(0x0099U));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);  // accept ok, no report, completion failure
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_fail_key}));
    EXPECT_EQ(tms[1].failure_code, static_cast<int>(MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE));
}

TEST(TcRouter, Pus3UnknownSubtypeAcceptedButCompletionFails) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                  .service_subtype = 99U,  // service 3 routable, subtype 99 is not
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x7U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_fail_key}));
    EXPECT_EQ(tms[1].failure_code, static_cast<int>(MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE));
}

TEST(TcRouter, AcceptedRejectedCountersTrackVerdicts) {
    auto ctx = make_ctx();
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const auto ok17 = build_tc({.source_id = 0x1U});
    const auto bad17 = build_tc({.corrupt_crc = true});
    const auto ok3 = build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                               .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                               .source_id = 0x2U},
                              sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    migris_tc_router_dispatch(&ctx, 0U, ok17.data(), ok17.size(), out.data(), out.size());
    migris_tc_router_dispatch(&ctx, 0U, bad17.data(), bad17.size(), out.data(), out.size());
    migris_tc_router_dispatch(&ctx, 0U, ok3.data(), ok3.size(), out.data(), out.size());

    EXPECT_EQ(ctx.tc_accepted_count, 2U);
    EXPECT_EQ(ctx.tc_rejected_count, 1U);
}

TEST(TcRouter, HkReportCarriesRouterCounters) {
    auto ctx = make_ctx();
    ctx.rx_ring_overflow_drops = 0x12345678U;  // application-snapshotted ISR count
    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                              .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                              .source_id = 0x3U},
                             sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    ASSERT_EQ(n, static_cast<int>(MIGRIS_PUS3_HK_TM_PACKET_SIZE));
    // The accepted counter is advanced (to 1) before routing, so this
    // very report observes itself counted.
    EXPECT_EQ(hk_u32(out.data(), hk_off_accepted), 1U);
    EXPECT_EQ(hk_u32(out.data(), hk_off_drops), 0x12345678U);
}

// --- fsw-8: PUS-5 hoist (de-zero) + router-side FDIR anomaly ----------

TEST(TcRouter, Pus3PollCarriesLivePus5Counters) {
    auto ctx = make_ctx();
    // The router now owns the PUS-5 context; a [27]-polled report
    // carries the live counters (fsw-7 zero-on-polled asymmetry gone).
    ctx.pus5.msg_counter[0] = 3U;
    ctx.pus5.msg_counter[1] = 5U;
    ctx.pus5.msg_counter[2] = 7U;
    ctx.pus5.msg_counter[3] = 9U;
    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_HOUSEKEEPING,
                              .service_subtype = MIGRIS_PUS3_SUBTYPE_ONE_SHOT_POLL,
                              .source_id = 0x44U},
                             sid_app(MIGRIS_PUS3_SID_FRAMEWORK_DIAG));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    ASSERT_EQ(n, static_cast<int>(MIGRIS_PUS3_HK_TM_PACKET_SIZE));
    EXPECT_EQ((std::array<std::uint8_t, 4>{hk_byte(out.data(), hk_off_pus5),
                                           hk_byte(out.data(), hk_off_pus5 + 1U),
                                           hk_byte(out.data(), hk_off_pus5 + 2U),
                                           hk_byte(out.data(), hk_off_pus5 + 3U)}),
              (std::array<std::uint8_t, 4>{3U, 5U, 7U, 9U}));
}

TEST(TcRouter, CrcRejectionFiresOneLowAnomalyWithCause) {
    auto ctx = make_ctx();
    SinkSpy spy;
    const auto sink = spy_sink(spy);
    ctx.sink = &sink;
    const auto tc = build_tc({.service_subtype = MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                              .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE,
                              .source_id = 0xAA55U,
                              .corrupt_crc = true});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n =
        migris_tc_router_dispatch(&ctx, 0x42U, tc.data(), tc.size(), out.data(), out.size());
    // PUS-1[2] still emitted (ack requested); the anomaly is separate.
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus1_accept_fail_key);

    EXPECT_EQ(spy.calls, 1);
    EXPECT_EQ(spy.last_now, 0x42U);
    EXPECT_EQ(spy.last_severity, static_cast<int>(MIGRIS_PUS5_SEV_LOW));
    EXPECT_EQ(spy.last_event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(spy.last_aux,
              (std::vector<std::uint8_t>{static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE),
                                         MIGRIS_PUS_SERVICE_TEST,
                                         MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC}));
}

TEST(TcRouter, NoAckRejectionIsPus1SilentButStillRaisesAnomaly) {
    // The key fsw-8 correctness invariant: PUS-1 "no-ack ⇒ silence" is
    // preserved (rule 3, byte-for-byte) while the spontaneous PUS-5
    // FDIR anomaly fires regardless of the ack flags.
    auto ctx = make_ctx();
    SinkSpy spy;
    const auto sink = spy_sink(spy);
    ctx.sink = &sink;
    const auto tc = build_tc({.corrupt_crc = true});  // no ack flags
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 7U, tc.data(), tc.size(), out.data(), out.size());
    EXPECT_EQ(n, 0);  // PUS-1 silent — no verification requested
    EXPECT_EQ(spy.calls, 1);
    EXPECT_EQ(spy.last_severity, static_cast<int>(MIGRIS_PUS5_SEV_LOW));
    EXPECT_EQ(spy.last_event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(spy.last_aux[0], static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE));
}

TEST(TcRouter, LengthErrorAnomalyHasZeroServiceFields) {
    auto ctx = make_ctx();
    SinkSpy spy;
    const auto sink = spy_sink(spy);
    ctx.sink = &sink;
    // A forged data length makes the secondary header unparseable, so
    // service type/subtype are not known — aux carries them as 0.
    const auto tc = build_tc({.source_id = 0x55U, .data_length_override = 99});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    EXPECT_EQ(spy.calls, 1);
    EXPECT_EQ(spy.last_event_id, MIGRIS_PUS5_EVT_TC_REJECTED);
    EXPECT_EQ(spy.last_aux,
              (std::vector<std::uint8_t>{
                  static_cast<std::uint8_t>(MIGRIS_PUS1_FC_LENGTH_ERROR), 0U, 0U}));
}

TEST(TcRouter, AcceptedTcFiresNoAnomaly) {
    auto ctx = make_ctx();
    SinkSpy spy;
    const auto sink = spy_sink(spy);
    ctx.sink = &sink;
    const auto tc = build_tc({.ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE, .source_id = 0x1U});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    EXPECT_EQ(spy.calls, 0);  // a clean command is not an anomaly
}

TEST(TcRouter, PartiallyInitialisedSinkIsSafe) {
    auto ctx = make_ctx();
    migris_event_sink_t sink;
    sink.report = nullptr;  // non-NULL sink, NULL report — must not crash
    sink.self = nullptr;
    ctx.sink = &sink;
    const auto tc = build_tc({.corrupt_crc = true});
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size()), 0);
}

// --- fsw-9: PUS-20 parameter management (the third routed service) ----

constexpr int pus20_report_key =
    MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER * 256 + MIGRIS_PUS20_SUBTYPE_VALUE_REPORT;

migris_dp_param_t
dp_param(migris_dp_param_id_t id, migris_dp_access_t access, migris_dp_value_t value) {
    migris_dp_param_t out{};
    out.id = id;
    out.access = access;
    out.value = value;
    return out;
}

// [20,1] application data: 1-byte count + 2-byte big-endian IDs.
std::vector<std::uint8_t> pus20_report_app(const std::vector<std::uint16_t>& ids) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(ids.size()));
    for (const std::uint16_t id : ids) {
        app.push_back(static_cast<std::uint8_t>(id >> 8));
        app.push_back(static_cast<std::uint8_t>(id & 0xFFU));
    }
    return app;
}

// [20,3] application data: count 1 + (2-byte ID, 4-byte u32 value).
std::vector<std::uint8_t> pus20_set_u32_app(std::uint16_t id, std::uint32_t value) {
    std::vector<std::uint8_t> app;
    app.push_back(static_cast<std::uint8_t>(1U));
    app.push_back(static_cast<std::uint8_t>(id >> 8));
    app.push_back(static_cast<std::uint8_t>(id & 0xFFU));
    app.push_back(static_cast<std::uint8_t>(value >> 24));
    app.push_back(static_cast<std::uint8_t>(value >> 16));
    app.push_back(static_cast<std::uint8_t>(value >> 8));
    app.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    return app;
}

TEST(TcRouterAccept, ClassifiesPus20ReportRequestAsAccepted) {
    migris_tc_accept_result_t r{};
    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
                              .service_subtype = MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                              .source_id = 0x55U},
                             pus20_report_app({0x0001U}));
    migris_tc_accept(tc.data(), tc.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_NONE);
    EXPECT_EQ(r.service_type, MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER);
    EXPECT_EQ(r.service_subtype, MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST);
}

TEST(TcRouter, Pus20ReportRequestEmitsValueReport) {
    auto ctx = make_ctx();
    const std::array<migris_dp_param_t, 1U> defs{
        dp_param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(0xABCDEF01U))};
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);
    ctx.datapool = &dp;

    const auto tc = build_tc({.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
                              .service_subtype = MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                              .source_id = 0xCAFEU},
                             pus20_report_app({0x0001U}));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(key(tms[0]), pus20_report_key);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, Pus20SetRequestMutatesDatapoolAndCompletes) {
    auto ctx = make_ctx();
    const std::array<migris_dp_param_t, 1U> defs{
        dp_param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(100U))};
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);
    ctx.datapool = &dp;

    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
                  .service_subtype = MIGRIS_PUS20_SUBTYPE_SET_REQUEST,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x1234U},
                 pus20_set_u32_app(0x0001U, 0xDEADBEEFU));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    // A [20,3] set emits no service TM — only PUS-1 acceptance + completion.
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_key}));
    EXPECT_TRUE(tms[0].crc_ok && tms[1].crc_ok);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 0xDEADBEEFU);
}

TEST(TcRouter, Pus20NullDatapoolFailsCompletion) {
    auto ctx = make_ctx();  // ctx.datapool is left NULL
    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
                  .service_subtype = MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x9001U},
                 pus20_report_app({0x0001U}));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);  // accepted, no report, completion failure
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_fail_key}));
    EXPECT_EQ(tms[1].failure_code, static_cast<int>(MIGRIS_PUS1_FC_EXEC_FAILURE));
}

TEST(TcRouter, Pus20UnknownIdAcceptedButCompletionFails) {
    auto ctx = make_ctx();
    const std::array<migris_dp_param_t, 1U> defs{
        dp_param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(1U))};
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);
    ctx.datapool = &dp;

    const auto tc =
        build_tc({.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
                  .service_subtype = MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
                  .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                  .source_id = 0x9001U},
                 pus20_report_app({0x0999U}));  // 0x0999 is undefined
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ((std::array<int, 2>{key(tms[0]), key(tms[1])}),
              (std::array<int, 2>{pus1_accept_key, pus1_complete_fail_key}));
    EXPECT_EQ(tms[1].failure_code, static_cast<int>(MIGRIS_PUS1_FC_EXEC_FAILURE));
}

TEST(TcRouter, Pus20MaxReportBurstFitsRouterBuffer) {
    auto ctx = make_ctx();
    // Eight f32 parameters → the worst-case [20,2] report (67 bytes).
    std::array<migris_dp_param_t, 8U> defs{};
    for (std::size_t i = 0U; i < defs.size(); ++i) {
        defs[i] = dp_param(static_cast<migris_dp_param_id_t>(0x0001U + i),
                           MIGRIS_DP_ACCESS_READ_WRITE,
                           migris_dp_f32(1.0F));
    }
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);
    ctx.datapool = &dp;

    const auto tc = build_tc(
        {.service_type = MIGRIS_PUS_SERVICE_ONBOARD_PARAMETER,
         .service_subtype = MIGRIS_PUS20_SUBTYPE_REPORT_REQUEST,
         .ack_flags = MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
         .source_id = 0x1234U},
        pus20_report_app({0x0001U, 0x0002U, 0x0003U, 0x0004U, 0x0005U, 0x0006U, 0x0007U, 0x0008U}));
    std::array<std::uint8_t, MIGRIS_TC_ROUTER_MAX_TM> out{};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out.data(), out.size());
    // 22 (accept) + 67 (8×f32 report) + 22 (completion) = 111 — proves
    // the MIGRIS_TC_ROUTER_MAX_TM 96 → 128 bump.
    ASSERT_EQ(n,
              static_cast<int>(MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE +
                               MIGRIS_PUS20_TM_MAX_PACKET_SIZE +
                               MIGRIS_PUS1_SUCCESS_TM_PACKET_SIZE));
    const auto tms = decode_all(out.data(), static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 3U);
    EXPECT_EQ((std::array<int, 3>{key(tms[0]), key(tms[1]), key(tms[2])}),
              (std::array<int, 3>{pus1_accept_key, pus20_report_key, pus1_complete_key}));
    EXPECT_TRUE(tms[0].crc_ok && tms[1].crc_ok && tms[2].crc_ok);
}

}  // namespace
}  // namespace migris::fsw::pus::test
