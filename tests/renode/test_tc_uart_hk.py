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

import time

import pytest

from _pus import (
    ACK_ACCEPTANCE,
    ACK_COMPLETION,
    FSW_APID,
    PACKET_TYPE_TM,
    PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS,
    PUS_1_SUBTYPE_COMPLETION_SUCCESS,
    PUS_3_SUBTYPE_HK_PARAM_REPORT,
    PUS_5_SUBTYPE_INFO,
    PUS_SERVICE_EVENT_REPORTING,
    PUS_SERVICE_HOUSEKEEPING,
    PUS_SERVICE_VERIFICATION,
    PUS_VERSION_C,
    PUS3_HK_SOURCE_DATA_SIZE,
    PUS3_SID_FRAMEWORK_DIAG,
    SEQ_FLAGS_UNSEGMENTED,
    DecodedTm,
    build_pus3_oneshot_poll_tc,
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
    PUS-3[25] framework housekeeping report one period after boot, at
    shared sequence count 1 (the boot event consumed 0)."""
    _, uart = tc_hk_running

    tms, hk = _collect(
        uart, lambda ts: next((t for t in ts if _is_hk(t)), None), timeout=60.0
    )
    _assert_boot_first(tms)

    assert hk.primary.type == PACKET_TYPE_TM
    assert hk.primary.apid == FSW_APID
    assert hk.primary.seq_flags == SEQ_FLAGS_UNSEGMENTED
    assert hk.primary.seq_count == 1  # boot consumed 0, no TC in between
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
    sequence is strictly monotonic across the triplet."""
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
    tms, (acc, hk, comp) = _collect(uart, find_triplet, timeout=60.0)

    # The poll report carries the full 47-byte packet and echoes the
    # triggering source ID — that is what set it apart from the
    # spontaneous reports (destination_id == 0) in the same stream.
    assert len(split_packets(uart.buffer())) >= 3
    assert hk.pus3_sid == PUS3_SID_FRAMEWORK_DIAG
    assert len(hk.source_data) == PUS3_HK_SOURCE_DATA_SIZE
    assert acc.primary.seq_count + 1 == hk.primary.seq_count
    assert hk.primary.seq_count + 1 == comp.primary.seq_count
    assert acc.crc_ok and hk.crc_ok and comp.crc_ok

    # Spontaneous reports in the same stream are addressed to nobody.
    spontaneous = [t for t in tms if _is_hk(t) and t.secondary.destination_id == 0]
    assert spontaneous, "expected at least one periodic HK report too"


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
