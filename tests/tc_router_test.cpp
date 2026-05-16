// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// TC reception / acceptance / routing — the production TC path as of
// slice fsw-5. A self-contained ground-side TC encoder (pinned to
// docs/wire/pus-1.md and pus-17.md, independent of the FSW decoder)
// builds the stimulus; pure (gtest-free) helpers decode the
// back-to-back TM packets the router emits.

#include "migris/fsw/pus/tc_router.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus17.h"
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
// exercised through TcOpts, not asserted here.
std::vector<std::uint8_t> build_tc(const TcOpts& o) {
    std::vector<std::uint8_t> tc(MIGRIS_PUS17_TC_PACKET_SIZE, 0U);

    const std::uint16_t dl =
        (o.data_length_override >= 0)
            ? static_cast<std::uint16_t>(o.data_length_override)
            : static_cast<std::uint16_t>(MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + 2U - 1U);

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

migris_tc_router_ctx_t make_ctx() {
    migris_tc_router_ctx_t ctx{};
    ctx.apid = test_apid;
    return ctx;
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
    const auto tc = build_tc({.service_type = 3U,
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

}  // namespace
}  // namespace migris::fsw::pus::test
