# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""fsw-7 PUS-3 housekeeping round-trip over UART.

Boots the *short-period* build of ``samples/tc_uart`` (the same sample
as ``test_tc_uart.py``, but with ``CONFIG_FSW_PUS3_HK_PERIOD_SEC`` pinned
small) so the spontaneous PUS-3[25] cadence is observable fast and
deterministically. Two facts make this suite robust to Renode's idle
virtual-time fast-forward:

  * cadence is asserted on the in-packet CUC ``time_seconds`` (the FSW
    clock), never on host wall-clock; and
  * packets are demultiplexed from the running stream by
    service/type/identity, never by fixed byte offsets — exactly how
    real ground software separates periodic housekeeping from
    command-verification traffic.

The ground-side codec lives in ``_pus.py``, independent of the C codec
under ``lib/pus/`` — the two meet only on the wire.
"""

from __future__ import annotations

import struct
import time

import pytest

from _pus import (
    ACK_ACCEPTANCE,
    ACK_COMPLETION,
    DP_PARAM_FW_BUILD_ID,
    DP_PARAM_FW_VERSION,
    DP_PARAM_HK_PERIOD_SEC,
    DP_TYPE_U16,
    DP_TYPE_U32,
    FSW_APID,
    PACKET_TYPE_TM,
    PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS,
    PUS_1_SUBTYPE_COMPLETION_SUCCESS,
    PUS_3_SUBTYPE_HK_PARAM_REPORT,
    PUS_5_SUBTYPE_HIGH,
    PUS_5_SUBTYPE_INFO,
    PUS_11_SUBTYPE_SUMMARY_REPORT,
    PUS_13_SUBTYPE_FIRST_PART,
    PUS_13_SUBTYPE_LAST_PART,
    PUS_15_SUBTYPE_STORE_REPORT,
    PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM,
    PUS_20_SUBTYPE_VALUE_REPORT,
    PUS_SERVICE_EVENT_REPORTING,
    PUS_SERVICE_HOUSEKEEPING,
    PUS_SERVICE_LARGE_DATA,
    PUS_SERVICE_ONBOARD_PARAMETER,
    PUS_SERVICE_SCHEDULING,
    PUS_SERVICE_STORAGE,
    PUS_SERVICE_TEST,
    PUS_SERVICE_VERIFICATION,
    PUS_VERSION_C,
    PUS3_HK_SOURCE_DATA_SIZE,
    PUS3_SID_FRAMEWORK_DIAG,
    PUS5_EVT_FDIR_RECOVERY,
    PUS5_EVT_FSW_BOOT,
    PUS5_EVT_MODE_CHANGED,
    PUS15_STORE_REPORT_SOURCE_SIZE,
    SEQ_FLAGS_UNSEGMENTED,
    DecodedTm,
    build_pus3_create_structure_tc,
    build_pus3_delete_structure_tc,
    build_pus3_disable_structure_tc,
    build_pus3_enable_structure_tc,
    build_pus3_oneshot_poll_tc,
    build_pus11_enable_tc,
    build_pus11_insert_tc,
    build_pus11_report_tc,
    build_pus15_disable_storage_tc,
    build_pus15_downlink_tc,
    build_pus15_enable_storage_tc,
    build_pus15_report_request_tc,
    build_pus17_are_you_alive_tc,
    build_pus20_report_request_tc,
    build_pus20_set_request_tc,
    decode_pus3_dynamic_report,
    decode_pus11_summary_report,
    decode_pus13_part,
    decode_pus15_store_report,
    decode_pus20_report,
    reassemble_pus13,
    split_packets,
)
from conftest import _RENODE_BIN, _TC_HK_ELF, tc_hk_running  # noqa: F401

# Must match the CI ``zephyr-tc-hk`` build override
# (-DCONFIG_FSW_PUS3_HK_PERIOD_SEC). Kept here so the cadence bound is
# legible next to the assertion.
HK_PERIOD_SEC = 2

pytestmark = [
    pytest.mark.skipif(
        _RENODE_BIN is None,
        reason="Renode not installed (set RENODE_BIN, put `renode` on PATH, "
        "or install Renode.app on macOS)",
    ),
    pytest.mark.skipif(
        _TC_HK_ELF is None,
        reason="short-period tc_uart ELF not built. Build "
        "fsw-core/samples/tc_uart with "
        "-DCONFIG_FSW_PUS3_HK_PERIOD_SEC=2, or set FSW_CORE_TC_HK_ELF.",
    ),
]


def _decode_all(pkts: list[bytes]) -> list[DecodedTm]:
    return [DecodedTm.decode(p) for p in pkts]


def _is_hk(tm: DecodedTm) -> bool:
    return (
        tm.secondary.service_type == PUS_SERVICE_HOUSEKEEPING
        and tm.secondary.service_subtype == PUS_3_SUBTYPE_HK_PARAM_REPORT
    )


def _collect(uart, predicate, *, timeout: float = 60.0, interval: float = 0.1):
    """Poll the cumulative UART buffer, splitting it into whole CCSDS
    packets, until ``predicate(decoded_list)`` returns a truthy value
    (which is returned). Robust to periodic HK interleaving — we never
    assume a fixed byte count."""
    deadline = time.monotonic() + timeout
    while True:
        raw = uart.buffer()
        tms = _decode_all(split_packets(raw))
        result = predicate(tms)
        if result:
            return tms, result
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"predicate unmet within {timeout}s; "
                f"{len(raw)} bytes / {len(tms)} packets: {raw.hex()}"
            )
        time.sleep(interval)


def _assert_boot_first(tms: list[DecodedTm]) -> None:
    """The very first TM is always the PUS-5[1] boot event at seq 0
    (slice fsw-6 invariant, unchanged by fsw-7)."""
    assert tms, "no packets at all"
    boot = tms[0]
    assert boot.primary.seq_count == 0
    assert boot.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
    assert boot.secondary.service_subtype == PUS_5_SUBTYPE_INFO


def test_periodic_hk_report_appears(tc_hk_running) -> None:  # noqa: F811
    """With no TC sent, the FSW spontaneously emits a well-formed
    PUS-3[25] framework housekeeping report a period after boot, on
    the shared per-APID sequence count, after the boot event."""
    _, uart = tc_hk_running

    tms, hk = _collect(
        uart, lambda ts: next((t for t in ts if _is_hk(t)), None), timeout=60.0
    )
    _assert_boot_first(tms)

    assert hk.primary.type == PACKET_TYPE_TM
    assert hk.primary.apid == FSW_APID
    assert hk.primary.seq_flags == SEQ_FLAGS_UNSEGMENTED
    # The boot event consumed sequence count 0; the housekeeping build
    # also downlinks the fsw-12 PUS-13 large-data demo at boot, so the
    # first report's exact count is not pinned — only that it follows
    # the boot event. Strict monotonicity is the cadence test's job.
    assert hk.primary.seq_count >= 1
    assert hk.secondary.pus_version == PUS_VERSION_C
    assert hk.secondary.destination_id == 0  # spontaneous, no triggering TC
    assert hk.pus3_sid == PUS3_SID_FRAMEWORK_DIAG
    assert len(hk.source_data) == PUS3_HK_SOURCE_DATA_SIZE
    assert hk.crc_ok


def test_periodic_hk_cadence(tc_hk_running) -> None:  # noqa: F811
    """Two consecutive periodic reports are spaced by the configured
    period on the FSW clock, with a strictly increasing shared sequence
    count. Asserting on the in-packet CUC time makes this independent of
    Renode's wall-clock/virtual-time ratio."""
    _, uart = tc_hk_running

    def two_hk(tms: list[DecodedTm]):
        hk = [t for t in tms if _is_hk(t)]
        return hk if len(hk) >= 2 else None

    _, hks = _collect(uart, two_hk, timeout=60.0)
    a, b = hks[0], hks[1]
    delta = b.secondary.time_seconds - a.secondary.time_seconds
    assert HK_PERIOD_SEC <= delta <= HK_PERIOD_SEC + 1, (
        a.secondary.time_seconds,
        b.secondary.time_seconds,
    )
    assert b.primary.seq_count > a.primary.seq_count
    assert a.crc_ok and b.crc_ok


def test_oneshot_poll_round_trip(tc_hk_running) -> None:  # noqa: F811
    """A PUS-3[27] one-shot poll requesting acceptance + completion
    yields the contiguous burst PUS-1[1] · PUS-3[25] · PUS-1[7] (one
    dispatch, so no periodic report can splice into it), distinguishable
    from the spontaneous reports by destination ID. The shared per-APID
    sequence is strictly monotonic across the triplet. From slice fsw-8
    the polled report also carries the *live* PUS-5 counters (the
    router now owns the PUS-5 context) — the fsw-7 zero-on-the-polled-
    path asymmetry is resolved."""
    _, uart = tc_hk_running

    source_id = 0x0055
    tc = build_pus3_oneshot_poll_tc(
        sid=PUS3_SID_FRAMEWORK_DIAG,
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=source_id,
        seq_count=9,
    )
    request_id = tc[:4]

    def find_triplet(tms: list[DecodedTm]):
        for i in range(len(tms) - 2):
            a, b, c = tms[i], tms[i + 1], tms[i + 2]
            if (
                a.secondary.service_type == PUS_SERVICE_VERIFICATION
                and a.secondary.service_subtype == PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS
                and a.request_id == request_id
                and _is_hk(b)
                and b.secondary.destination_id == source_id
                and c.secondary.service_type == PUS_SERVICE_VERIFICATION
                and c.secondary.service_subtype == PUS_1_SUBTYPE_COMPLETION_SUCCESS
                and c.request_id == request_id
            ):
                return (a, b, c)
        return None

    uart.send(tc)
    _, (acc, hk, comp) = _collect(uart, find_triplet, timeout=60.0)

    # The poll report carries the full 47-byte packet, echoes the
    # triggering source ID (asserted in find_triplet — that is what
    # distinguishes it from the destination_id==0 spontaneous reports),
    # and the shared per-APID sequence is strictly monotonic across the
    # one-dispatch triplet (no periodic report can splice into it). The
    # spontaneous-report path itself is covered by the periodic tests
    # above; the FSW answers this poll within emulated milliseconds, so
    # the triplet generally lands before the first ~period-delayed
    # spontaneous report — asserting one is already present here would
    # be racy and is intentionally not done.
    assert hk.pus3_sid == PUS3_SID_FRAMEWORK_DIAG
    assert len(hk.source_data) == PUS3_HK_SOURCE_DATA_SIZE
    assert acc.primary.seq_count + 1 == hk.primary.seq_count
    assert hk.primary.seq_count + 1 == comp.primary.seq_count
    assert acc.crc_ok and hk.crc_ok and comp.crc_ok

    # fsw-8 de-zero proof: the PUS-5 counter sub-block is source-data
    # bytes [12:16] (wire bytes 28..31, docs/wire/pus-3.md). Pre-fsw-8 a
    # *polled* report hardcoded these to 00 00 00 00; now the router
    # owns the PUS-5 context so they are live. The spontaneous PUS-5[1]
    # boot event already advanced the info counter, so byte 12 (info)
    # is non-zero on this polled report — the asymmetry is gone.
    pus5_counters = hk.source_data[12:16]
    assert pus5_counters[0] >= 1, pus5_counters.hex()


def test_rx_overflow_counter_zero_under_nominal_traffic(tc_hk_running) -> None:  # noqa: F811
    """Under nominal load nothing overflows the UART RX ring, so the
    last counter in the framework HK structure (bytes [25:29], the
    ISR-snapshotted drop count) is zero. Correct *counting* is proven
    host-side; this only proves the field is wired through to the wire."""
    _, uart = tc_hk_running

    _, hk = _collect(
        uart, lambda ts: next((t for t in ts if _is_hk(t)), None), timeout=60.0
    )
    rx_drops = int.from_bytes(hk.source_data[25:29], "big")
    assert rx_drops == 0, hk.source_data.hex()


# The period a PUS-20[3] set retunes the housekeeping cadence to —
# distinct from this build's HK_PERIOD_SEC (2) so a post-set cadence is
# unambiguous on the wire (a pre-set pair can never be this far apart).
NEW_HK_PERIOD_SEC = 4


def test_pus20_set_then_report_round_trips_the_hk_period(  # noqa: F811
    tc_hk_running,
) -> None:
    """A PUS-20[3] set of the housekeeping-period datapool parameter is
    reflected by a subsequent PUS-20[1] report — the new value lands in
    the datapool on real emulated hardware."""
    _, uart = tc_hk_running

    report_source = 0x0066
    uart.send(
        build_pus20_set_request_tc(
            params=[(DP_PARAM_HK_PERIOD_SEC, struct.pack(">I", NEW_HK_PERIOD_SEC))]
        )
    )
    uart.send(
        build_pus20_report_request_tc(
            param_ids=[DP_PARAM_HK_PERIOD_SEC], source_id=report_source
        )
    )

    def find_report(tms: list[DecodedTm]):
        for t in tms:
            if (
                t.secondary.service_type == PUS_SERVICE_ONBOARD_PARAMETER
                and t.secondary.service_subtype == PUS_20_SUBTYPE_VALUE_REPORT
                and t.secondary.destination_id == report_source
            ):
                return t
        return None

    _, report = _collect(uart, find_report, timeout=60.0)
    assert report.crc_ok
    values = decode_pus20_report(report, {DP_PARAM_HK_PERIOD_SEC: DP_TYPE_U32})
    period = int.from_bytes(values[DP_PARAM_HK_PERIOD_SEC], "big")
    assert period == NEW_HK_PERIOD_SEC


def test_pus20_set_retunes_the_live_hk_cadence(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-9 headline: a PUS-20[3] set retunes the running
    housekeeping cadence with no rebuild. After the period is widened
    from this build's 2 s to NEW_HK_PERIOD_SEC, a pair of consecutive
    spontaneous PUS-3[25] reports spaced by the new period appears on
    the FSW clock — a spacing that cannot occur pre-set."""
    _, uart = tc_hk_running

    uart.send(
        build_pus20_set_request_tc(
            params=[(DP_PARAM_HK_PERIOD_SEC, struct.pack(">I", NEW_HK_PERIOD_SEC))]
        )
    )

    def widened_cadence(tms: list[DecodedTm]):
        hk = [t for t in tms if _is_hk(t)]
        for a, b in zip(hk, hk[1:]):
            delta = b.secondary.time_seconds - a.secondary.time_seconds
            if NEW_HK_PERIOD_SEC <= delta <= NEW_HK_PERIOD_SEC + 1:
                return (a, b)
        return None

    _, (a, b) = _collect(uart, widened_cadence, timeout=90.0)
    assert b.primary.seq_count > a.primary.seq_count
    assert a.crc_ok and b.crc_ok


def test_pus11_scheduled_telecommand_is_released_and_executed(  # noqa: F811
    tc_hk_running,
) -> None:
    """The slice fsw-10 headline: a PUS-11[4] insert schedules a
    telecommand; once the schedule is enabled and the release time is
    reached, the FSW autonomously re-dispatches it through the router.
    Here the release time is already in the past, so it fires on the
    next release tick — the released PUS-17[1] yields a PUS-17[2]
    carrying the scheduled telecommand's source id."""
    _, uart = tc_hk_running

    uart.send(build_pus11_enable_tc())
    scheduled = build_pus17_are_you_alive_tc(source_id=0x7711, seq_count=0x0030)
    # Release time 1 is already in the past — due on the next tick.
    uart.send(build_pus11_insert_tc(activities=[(1, scheduled)]))

    def find_released_response(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_TEST
                and t.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
                and t.secondary.destination_id == 0x7711
            ),
            None,
        )

    _, released = _collect(uart, find_released_response, timeout=60.0)
    assert released.crc_ok


def test_pus11_summary_report_round_trip(tc_hk_running) -> None:  # noqa: F811
    """A PUS-11[4] insert followed by a PUS-11[11] summary-report
    request: the [11,12] report lists the activity with its release
    time and request identifier. The release time is far in the future
    so the activity is never released before it is reported."""
    _, uart = tc_hk_running

    scheduled = build_pus17_are_you_alive_tc(source_id=0x4242, seq_count=0x0099)
    release_time = 0xFFFF0000
    uart.send(build_pus11_insert_tc(activities=[(release_time, scheduled)]))

    report_source = 0x0088
    uart.send(build_pus11_report_tc(request_ids=[scheduled[:4]], source_id=report_source))

    def find_report(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_SCHEDULING
                and t.secondary.service_subtype == PUS_11_SUBTYPE_SUMMARY_REPORT
                and t.secondary.destination_id == report_source
            ),
            None,
        )

    _, report = _collect(uart, find_report, timeout=60.0)
    assert report.crc_ok
    assert decode_pus11_summary_report(report) == [(release_time, scheduled[:4])]


# Mirrors Kconfig FSW_PKTSTORE_CAPACITY for the tc_uart sample — the
# on-board packet store wraps after this many packets, overwriting the
# oldest. Kept here so the count bound is legible next to the assertion.
PKTSTORE_CAPACITY = 32


def test_pus15_store_report_round_trip(tc_hk_running) -> None:  # noqa: F811
    """A PUS-15[12] report request yields a well-formed [15,13] packet
    store report: storage enabled (the post-boot default), at least the
    boot event captured, a non-decreasing oldest..newest span, and a
    count bounded by the store capacity."""
    _, uart = tc_hk_running

    report_source = 0x0015
    uart.send(build_pus15_report_request_tc(source_id=report_source))

    def find_report(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_STORAGE
                and t.secondary.service_subtype == PUS_15_SUBTYPE_STORE_REPORT
                and t.secondary.destination_id == report_source
            ),
            None,
        )

    tms, report = _collect(uart, find_report, timeout=60.0)
    _assert_boot_first(tms)

    assert report.primary.type == PACKET_TYPE_TM
    assert report.primary.apid == FSW_APID
    assert report.secondary.pus_version == PUS_VERSION_C
    assert len(report.source_data) == PUS15_STORE_REPORT_SOURCE_SIZE
    assert report.crc_ok

    store = decode_pus15_store_report(report)
    assert store.enabled  # storage starts enabled at boot
    assert 1 <= store.count <= PKTSTORE_CAPACITY
    assert store.oldest_time <= store.newest_time


def test_pus15_downlink_retrieves_stored_tm(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-11 headline: every TM the FSW emits is tapped into
    the on-board packet store, and a PUS-15[9] by-time-window downlink
    replays it verbatim. Live telemetry draws a strictly monotonic
    per-APID sequence count; a retrieval re-emits stored packets with
    their original (older, lower) counts. A sequence-count regression on
    the stream is therefore unambiguous proof of a replay — and it does
    not depend on any one stored packet surviving circular overwrite
    before the retrieval is armed."""
    _, uart = tc_hk_running

    # Let the store capture the boot event plus a housekeeping report,
    # then arm a retrieval over the whole representable time range.
    _collect(
        uart,
        lambda ts: ts if any(_is_hk(t) for t in ts) else None,
        timeout=60.0,
    )
    uart.send(build_pus15_downlink_tc(from_time=0, to_time=0xFFFFFFFF))

    def find_replay(tms: list[DecodedTm]):
        running_max = -1
        for t in tms:
            if t.primary.seq_count < running_max:
                return t  # a sequence regression — a replayed packet
            running_max = max(running_max, t.primary.seq_count)
        return None

    _, replay = _collect(uart, find_replay, timeout=60.0)
    # The replayed packet is one the FSW emitted earlier, re-sent intact
    # end to end: a TM Space Packet on this AP with a valid CRC.
    assert replay.primary.type == PACKET_TYPE_TM
    assert replay.primary.apid == FSW_APID
    assert replay.crc_ok


def test_pus15_disable_then_reenable_round_trips_storage_state(  # noqa: F811
    tc_hk_running,
) -> None:
    """PUS-15[2] suspends packet capture and PUS-15[1] resumes it; each
    state change is observable in a subsequent [15,13] store report. The
    two report requests carry distinct source ids so the disabled and
    re-enabled reports are told apart on the running stream — the FSW
    processes the four telecommands strictly in arrival order, so the
    first report is built while storage is off and the second while it
    is back on."""
    _, uart = tc_hk_running

    disabled_source = 0x00D5
    enabled_source = 0x00E5
    uart.send(build_pus15_disable_storage_tc())
    uart.send(build_pus15_report_request_tc(source_id=disabled_source))
    uart.send(build_pus15_enable_storage_tc())
    uart.send(build_pus15_report_request_tc(source_id=enabled_source))

    def find_for(source_id: int):
        def predicate(tms: list[DecodedTm]):
            return next(
                (
                    t
                    for t in tms
                    if t.secondary.service_type == PUS_SERVICE_STORAGE
                    and t.secondary.service_subtype == PUS_15_SUBTYPE_STORE_REPORT
                    and t.secondary.destination_id == source_id
                ),
                None,
            )

        return predicate

    _, disabled_report = _collect(uart, find_for(disabled_source), timeout=60.0)
    _, enabled_report = _collect(uart, find_for(enabled_source), timeout=60.0)

    assert disabled_report.crc_ok and enabled_report.crc_ok
    assert not decode_pus15_store_report(disabled_report).enabled
    assert decode_pus15_store_report(enabled_report).enabled


# The byte ramp the tc_uart sample downlinks when the PUS-13 large-data
# demo is enabled (CONFIG_FSW_LARGEDATA_DEMO, on for this build):
# LARGEDATA_DEMO_UNIT_LEN bytes, byte i == i & 0xFF. Kept here so the
# expected payload is legible next to the assertion.
LARGEDATA_DEMO_UNIT_LEN = 200


def test_pus13_large_data_downlink_reassembles(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-12 headline: the FSW downlinks a data unit too
    large for one Space Packet as an ordered sequence of PUS-13 part
    packets, and the ground reassembles it from the part header. The
    housekeeping build runs one demo transfer at boot; collecting its
    [13,1] / [13,2] / [13,3] parts and concatenating their payloads in
    part-number order reproduces the sample's byte ramp exactly."""
    _, uart = tc_hk_running

    def all_parts(tms: list[DecodedTm]):
        parts = [t for t in tms if t.secondary.service_type == PUS_SERVICE_LARGE_DATA]
        if not parts:
            return None
        # Every part declares the transfer's total; wait for them all.
        total = decode_pus13_part(parts[0]).total_parts
        return parts if len(parts) >= total else None

    tms, parts = _collect(uart, all_parts, timeout=60.0)
    _assert_boot_first(tms)

    decoded = [decode_pus13_part(p) for p in parts]
    by_number = {d.part_number: p for d, p in zip(decoded, parts)}
    txn = decoded[0].transaction_id
    total = decoded[0].total_parts

    # One transaction, one consistent part count, gapless numbering.
    assert all(d.transaction_id == txn for d in decoded)
    assert all(d.total_parts == total for d in decoded)
    assert sorted(by_number) == list(range(total))
    assert all(p.crc_ok for p in parts)
    assert all(p.primary.apid == FSW_APID for p in parts)

    # The first part is [13,1], the last is [13,3].
    assert by_number[0].secondary.service_subtype == PUS_13_SUBTYPE_FIRST_PART
    assert by_number[total - 1].secondary.service_subtype == PUS_13_SUBTYPE_LAST_PART

    # Reassembled in part-number order, the payloads are the sample's ramp.
    assert reassemble_pus13(parts) == bytes(
        i & 0xFF for i in range(LARGEDATA_DEMO_UNIT_LEN)
    )


# Operating-mode IDs the tc_uart sample's mode-manager demo defines
# (CONFIG_FSW_MODE_DEMO, on for this build). Kept here so the expected
# boot-time transition is legible next to the assertion.
FSW_MODE_BOOT = 0
FSW_MODE_NOMINAL = 1


def test_mode_manager_emits_a_mode_changed_event(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-13 headline: the operating-mode manager performs a
    boot-time BOOT -> NOMINAL transition and announces it as a
    spontaneous PUS-5 MODE_CHANGED informative event, carrying the
    previous and new mode IDs as its 2-byte auxiliary data."""
    _, uart = tc_hk_running

    def find_mode_changed(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
                and t.event_id == PUS5_EVT_MODE_CHANGED
            ),
            None,
        )

    tms, event = _collect(uart, find_mode_changed, timeout=60.0)
    _assert_boot_first(tms)

    assert event.primary.type == PACKET_TYPE_TM
    assert event.primary.apid == FSW_APID
    assert event.secondary.service_subtype == PUS_5_SUBTYPE_INFO
    assert event.secondary.destination_id == 0  # spontaneous, no triggering TC
    assert event.event_aux == bytes([FSW_MODE_BOOT, FSW_MODE_NOMINAL])
    assert event.crc_ok


# The safe mode and the rejected-TC confirmation threshold the tc_uart
# sample's FDIR-recovery demo uses — must match the tc-hk Kconfig
# overrides (CONFIG_FSW_FDIR_TC_REJECTED_THRESHOLD). FDIR_ANOM_TC_REJECTED
# is the anomaly type in the FDIR_RECOVERY aux (lib/fdir/fdir.h).
FSW_MODE_SAFE = 2
FDIR_TC_REJECTED_THRESHOLD = 3
FDIR_ANOM_TC_REJECTED = 0


def test_fdir_confirms_repeated_rejections_and_safes_the_spacecraft(  # noqa: F811
    tc_hk_running,
) -> None:
    """The slice fsw-14 headline: a single malformed telecommand is a
    transient and is not recovered; FDIR_TC_REJECTED_THRESHOLD of them
    cross the confirmation threshold, and FDIR autonomously emits a
    high-severity PUS-5 FDIR_RECOVERY event and commands the mode
    manager to SAFE — observed as a NOMINAL -> SAFE mode change on the
    live downlink."""
    _, uart = tc_hk_running

    # Send threshold-many CRC-corrupted telecommands, each with a
    # distinct source id so the rejections are individually genuine.
    for i in range(FDIR_TC_REJECTED_THRESHOLD):
        bad = bytearray(build_pus17_are_you_alive_tc(source_id=0x6100 + i))
        bad[-1] ^= 0xFF  # corrupt the CRC
        uart.send(bytes(bad))

    def find_recovery(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
                and t.event_id == PUS5_EVT_FDIR_RECOVERY
            ),
            None,
        )

    tms, recovery = _collect(uart, find_recovery, timeout=60.0)
    _assert_boot_first(tms)

    # The FDIR_RECOVERY event: high severity, spontaneous; aux is the
    # anomaly type, the commanded safe mode, and the occurrence count.
    assert recovery.secondary.service_subtype == PUS_5_SUBTYPE_HIGH
    assert recovery.secondary.destination_id == 0
    assert recovery.event_aux[0] == FDIR_ANOM_TC_REJECTED
    assert recovery.event_aux[1] == FSW_MODE_SAFE
    assert int.from_bytes(recovery.event_aux[2:4], "big") == FDIR_TC_REJECTED_THRESHOLD
    assert recovery.crc_ok

    # The autonomous transition: a MODE_CHANGED from NOMINAL to SAFE,
    # after the recovery event in sequence-count order (cause then
    # effect on the shared per-APID space).
    def find_safe_transition(tms: list[DecodedTm]):
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
                and t.event_id == PUS5_EVT_MODE_CHANGED
                and t.event_aux == bytes([FSW_MODE_NOMINAL, FSW_MODE_SAFE])
            ),
            None,
        )

    _, transition = _collect(uart, find_safe_transition, timeout=60.0)
    assert transition.primary.seq_count > recovery.primary.seq_count
    assert transition.crc_ok


# --- slice fsw-15: PUS-3 housekeeping structure management -------------

# Mission Structure ID for the ground-created housekeeping structure.
DYNAMIC_SID = 0x0100

# Firmware-identity values the tc_uart sample sets on its two read-only
# datapool parameters (samples/tc_uart/src/main.c). A ground-created
# housekeeping structure samples them, so the dynamic [3,25] report
# carries exactly these.
FW_VERSION = 0x0001
FW_BUILD_ID = 0x20260522


def _completion_success(tms: list[DecodedTm], request_id: bytes):
    """The PUS-1[7] completion-success report for ``request_id``, if
    one is present in ``tms``."""
    return next(
        (
            t
            for t in tms
            if t.secondary.service_type == PUS_SERVICE_VERIFICATION
            and t.secondary.service_subtype == PUS_1_SUBTYPE_COMPLETION_SUCCESS
            and t.request_id == request_id
        ),
        None,
    )


def test_dynamic_housekeeping_structure_lifecycle(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-15 headline: ground creates a housekeeping
    structure over two read-only datapool parameters, enables it, the
    FSW spontaneously emits a datapool-backed dynamic PUS-3[25] report
    for it, and ground then disables and deletes it — the full
    structure-management round trip on the live downlink."""
    _, uart = tc_hk_running

    # Create a structure sampling the sample's two firmware-identity
    # parameters, reported every HK_PERIOD_SEC seconds. Distinct
    # sequence counts keep each management TC's request id unique.
    create = build_pus3_create_structure_tc(
        sid=DYNAMIC_SID,
        param_ids=[DP_PARAM_FW_VERSION, DP_PARAM_FW_BUILD_ID],
        interval_sec=HK_PERIOD_SEC,
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=0x0510,
        seq_count=0x0020,
    )
    uart.send(create)
    _collect(uart, lambda ts: _completion_success(ts, create[:4]), timeout=60.0)

    # Enable it — a create leaves the structure disabled (flight-safe).
    enable = build_pus3_enable_structure_tc(
        sid=DYNAMIC_SID,
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=0x0511,
        seq_count=0x0021,
    )
    uart.send(enable)
    _collect(uart, lambda ts: _completion_success(ts, enable[:4]), timeout=60.0)

    # The enabled structure is now emitted spontaneously as a dynamic
    # [3,25] — Structure ID 0x0100, destination 0, distinct from the
    # FRAMEWORK_DIAG (SID 0x0001) report.
    def find_dynamic(tms: list[DecodedTm]):
        return next((t for t in tms if _is_hk(t) and t.pus3_sid == DYNAMIC_SID), None)

    _, dyn = _collect(uart, find_dynamic, timeout=60.0)
    assert dyn.secondary.destination_id == 0  # spontaneous, no triggering TC
    sid, values = decode_pus3_dynamic_report(dyn, [DP_TYPE_U16, DP_TYPE_U32])
    assert sid == DYNAMIC_SID
    assert len(dyn.source_data) == 8  # SID(2) + u16(2) + u32(4)
    assert int.from_bytes(values[0], "big") == FW_VERSION
    assert int.from_bytes(values[1], "big") == FW_BUILD_ID
    assert dyn.crc_ok

    # Disable, then delete — both complete successfully.
    disable = build_pus3_disable_structure_tc(
        sid=DYNAMIC_SID,
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=0x0512,
        seq_count=0x0022,
    )
    uart.send(disable)
    _collect(uart, lambda ts: _completion_success(ts, disable[:4]), timeout=60.0)

    delete = build_pus3_delete_structure_tc(
        sid=DYNAMIC_SID,
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=0x0513,
        seq_count=0x0023,
    )
    uart.send(delete)
    _, completion = _collect(
        uart, lambda ts: _completion_success(ts, delete[:4]), timeout=60.0
    )
    assert completion.failure_code is None  # completion success


# --- slice fsw-16: non-volatile parameter persistence ------------------


def test_datapool_value_survives_a_warm_reset(tc_hk_running) -> None:  # noqa: F811
    """The slice fsw-16 headline: a parameter tuned via PUS-20[3]
    auto-saves to flash, the emulated MCU warm-resets, and on the
    next boot the persisted value is reported instead of the Kconfig
    default. End-to-end proof that on-board state survives a reboot
    through the lib/nvstore/ A/B-redundant flash image."""
    mon, uart = tc_hk_running

    # 1. Tune the HK period to a sentinel value distinct from the
    #    Kconfig default (2) and from every other test's value.
    sentinel = 1234
    set_tc = build_pus20_set_request_tc(
        params=[(DP_PARAM_HK_PERIOD_SEC, struct.pack(">I", sentinel))],
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION,
        source_id=0x0F00,
        seq_count=0x10,
    )
    uart.send(set_tc)
    _, set_completion = _collect(
        uart, lambda ts: _completion_success(ts, set_tc[:4]), timeout=30.0
    )
    assert set_completion.failure_code is None

    # 2. The save tick runs at the top of every main-loop iteration —
    #    so by the time a *subsequent* TC's completion lands, the FSW
    #    has done at least one more iteration and the save has fired.
    #    The cheapest such TC is a PUS-17 ping with acceptance ack.
    ping_tc = build_pus17_are_you_alive_tc(
        ack_flags=ACK_ACCEPTANCE,
        source_id=0x0F01,
        seq_count=0x11,
    )
    uart.send(ping_tc)
    _collect(
        uart,
        lambda ts: next(
            (t for t in ts if t.secondary.service_type == PUS_SERVICE_TEST), None
        ),
        timeout=30.0,
    )

    # 3. Warm-reset the emulated MCU. Renode's `machine Reset` resets
    #    the CPU and peripherals but DOES NOT touch the flash banks —
    #    the storage_partition keeps the A/B image across the reset.
    mon.cmd("machine Reset")

    # 4. Ask the FSW to report the HK-period parameter on the
    #    post-reset boot. Used a different source_id so the report is
    #    uniquely identifiable in the cumulative UART buffer.
    report_tc = build_pus20_report_request_tc(
        param_ids=[DP_PARAM_HK_PERIOD_SEC],
        ack_flags=ACK_ACCEPTANCE,
        source_id=0x0F02,
        seq_count=0x12,
    )
    uart.send(report_tc)

    def find_post_reset_report(tms: list[DecodedTm]):
        # Two boot events in the cumulative stream means we have seen
        # a reset cycle; the report from our source_id is the
        # post-reset response we care about.
        boot_count = sum(
            1
            for t in tms
            if t.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
            and t.event_id == PUS5_EVT_FSW_BOOT
        )
        if boot_count < 2:
            return None
        return next(
            (
                t
                for t in tms
                if t.secondary.service_type == PUS_SERVICE_ONBOARD_PARAMETER
                and t.secondary.service_subtype == PUS_20_SUBTYPE_VALUE_REPORT
                and t.secondary.destination_id == 0x0F02
            ),
            None,
        )

    _, report = _collect(uart, find_post_reset_report, timeout=60.0)
    values = decode_pus20_report(report, {DP_PARAM_HK_PERIOD_SEC: DP_TYPE_U32})
    # The reported value is the sentinel set BEFORE the reset, NOT the
    # Kconfig default (2) — the persisted image was loaded and the
    # datapool's parameter was restored.
    assert int.from_bytes(values[DP_PARAM_HK_PERIOD_SEC], "big") == sentinel
    assert report.crc_ok
