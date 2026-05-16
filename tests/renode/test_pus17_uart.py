# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""fsw-4 PUS-17 connection-test round-trip over UART.

Boots ``samples/pus17_uart/zephyr.elf`` on the Renode-bundled
``nucleo_h753zi`` platform, sends a PUS-17[1] TC on USART3, and
asserts that the FSW emits a well-formed PUS-17[2] TM back on the
same UART within a bounded timeout. Wire format is pinned in
``docs/wire/pus-17.md``.

The ground-side encoder/decoder lives in ``_pus.py`` and is
deliberately independent of the C codec under ``lib/pus/`` — the
two implementations meet only on the wire.
"""

from __future__ import annotations

import pytest

from _pus import (
    FSW4_APID,
    PACKET_TYPE_TM,
    PUS17_TM_PACKET_SIZE,
    PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM,
    PUS_SERVICE_TEST,
    PUS_VERSION_C,
    SEQ_FLAGS_UNSEGMENTED,
    Pus17Tm,
    build_pus17_are_you_alive_tc,
)
from conftest import _PUS17_ELF, _RENODE_BIN, pus17_running  # noqa: F401


pytestmark = [
    pytest.mark.skipif(
        _RENODE_BIN is None,
        reason="Renode not installed (set RENODE_BIN, put `renode` on PATH, "
        "or install Renode.app on macOS)",
    ),
    pytest.mark.skipif(
        _PUS17_ELF is None,
        reason="PUS-17 sample ELF not built. Run `west build -b nucleo_h753zi "
        "fsw-core/samples/pus17_uart --pristine=always` from the workspace "
        "root, or set FSW_CORE_PUS17_ELF to a prebuilt ELF.",
    ),
]


def _wait_for_tm(uart, *, timeout: float = 30.0) -> bytes:
    """Block until ``PUS17_TM_PACKET_SIZE`` bytes have accumulated on
    the UART capture, then return exactly that prefix. The FSW emits
    a single packet per accepted TC, so once we have the full length
    we are done — extra bytes would be a regression."""
    import re

    pattern = re.compile(rb"(?s).{%d}" % PUS17_TM_PACKET_SIZE)
    matched = uart.expect(pattern, timeout=timeout)
    return matched[:PUS17_TM_PACKET_SIZE]


def test_are_you_alive_round_trip(pus17_running) -> None:  # noqa: F811
    """A single PUS-17[1] TC on USART3 produces a single, well-formed
    PUS-17[2] TM on the same UART within 30 s.

    Verifies the full wire-format contract pinned in
    docs/wire/pus-17.md: primary header values, secondary header
    semantics (echo of source_id into destination_id), CRC integrity,
    and exact packet length."""
    mon, uart = pus17_running

    source_id = 0xCAFE
    tc = build_pus17_are_you_alive_tc(source_id=source_id, seq_count=7)
    uart.send(tc)

    try:
        raw = _wait_for_tm(uart, timeout=30.0)
    except TimeoutError as e:
        pytest.fail(
            f"no PUS-17[2] TM seen within timeout: {e}\n"
            f"sent TC ({len(tc)} bytes): {tc.hex()}\n"
            f"uart buffer so far ({len(uart.buffer())} bytes): "
            f"{uart.buffer().hex()}"
        )

    tm = Pus17Tm.decode(raw)

    # Primary header — pinned values.
    assert tm.primary.version == 0, raw.hex()
    assert tm.primary.type == PACKET_TYPE_TM
    assert tm.primary.sec_hdr_flag == 1
    assert tm.primary.apid == FSW4_APID
    assert tm.primary.seq_flags == SEQ_FLAGS_UNSEGMENTED
    assert tm.primary.seq_count == 0  # First TM the FSW emits.

    # Secondary header — PUS-17[2] for the source we addressed.
    assert tm.secondary.pus_version == PUS_VERSION_C
    assert tm.secondary.service_type == PUS_SERVICE_TEST
    assert tm.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert tm.secondary.msg_counter == 0
    assert tm.secondary.destination_id == source_id

    # CRC over the full packet checks out.
    assert tm.crc_ok, raw.hex()


def test_counters_advance_on_consecutive_tcs(pus17_running) -> None:  # noqa: F811
    """Three TCs in a row produce three TMs with monotonically
    increasing CCSDS sequence count and PUS-17 message counter — the
    FSW state machine commits ctx updates per accepted TC, as
    docs/wire/pus-17.md mandates."""
    _, uart = pus17_running

    for i in range(3):
        uart.send(build_pus17_are_you_alive_tc(source_id=0xBEEF, seq_count=i))

    # Three TMs back-to-back = 3 * PUS17_TM_PACKET_SIZE bytes.
    import re

    pattern = re.compile(
        rb"(?s).{%d}" % (3 * PUS17_TM_PACKET_SIZE),
    )
    try:
        raw = uart.expect(pattern, timeout=30.0)
    except TimeoutError as e:
        pytest.fail(
            f"did not receive 3 PUS-17[2] TMs within timeout: {e}\n"
            f"uart buffer ({len(uart.buffer())} bytes): "
            f"{uart.buffer().hex()}"
        )

    tms = [
        Pus17Tm.decode(raw[i * PUS17_TM_PACKET_SIZE : (i + 1) * PUS17_TM_PACKET_SIZE])
        for i in range(3)
    ]

    for i, tm in enumerate(tms):
        assert tm.primary.seq_count == i, (i, raw.hex())
        assert tm.secondary.msg_counter == i, (i, raw.hex())
        assert tm.secondary.destination_id == 0xBEEF
        assert tm.crc_ok, raw.hex()


def test_corrupted_tc_produces_no_tm(pus17_running) -> None:  # noqa: F811
    """A TC with a flipped CRC byte must be silently dropped — no TM
    leaves the FSW. Slice fsw-4 has no PUS-1 verification yet, so
    rejection surfaces as the absence of a response.

    We use a follow-up valid TC as the liveness probe: the FSW
    answers it, and its TM must be the *only* output on the UART —
    proving the rejected TC leaked zero bytes."""
    _, uart = pus17_running

    bad = bytearray(build_pus17_are_you_alive_tc(source_id=0xAA55, seq_count=0))
    bad[-1] ^= 0xFF
    uart.send(bytes(bad))

    good_source = 0x1234
    uart.send(build_pus17_are_you_alive_tc(source_id=good_source, seq_count=1))

    raw = _wait_for_tm(uart, timeout=30.0)
    tm = Pus17Tm.decode(raw)

    # The single TM on the wire is the response to the *valid* TC —
    # destination_id equals the valid TC's source. seq_count and
    # msg_counter both start at 0 because the FSW only commits state
    # on accepted TCs.
    assert tm.secondary.destination_id == good_source, raw.hex()
    assert tm.primary.seq_count == 0
    assert tm.secondary.msg_counter == 0
    assert tm.crc_ok

    # Nothing else followed.
    assert len(uart.buffer()) == PUS17_TM_PACKET_SIZE, uart.buffer().hex()
