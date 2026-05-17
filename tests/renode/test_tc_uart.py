# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""fsw-8 TC verification + PUS-5 event reporting round-trip over UART.

Boots ``samples/tc_uart/zephyr.elf`` on the Renode-bundled
``nucleo_h753zi`` platform. On reset the FSW emits one spontaneous
PUS-5[1] "FSW boot" informative event (slice fsw-6) — the first TM it
produces — and then services inbound PUS-17[1] TCs, emitting the
PUS-1 / PUS-17 verification stream pinned in ``docs/wire/pus-1.md``
and ``docs/wire/pus-17.md``. The boot event consumes the first
per-APID sequence count, so the first TC response starts at count 1.

Slice fsw-8 adds the FDIR path: a rejected TC additionally produces a
spontaneous PUS-5[2] ``TC_REJECTED`` anomaly, drained from the FDIR
FIFO *after* that TC's PUS-1 verification — and emitted even when the
TC requested no verification (the anomaly is ungated by ack flags;
``docs/wire/pus-5.md``).

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
    PUS_5_SUBTYPE_INFO,
    PUS_5_SUBTYPE_LOW,
    PUS_17_SUBTYPE_ARE_YOU_ALIVE_TC,
    PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM,
    PUS_SERVICE_EVENT_REPORTING,
    PUS_SERVICE_TEST,
    PUS_SERVICE_VERIFICATION,
    PUS_VERSION_C,
    PUS1_FAILURE_TM_PACKET_SIZE,
    PUS1_SUCCESS_TM_PACKET_SIZE,
    PUS5_BARE_TM_PACKET_SIZE,
    PUS5_EVT_FSW_BOOT,
    PUS5_EVT_TC_REJECTED,
    PUS17_TM_PACKET_SIZE,
    SEQ_FLAGS_UNSEGMENTED,
    DecodedTm,
    build_pus17_are_you_alive_tc,
    split_packets,
)

# PUS-5[2] TC_REJECTED carries 3 aux bytes (fc, service type, subtype).
PUS5_TC_REJECTED_PACKET_SIZE = PUS5_BARE_TM_PACKET_SIZE + 3
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
    that prefix. The FSW emits a fixed number of bytes per stimulus, so
    once we have them we are done — extra bytes would be a regression."""
    pattern = re.compile(rb"(?s).{%d}" % nbytes)
    return uart.expect(pattern, timeout=timeout)[:nbytes]


def _assert_boot_event(pkt: bytes) -> None:
    """Assert ``pkt`` is the slice-fsw-6 spontaneous PUS-5[1] boot
    event: the first TM the FSW emits, on APID 0x100, sequence 0, no
    auxiliary data, no triggering TC."""
    tm = DecodedTm.decode(pkt)
    assert tm.primary.type == PACKET_TYPE_TM
    assert tm.primary.apid == FSW_APID
    assert tm.primary.seq_flags == SEQ_FLAGS_UNSEGMENTED
    assert tm.primary.seq_count == 0
    assert tm.secondary.pus_version == PUS_VERSION_C
    assert tm.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
    assert tm.secondary.service_subtype == PUS_5_SUBTYPE_INFO
    assert tm.secondary.destination_id == 0
    assert tm.event_id == PUS5_EVT_FSW_BOOT
    assert tm.event_aux == b""
    assert tm.crc_ok


def _read_after_boot(uart, nbytes: int, *, timeout: float = 30.0) -> tuple[bytes, bytes]:
    """Read the leading 20-byte PUS-5[1] boot event plus ``nbytes`` of
    test-triggered TM that follows it. Returns
    ``(boot_packet, raw_after_boot)``. The UART buffer is cumulative
    and never drained, so every test sees the boot event first."""
    raw = _read_exact(uart, PUS5_BARE_TM_PACKET_SIZE + nbytes, timeout=timeout)
    return raw[:PUS5_BARE_TM_PACKET_SIZE], raw[PUS5_BARE_TM_PACKET_SIZE:]


def test_boot_emits_pus5_info_event(tc_running) -> None:  # noqa: F811
    """On reset, before any TC, the FSW emits exactly one spontaneous
    PUS-5[1] FSW_BOOT informative event — the slice fsw-6
    demonstration of asynchronous, non-TC-triggered TM."""
    _, uart = tc_running

    raw = _read_exact(uart, PUS5_BARE_TM_PACKET_SIZE, timeout=30.0)
    pkts = split_packets(raw)
    assert len(pkts) == 1, raw.hex()
    _assert_boot_event(pkts[0])
    # First PUS-5[1] → its per-(service,subtype) message counter is 0.
    assert DecodedTm.decode(pkts[0]).secondary.msg_counter == 0


def test_acceptance_and_completion_round_trip(tc_running) -> None:  # noqa: F811
    """A PUS-17[1] TC requesting acceptance + completion verification
    yields three back-to-back packets: PUS-1[1], PUS-17[2], PUS-1[7].
    After the boot event the shared per-APID count is 1, so the burst
    is seq 1,2,3."""
    _, uart = tc_running

    source_id = 0xCAFE
    tc = build_pus17_are_you_alive_tc(
        ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=source_id, seq_count=7
    )
    uart.send(tc)

    total = 2 * PUS1_SUCCESS_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE
    try:
        boot, raw = _read_after_boot(uart, total, timeout=30.0)
    except TimeoutError as e:
        pytest.fail(
            f"did not receive boot event + full verification burst: {e}\n"
            f"sent TC ({len(tc)} bytes): {tc.hex()}\n"
            f"uart buffer ({len(uart.buffer())} bytes): {uart.buffer().hex()}"
        )

    _assert_boot_event(boot)

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

    # One shared, strictly-monotonic per-APID sequence count, rebased
    # past the boot event (which consumed count 0).
    assert [t.primary.seq_count for t in tms] == [1, 2, 3], raw.hex()
    for t in tms:
        assert t.crc_ok, raw.hex()


def test_no_ack_flags_is_back_compatible(tc_running) -> None:  # noqa: F811
    """With no ack flags the router still routes to PUS-17 and emits
    exactly one PUS-17[2] — the pre-fsw-5 behaviour, unchanged. It now
    follows the boot event, at shared seq count 1."""
    _, uart = tc_running

    uart.send(build_pus17_are_you_alive_tc(ack_flags=0, source_id=0x0042))
    boot, raw = _read_after_boot(uart, PUS17_TM_PACKET_SIZE, timeout=30.0)

    _assert_boot_event(boot)

    pkts = split_packets(raw)
    assert len(pkts) == 1, raw.hex()
    tm = DecodedTm.decode(pkts[0])
    assert tm.secondary.service_type == PUS_SERVICE_TEST
    assert tm.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert tm.secondary.destination_id == 0x0042
    assert tm.primary.seq_count == 1  # boot event consumed count 0
    assert tm.crc_ok
    assert (
        len(uart.buffer()) == PUS5_BARE_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE
    ), uart.buffer().hex()


def test_corrupted_ack_tc_yields_pus1_failure_then_fdir_anomaly(  # noqa: F811
    tc_running,
) -> None:
    """A CRC-corrupted TC requesting acceptance verification produces
    its PUS-1[2] (CRC_FAILURE) and, from slice fsw-8, a spontaneous
    PUS-5[2] ``TC_REJECTED`` anomaly drained right after it — the
    PUS-1 ack precedes the anomaly on the wire. The TC is still neither
    routed nor completed. A follow-up clean no-ack TC proves the FSW
    is live and that the rejected TC leaked nothing else."""
    _, uart = tc_running

    bad = bytearray(
        build_pus17_are_you_alive_tc(
            ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=0xAA55
        )
    )
    bad[-1] ^= 0xFF  # flip the low CRC byte
    uart.send(bytes(bad))

    # Rejected TC → PUS-1[2] then PUS-5[2]. Then a clean no-ack TC → PUS-17[2].
    good_source = 0x1234
    uart.send(build_pus17_are_you_alive_tc(ack_flags=0, source_id=good_source))

    total = (
        PUS1_FAILURE_TM_PACKET_SIZE
        + PUS5_TC_REJECTED_PACKET_SIZE
        + PUS17_TM_PACKET_SIZE
    )
    boot, raw = _read_after_boot(uart, total, timeout=30.0)
    _assert_boot_event(boot)

    pkts = split_packets(raw)
    assert len(pkts) == 3, raw.hex()
    fail, anomaly, live = (DecodedTm.decode(p) for p in pkts)

    # 1. PUS-1[2] acceptance failure (the solicited verification).
    assert fail.secondary.service_type == PUS_SERVICE_VERIFICATION
    assert fail.secondary.service_subtype == PUS_1_SUBTYPE_ACCEPTANCE_FAILURE
    assert fail.failure_code == FC_CRC_FAILURE
    assert fail.secondary.destination_id == 0xAA55
    assert fail.primary.seq_count == 1  # boot event consumed count 0

    # 2. PUS-5[2] TC_REJECTED FDIR anomaly, strictly after the ack.
    assert anomaly.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
    assert anomaly.secondary.service_subtype == PUS_5_SUBTYPE_LOW
    assert anomaly.secondary.destination_id == 0  # spontaneous
    assert anomaly.event_id == PUS5_EVT_TC_REJECTED
    assert anomaly.event_aux == bytes(
        [FC_CRC_FAILURE, PUS_SERVICE_TEST, PUS_17_SUBTYPE_ARE_YOU_ALIVE_TC]
    )
    assert anomaly.primary.seq_count == 2

    # 3. The clean follow-up TC still serviced.
    assert live.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert live.secondary.destination_id == good_source
    assert live.primary.seq_count == 3

    for t in (fail, anomaly, live):
        assert t.crc_ok, raw.hex()
    # Nothing else followed the boot event + three expected packets.
    assert (
        len(uart.buffer()) == PUS5_BARE_TM_PACKET_SIZE + total
    ), uart.buffer().hex()


def test_no_ack_rejection_is_pus1_silent_but_raises_anomaly(  # noqa: F811
    tc_running,
) -> None:
    """The key fsw-8 invariant on real hardware: a CRC-corrupted TC
    with *no* ack flags stays PUS-1-silent (rule 3) yet still emits the
    spontaneous PUS-5[2] ``TC_REJECTED`` anomaly. A clean no-ack TC
    after it proves liveness and that no PUS-1 leaked."""
    _, uart = tc_running

    bad = bytearray(build_pus17_are_you_alive_tc(ack_flags=0, source_id=0x9001))
    bad[-1] ^= 0xFF
    uart.send(bytes(bad))

    good_source = 0x4242
    uart.send(build_pus17_are_you_alive_tc(ack_flags=0, source_id=good_source))

    total = PUS5_TC_REJECTED_PACKET_SIZE + PUS17_TM_PACKET_SIZE
    boot, raw = _read_after_boot(uart, total, timeout=30.0)
    _assert_boot_event(boot)

    pkts = split_packets(raw)
    assert len(pkts) == 2, raw.hex()
    anomaly, live = (DecodedTm.decode(p) for p in pkts)

    # No PUS-1 anywhere — the corrupted no-ack TC requested no
    # verification, so rule 3 keeps the FSW PUS-1-silent.
    assert all(
        t.secondary.service_type != PUS_SERVICE_VERIFICATION
        for t in (anomaly, live)
    ), raw.hex()

    assert anomaly.secondary.service_type == PUS_SERVICE_EVENT_REPORTING
    assert anomaly.secondary.service_subtype == PUS_5_SUBTYPE_LOW
    assert anomaly.event_id == PUS5_EVT_TC_REJECTED
    assert anomaly.event_aux[0] == FC_CRC_FAILURE
    assert anomaly.primary.seq_count == 1  # boot consumed 0; no PUS-1 emitted

    assert live.secondary.service_subtype == PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM
    assert live.secondary.destination_id == good_source
    assert live.primary.seq_count == 2
    assert anomaly.crc_ok and live.crc_ok, raw.hex()


def test_sequence_count_coherent_across_consecutive_tcs(tc_running) -> None:  # noqa: F811
    """Two acceptance+completion TCs back to back: the boot event plus
    six packets sharing one strictly-increasing per-APID sequence
    count (0 for the boot event, then 1..6)."""
    _, uart = tc_running

    for i in range(2):
        uart.send(
            build_pus17_are_you_alive_tc(
                ack_flags=ACK_ACCEPTANCE | ACK_COMPLETION, source_id=0xBEEF, seq_count=i
            )
        )

    total = 2 * (2 * PUS1_SUCCESS_TM_PACKET_SIZE + PUS17_TM_PACKET_SIZE)
    boot, raw = _read_after_boot(uart, total, timeout=30.0)
    _assert_boot_event(boot)

    pkts = split_packets(raw)
    assert len(pkts) == 6, raw.hex()

    tms = [DecodedTm.decode(p) for p in pkts]
    assert [t.primary.seq_count for t in tms] == [1, 2, 3, 4, 5, 6], raw.hex()
    for t in tms:
        assert t.crc_ok, raw.hex()
