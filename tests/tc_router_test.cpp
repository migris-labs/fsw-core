// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// TC reception / acceptance / routing — the production TC path as of
// slice fsw-5. A self-contained ground-side TC encoder (pinned to
// docs/wire/pus-1.md and pus-17.md, independent of the FSW decoder)
// builds the stimulus; the helpers below decode the back-to-back TM
// packets the router emits.

#include "migris/fsw/pus/tc_router.h"

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus1.h"
#include "migris/fsw/pus/pus17.h"
#include "migris/fsw/pus/pus_tc.h"
#include "migris/fsw/pus/pus_tm.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace migris::fsw::pus::test {
namespace {

constexpr std::uint16_t test_apid = 0x100U;

// Ground-side PUS-C TC encoder. `data_length_override` < 0 means
// "use the correct value"; a non-negative value forges an
// inconsistent CCSDS Packet Data Length (to exercise length errors).
std::vector<std::uint8_t> build_tc(std::uint8_t service_type,
                                   std::uint8_t service_subtype,
                                   std::uint8_t ack_flags,
                                   std::uint16_t source_id,
                                   std::uint16_t seq_count,
                                   std::uint16_t apid = test_apid,
                                   bool corrupt_crc = false,
                                   std::uint8_t pus_version = MIGRIS_PUS_VERSION_C,
                                   int data_length_override = -1) {
    std::vector<std::uint8_t> tc(MIGRIS_PUS17_TC_PACKET_SIZE, 0U);

    const std::uint16_t dl =
        (data_length_override >= 0)
            ? static_cast<std::uint16_t>(data_length_override)
            : static_cast<std::uint16_t>(MIGRIS_PUS_TC_SECONDARY_HEADER_SIZE + 2U - 1U);

    const migris_ccsds_primary_header_t primary = {
        /*version=*/0,
        /*type=*/MIGRIS_CCSDS_PACKET_TYPE_TC,
        /*sec_hdr_flag=*/1,
        /*apid=*/apid,
        /*seq_flags=*/MIGRIS_CCSDS_SEQ_FLAGS_UNSEGMENTED,
        /*seq_count=*/seq_count,
        /*data_length=*/dl,
    };
    EXPECT_EQ(migris_ccsds_primary_pack(&primary, tc.data(), tc.size()), MIGRIS_CCSDS_OK);

    const migris_pus_tc_secondary_header_t sec = {
        /*pus_version=*/pus_version,
        /*ack_flags=*/ack_flags,
        /*service_type=*/service_type,
        /*service_subtype=*/service_subtype,
        /*source_id=*/source_id,
    };
    EXPECT_EQ(migris_pus_tc_secondary_pack(&sec,
                                           &tc[MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                           tc.size() - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);

    const std::size_t crc_off = tc.size() - 2U;
    std::uint16_t crc = migris_crc16_ccitt_false(tc.data(), crc_off);
    if (corrupt_crc) {
        crc = static_cast<std::uint16_t>(crc ^ 0xFFFFU);
    }
    tc[crc_off] = static_cast<std::uint8_t>(crc >> 8);
    tc[crc_off + 1U] = static_cast<std::uint8_t>(crc & 0xFFU);
    return tc;
}

std::vector<std::uint8_t>
pus17_tc(std::uint8_t ack_flags, std::uint16_t source_id = 0xCAFEU, std::uint16_t seq_count = 0U) {
    return build_tc(MIGRIS_PUS_SERVICE_TEST,
                    MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                    ack_flags,
                    source_id,
                    seq_count);
}

// A decoded TM packet plus where the next one starts.
struct Tm {
    migris_ccsds_primary_header_t primary{};
    migris_pus_tm_secondary_header_t secondary{};
    std::uint8_t request_id[MIGRIS_PUS1_REQUEST_ID_SIZE]{};
    std::uint8_t failure_code = 0U;  // valid only for PUS-1 failure subtypes
    bool crc_ok = false;
    std::size_t size = 0U;
};

// Decode one packet at `buf+pos`, inferring its size from the CCSDS
// Packet Data Length so a back-to-back burst can be walked.
Tm decode_at(const std::uint8_t* buf, std::size_t pos, std::size_t total) {
    Tm tm{};
    EXPECT_LE(pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE, total);
    EXPECT_EQ(migris_ccsds_primary_unpack(
                  &tm.primary, &buf[pos], total - pos == 0 ? 0 : MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              MIGRIS_CCSDS_OK);
    tm.size = migris_ccsds_packet_total_size(tm.primary.data_length);
    EXPECT_LE(pos + tm.size, total);

    EXPECT_EQ(migris_pus_tm_secondary_unpack(&tm.secondary,
                                             &buf[pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE],
                                             tm.size - MIGRIS_CCSDS_PRIMARY_HEADER_SIZE),
              0);

    const std::size_t udf =
        pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE + MIGRIS_PUS_TM_SECONDARY_HEADER_SIZE;
    for (std::size_t i = 0; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        tm.request_id[i] = buf[udf + i];
    }
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
    while (pos < total) {
        Tm tm = decode_at(buf, pos, total);
        pos += tm.size;
        out.push_back(tm);
    }
    return out;
}

migris_tc_router_ctx_t make_ctx() {
    migris_tc_router_ctx_t ctx{};
    ctx.apid = test_apid;
    return ctx;
}

TEST(TcRouter, AcceptedNoAckFlagsEmitsOnlyServiceTm) {
    auto ctx = make_ctx();
    const auto tc = pus17_tc(/*ack_flags=*/0U, /*source_id=*/0xCAFEU);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    ASSERT_EQ(n, static_cast<int>(MIGRIS_PUS17_TM_PACKET_SIZE));

    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(tms[0].secondary.service_type, MIGRIS_PUS_SERVICE_TEST);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, AcceptanceFlagEmitsPus1ThenServiceTm) {
    auto ctx = make_ctx();
    const auto tc = pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE, /*source_id=*/0xCAFEU, /*seq=*/9U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);

    // PUS-1[1] acceptance success, request ID == TC primary bytes [0..3].
    EXPECT_EQ(tms[0].secondary.service_type, MIGRIS_PUS_SERVICE_VERIFICATION);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS);
    EXPECT_EQ(tms[0].secondary.destination_id, 0xCAFEU);
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    for (std::size_t i = 0; i < MIGRIS_PUS1_REQUEST_ID_SIZE; ++i) {
        EXPECT_EQ(tms[0].request_id[i], tc[i]) << "request id byte " << i;
    }
    EXPECT_TRUE(tms[0].crc_ok);

    // PUS-17[2], shared sequence count advanced.
    EXPECT_EQ(tms[1].secondary.service_type, MIGRIS_PUS_SERVICE_TEST);
    EXPECT_EQ(tms[1].secondary.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tms[1].primary.seq_count, 1U);
    EXPECT_TRUE(tms[1].crc_ok);
}

TEST(TcRouter, AcceptanceAndCompletionEmitThreePackets) {
    auto ctx = make_ctx();
    const auto tc = pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                             /*source_id=*/0x1234U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 3U);

    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS);
    EXPECT_EQ(tms[1].secondary.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tms[2].secondary.service_type, MIGRIS_PUS_SERVICE_VERIFICATION);
    EXPECT_EQ(tms[2].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS);

    // Single shared per-APID sequence count, strictly monotonic.
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_EQ(tms[1].primary.seq_count, 1U);
    EXPECT_EQ(tms[2].primary.seq_count, 2U);
    for (const auto& tm : tms) {
        EXPECT_EQ(tm.secondary.destination_id, 0x1234U);
        EXPECT_TRUE(tm.crc_ok);
    }
}

TEST(TcRouter, CompletionFlagOnlyEmitsServiceThenCompletion) {
    auto ctx = make_ctx();
    const auto tc = pus17_tc(MIGRIS_PUS_TC_ACK_COMPLETION, /*source_id=*/0x77U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TM);
    EXPECT_EQ(tms[1].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_SUCCESS);
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_EQ(tms[1].primary.seq_count, 1U);
}

TEST(TcRouter, BadCrcWithAcceptanceEmitsOnlyFailureReport) {
    auto ctx = make_ctx();
    auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                       MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                       MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                       /*source_id=*/0xAA55U,
                       /*seq=*/0U,
                       test_apid,
                       /*corrupt_crc=*/true);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);  // rejected: no routing, no completion
    EXPECT_EQ(tms[0].secondary.service_type, MIGRIS_PUS_SERVICE_VERIFICATION);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE);
    EXPECT_EQ(tms[0].failure_code, static_cast<std::uint8_t>(MIGRIS_PUS1_FC_CRC_FAILURE));
    EXPECT_EQ(tms[0].secondary.destination_id, 0xAA55U);
    EXPECT_TRUE(tms[0].crc_ok);
}

TEST(TcRouter, BadCrcWithoutAckFlagsIsSilent) {
    auto ctx = make_ctx();
    auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                       MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                       /*ack_flags=*/0U,
                       0U,
                       0U,
                       test_apid,
                       /*corrupt_crc=*/true);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out)), 0);
}

TEST(TcRouter, UnknownServiceFailsAcceptance) {
    auto ctx = make_ctx();
    const auto tc =
        build_tc(/*service_type=*/3U, /*subtype=*/1U, MIGRIS_PUS_TC_ACK_ACCEPTANCE, 0x42U, 0U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE);
    EXPECT_EQ(tms[0].failure_code, static_cast<std::uint8_t>(MIGRIS_PUS1_FC_UNKNOWN_SERVICE));
}

TEST(TcRouter, UnknownSubtypeAcceptedButCompletionFails) {
    auto ctx = make_ctx();
    // Service 17 is routable, but subtype 3 is not implemented: the TC
    // is accepted, PUS-17 declines to execute, and the completion
    // report carries UNKNOWN_SUBTYPE. No PUS-17[2] is emitted.
    const auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                             /*subtype=*/3U,
                             MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION,
                             /*source_id=*/0x9001U,
                             0U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 2U);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_SUCCESS);
    EXPECT_EQ(tms[1].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_COMPLETION_FAILURE);
    EXPECT_EQ(tms[1].failure_code, static_cast<std::uint8_t>(MIGRIS_PUS1_FC_UNKNOWN_SUBTYPE));
    EXPECT_EQ(tms[0].primary.seq_count, 0U);
    EXPECT_EQ(tms[1].primary.seq_count, 1U);
}

TEST(TcRouter, LengthErrorIsReportedEvenWithoutAckFlags) {
    auto ctx = make_ctx();
    // Forge a Packet Data Length that disagrees with the bytes sent.
    auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                       MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                       /*ack_flags=*/0U,
                       0x55U,
                       0U,
                       test_apid,
                       false,
                       MIGRIS_PUS_VERSION_C,
                       /*data_length_override=*/99);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(tms[0].secondary.service_subtype, MIGRIS_PUS1_SUBTYPE_ACCEPTANCE_FAILURE);
    EXPECT_EQ(tms[0].failure_code, static_cast<std::uint8_t>(MIGRIS_PUS1_FC_LENGTH_ERROR));
    EXPECT_EQ(tms[0].secondary.destination_id, 0U);  // source unparsed on length error
}

TEST(TcRouter, BadPusVersionFailsAcceptance) {
    auto ctx = make_ctx();
    const auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                             MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                             MIGRIS_PUS_TC_ACK_ACCEPTANCE,
                             0x1U,
                             0U,
                             test_apid,
                             false,
                             /*pus_version=*/1U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
    const auto tms = decode_all(out, static_cast<std::size_t>(n));
    ASSERT_EQ(tms.size(), 1U);
    EXPECT_EQ(tms[0].failure_code, static_cast<std::uint8_t>(MIGRIS_PUS1_FC_BAD_PUS_VERSION));
}

TEST(TcRouter, WrongApidProducesNoOutput) {
    auto ctx = make_ctx();
    const auto tc = build_tc(MIGRIS_PUS_SERVICE_TEST,
                             MIGRIS_PUS17_SUBTYPE_ARE_YOU_ALIVE_TC,
                             MIGRIS_PUS_TC_ACK_ACCEPTANCE,
                             0U,
                             0U,
                             /*apid=*/0x101U);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out)), 0);
}

TEST(TcRouter, NonTcPacketProducesNoOutput) {
    auto ctx = make_ctx();
    auto tc = pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE);
    tc[0] = static_cast<std::uint8_t>(tc[0] & 0xEFU);  // clear the TC type bit → looks like TM
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out)), 0);
}

TEST(TcRouter, SharedSeqCounterIsCoherentAcrossConsecutiveTcs) {
    auto ctx = make_ctx();
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM] = {};

    std::vector<std::uint16_t> seqs;
    for (std::uint16_t i = 0; i < 2U; ++i) {
        const auto tc =
            pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE | MIGRIS_PUS_TC_ACK_COMPLETION, 0xBEEFU, i);
        const int n = migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out));
        for (const auto& tm : decode_all(out, static_cast<std::size_t>(n))) {
            seqs.push_back(tm.primary.seq_count);
            EXPECT_TRUE(tm.crc_ok);
        }
    }
    // Two TCs × three packets each: one shared, strictly monotonic
    // sequence space across PUS-1 and PUS-17.
    ASSERT_EQ(seqs.size(), 6U);
    for (std::uint16_t i = 0; i < 6U; ++i) {
        EXPECT_EQ(seqs[i], i);
    }
}

TEST(TcRouter, RejectsTooSmallOutputBuffer) {
    auto ctx = make_ctx();
    const auto tc = pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE);
    std::uint8_t out[MIGRIS_TC_ROUTER_MAX_TM - 1] = {};
    EXPECT_EQ(migris_tc_router_dispatch(&ctx, 0U, tc.data(), tc.size(), out, sizeof(out)),
              MIGRIS_TC_ROUTER_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(ctx.tm_seq_count, 0U);  // nothing emitted, no state change
}

TEST(TcRouterAccept, ClassifiesAddressedAndFailureCodes) {
    migris_tc_accept_result_t r{};

    const auto good = pus17_tc(MIGRIS_PUS_TC_ACK_ACCEPTANCE, 0xCAFEU);
    migris_tc_accept(good.data(), good.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_NONE);
    EXPECT_EQ(r.ack_flags, MIGRIS_PUS_TC_ACK_ACCEPTANCE);
    EXPECT_EQ(r.source_id, 0xCAFEU);
    EXPECT_EQ(r.service_type, MIGRIS_PUS_SERVICE_TEST);

    const auto wrong_apid = build_tc(MIGRIS_PUS_SERVICE_TEST, 1U, 0U, 0U, 0U, 0x102U);
    migris_tc_accept(wrong_apid.data(), wrong_apid.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 0);

    const auto bad_crc = build_tc(MIGRIS_PUS_SERVICE_TEST, 1U, 0U, 0U, 0U, test_apid, true);
    migris_tc_accept(bad_crc.data(), bad_crc.size(), test_apid, &r);
    EXPECT_EQ(r.addressed, 1);
    EXPECT_EQ(r.fc, MIGRIS_PUS1_FC_CRC_FAILURE);
}

}  // namespace
}  // namespace migris::fsw::pus::test
