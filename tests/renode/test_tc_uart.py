# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""fsw-5 TC reception + verification round-trip over UART.

Boots ``samples/tc_uart/zephyr.elf`` on the Renode-bundled
``nucleo_h753zi`` platform, sends a PUS-17[1] TC on USART3 with
various ack-flag combinations, and asserts the FSW emits the
PUS-1 / PUS-17 verification stream pinned in ``docs/wire/pus-1.md``
and ``docs/wire/pus-17.md`` within a bounded timeout.

The ground-side encoder/decoder lives in ``_pus.py`` and is
deliberately independent of the C codec under ``lib/pus/`` — the two
implementations meet only on the wire.
"""

from __future__ import annotations

import re

import pytest

from _pus import (
    ACK_ACCEPTANCE,
    ACK_COMPLETION,
    FC_CRC_FAILURE,
    FSW_APID,
    PACKET_TYPE_TM,
    PUS_1_SUBTYPE_ACCEPTANCE_FAILURE,
    PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS,
    PUS_1_SUBTYPE_COMPLETION_SUCCESS,
    PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM,
    PUS_SERVICE_TEST,
    PUS_SERVICE_VERIFICATION,
    PUS_VERSION_C,
    PUS1_FAILURE_TM_PACKET_SIZE,
    PUS1_SUCCESS_TM_PACKET_SIZE,
    PUS17_TM_PACKET_SIZE,
    SEQ_FLAGS_UNSEGMENTED,
    DecodedTm,
    build_pus17_are_you_alive_tc,
    split_packets,
)
from conftest import _RENODE_BIN, _TC_ELF, tc_running  # noqa: F401


pytestmark = [
    pytest.mark.skipif(
        _RENODE_BIN is None,
        reason="Renode not installed (set RENODE_BIN, put `renode` on PATH, "
        "or install Renode.app on macOS)",
    ),
    pytest.mark.skipif(
        _TC_ELF is None,
        reason="tc_uart sample ELF not built. Run `west build -b nucleo_h753zi "
        "fsw-core/samples/tc_uart --pristine=always` from the workspace "
        "root, or set FSW_CORE_TC_ELF to a prebuilt ELF.",
    ),
]


def _read_exact(uart, nbytes: int, *, timeout: float = 30.0) -> bytes:
    """Block until exactly ``nbytes`` have accumulated, then return
    that prefix. The FSW emits a fixed number of bytes per TC, so once
    we have them we are done — extra bytes would be a regression."""
    pattern = re.compile(rb"(?s).{%d}" % nbytes)
    return uart.expect(pattern, timeout=timeout)[:nbytes]


def test_acceptance_and_completion_round_trip(tc_running) -> None:  # noqa: F811
    """A PUS-17[1] TC requesting acceptance + completion verification
    yields three back-to-back packets: PUS-1[1], PUS-17[2], PUS-1[7],
    with a single strictly-monotonic per-APID sequence count."""
    _, uart = tc_running

    source_id = 0xCAFE
    tc = build_pus17_are_you_alive_tc(
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=source_id, seq_count=7
    )
    uart.send(tc)

    total = 2 * PUS1_SUCCESS_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE
    try:
        raw = _read_exact(uart, total, timeout=30.0)
    except TimeoutError as e:
        pytest.fail(
            f"did not receive the full verification burst: {e}\n"
            f"sent TC ({len(tc)} bytes): {tc.hex()}\n"
            f"uart buffer ({len(uart.buffer())} bytes): {uart.buffer().hex()}"
        )

    pkts = split_packets(raw)
    assert len(pkts) == 3, raw.hex()
    tms = [DecodedTm.decode(p) for p in pkts]

    # PUS-1[1] acceptance success.
    assert tms[0].primary.type == PACKET_TYPE_TM
    assert tms[0].primary.apid == FSW_APID
    assert tms[0].primary.seq_flags == SEQ_FLAGS_UNSEGMENTED
    assert tms[0].secondary.pus_version == PUS_VERSION_C
    assert tms[0].secondary.service_type == PUS_SERVICE_VERIFICATION
    assert tms[0].secondary.service_subtype == PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS
    assert tms[0].secondary.destination_id == source_id
    assert tms[0].request_id == tc[:4]
    assert tms[0].failure_code is None

    # PUS-17[2] are-you-alive report.
    assert tms[1].secondary.service_type == PUS_SERVICE_TEST
    assert tms[1].secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert tms[1].secondary.destination_id == source_id

    # PUS-1[7] completion success.
    assert tms[2].secondary.service_type == PUS_SERVICE_VERIFICATION
    assert tms[2].secondary.service_subtype == PUS_1_SUBTYPE_COMPLETION_SUCCESS
    assert tms[2].request_id == tc[:4]

    # One shared, strictly-monotonic per-APID sequence count.
    assert [t.primary.seq_count for t in tms] == [0, 1, 2], raw.hex()
    for t in tms:
        assert t.crc_ok, raw.hex()


def test_no_ack_flags_is_back_compatible(tc_running) -> None:  # noqa: F811
    """With no ack flags the router still routes to PUS-17 and emits
    exactly one PUS-17[2] — the pre-fsw-5 behaviour, unchanged."""
    _, uart = tc_running

    uart.send(build_pus17_are_you_alive_tc(ack_flags=0, source_id=0x0042))
    raw = _read_exact(uart, PUS17_TM_PACKET_SIZE, timeout=30.0)

    pkts = split_packets(raw)
    assert len(pkts) == 1, raw.hex()
    tm = DecodedTm.decode(pkts[0])
    assert tm.secondary.service_type == PUS_SERVICE_TEST
    assert tm.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert tm.secondary.destination_id == 0x0042
    assert tm.primary.seq_count == 0
    assert tm.crc_ok
    assert len(uart.buffer()) == PUS17_TM_PACKET_SIZE, uart.buffer().hex()


def test_corrupted_tc_yields_single_acceptance_failure(tc_running) -> None:  # noqa: F811
    """A CRC-corrupted TC requesting acceptance verification produces
    exactly one PUS-1[2] (CRC_FAILURE) and is neither routed nor
    completed. A follow-up valid TC proves the FSW is still live and
    that the rejected TC leaked nothing else."""
    _, uart = tc_running

    bad = bytearray(
        build_pus17_are_you_alive_tc(
            ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=0xAA55
        )
    )
    bad[-1] ^= 0xFF  # flip the low CRC byte
    uart.send(bytes(bad))

    # Rejected TC → one PUS-1[2]. Then a clean no-ack TC → one PUS-17[2].
    good_source = 0x1234
    uart.send(build_pus17_are_you_alive_tc(ack_flags=0, source_id=good_source))

    total = PUS1_FAILURE_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE
    raw = _read_exact(uart, total, timeout=30.0)
    pkts = split_packets(raw)
    assert len(pkts) == 2, raw.hex()

    fail = DecodedTm.decode(pkts[0])
    assert fail.secondary.service_type == PUS_SERVICE_VERIFICATION
    assert fail.secondary.service_subtype == PUS_1_SUBTYPE_ACCEPTANCE_FAILURE
    assert fail.failure_code == FC_CRC_FAILURE
    assert fail.secondary.destination_id == 0xAA55
    assert fail.primary.seq_count == 0
    assert fail.crc_ok

    live = DecodedTm.decode(pkts[1])
    assert live.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert live.secondary.destination_id == good_source
    assert live.primary.seq_count == 1  # shared counter advanced once

    # Nothing else followed the two expected packets.
    assert len(uart.buffer()) == total, uart.buffer().hex()


def test_sequence_count_coherent_across_consecutive_tcs(tc_running) -> None:  # noqa: F811
    """Two acceptance+completion TCs back to back: six packets sharing
    one strictly-increasing per-APID sequence count."""
    _, uart = tc_running

    for i in range(2):
        uart.send(
            build_pus17_are_you_alive_tc(
                ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=0xBEEF, seq_count=i
            )
        )

    total = 2 * (2 * PUS1_SUCCESS_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE)
    raw = _read_exact(uart, total, timeout=30.0)
    pkts = split_packets(raw)
    assert len(pkts) == 6, raw.hex()

    tms = [DecodedTm.decode(p) for p in pkts]
    assert [t.primary.seq_count for t in tms] == [0, 1, 2, 3, 4, 5], raw.hex()
    for t in tms:
        assert t.crc_ok, raw.hex()
