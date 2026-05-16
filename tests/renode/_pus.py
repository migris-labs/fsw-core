# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""Ground-side CCSDS Space Packet + PUS-C encoders / decoders.

Built independently from the C codec in ``lib/pus/`` so the on-board
implementation does not silently agree with itself. The two
implementations meet only on the wire, which is exactly where we
want them to be byte-identical.

Wire-format authority: ``docs/wire/pus-17.md``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


# ---------------------------------------------------------------------------
# CCSDS Space Packet primary header (6 bytes, big-endian).
# ---------------------------------------------------------------------------

CCSDS_PRIMARY_HEADER_SIZE = 6

PACKET_TYPE_TM = 0
PACKET_TYPE_TC = 1

SEQ_FLAGS_UNSEGMENTED = 0b11


@dataclass
class CcsdsPrimary:
    version: int = 0
    type: int = 0  # 0 = TM, 1 = TC
    sec_hdr_flag: int = 1
    apid: int = 0
    seq_flags: int = SEQ_FLAGS_UNSEGMENTED
    seq_count: int = 0
    data_length: int = 0

    def pack(self) -> bytes:
        word0 = (
            ((self.version & 0x7) << 13)
            | ((self.type & 0x1) << 12)
            | ((self.sec_hdr_flag & 0x1) << 11)
            | (self.apid & 0x7FF)
        )
        word1 = ((self.seq_flags & 0x3) << 14) | (self.seq_count & 0x3FFF)
        return struct.pack(">HHH", word0, word1, self.data_length & 0xFFFF)

    @classmethod
    def unpack(cls, buf: bytes) -> "CcsdsPrimary":
        if len(buf) < CCSDS_PRIMARY_HEADER_SIZE:
            raise ValueError(
                f"CCSDS primary header needs {CCSDS_PRIMARY_HEADER_SIZE} bytes, "
                f"got {len(buf)}"
            )
        w0, w1, data_length = struct.unpack(">HHH", buf[:CCSDS_PRIMARY_HEADER_SIZE])
        return cls(
            version=(w0 >> 13) & 0x7,
            type=(w0 >> 12) & 0x1,
            sec_hdr_flag=(w0 >> 11) & 0x1,
            apid=w0 & 0x7FF,
            seq_flags=(w1 >> 14) & 0x3,
            seq_count=w1 & 0x3FFF,
            data_length=data_length,
        )


# ---------------------------------------------------------------------------
# PUS-C TC secondary header (5 bytes).
# ---------------------------------------------------------------------------

PUS_TC_SECONDARY_HEADER_SIZE = 5
PUS_VERSION_C = 2

PUS_SERVICE_TEST = 17
PUS_17_SUBTYPE_ARE_YOU_ALIVE_TC = 1
PUS_17_SUBTYPE_ARE_YOU_ALIVE_TM = 2


@dataclass
class PusTcSecondary:
    pus_version: int = PUS_VERSION_C
    ack_flags: int = 0
    service_type: int = 0
    service_subtype: int = 0
    source_id: int = 0

    def pack(self) -> bytes:
        return struct.pack(
            ">BBBH",
            ((self.pus_version & 0xF) << 4) | (self.ack_flags & 0xF),
            self.service_type & 0xFF,
            self.service_subtype & 0xFF,
            self.source_id & 0xFFFF,
        )


# ---------------------------------------------------------------------------
# PUS-C TM secondary header (10 bytes).
# ---------------------------------------------------------------------------

PUS_TM_SECONDARY_HEADER_SIZE = 10


@dataclass
class PusTmSecondary:
    pus_version: int
    sc_time_ref_status: int
    service_type: int
    service_subtype: int
    msg_counter: int
    destination_id: int
    time_seconds: int

    @classmethod
    def unpack(cls, buf: bytes) -> "PusTmSecondary":
        if len(buf) < PUS_TM_SECONDARY_HEADER_SIZE:
            raise ValueError(
                f"PUS-C TM secondary header needs {PUS_TM_SECONDARY_HEADER_SIZE} "
                f"bytes, got {len(buf)}"
            )
        ver_status, service_type, service_subtype, msg_counter, dest_id, t = (
            struct.unpack(">BBBBHI", buf[:PUS_TM_SECONDARY_HEADER_SIZE])
        )
        return cls(
            pus_version=(ver_status >> 4) & 0xF,
            sc_time_ref_status=ver_status & 0xF,
            service_type=service_type,
            service_subtype=service_subtype,
            msg_counter=msg_counter,
            destination_id=dest_id,
            time_seconds=t,
        )


# ---------------------------------------------------------------------------
# CRC-16-CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no XOR-out).
# ---------------------------------------------------------------------------


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Convenience builders for the slice fsw-4 wire.
# ---------------------------------------------------------------------------

PUS17_TC_PACKET_SIZE = 13
PUS17_TM_PACKET_SIZE = 18

# Pinned by docs/wire/pus-17.md.
FSW4_APID = 0x100


def build_pus17_are_you_alive_tc(
    *,
    apid: int = FSW4_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-17[1] TC packet (13 bytes, CRC included)."""
    primary = CcsdsPrimary(
        type=PACKET_TYPE_TC,
        sec_hdr_flag=1,
        apid=apid,
        seq_count=seq_count,
        data_length=PUS_TC_SECONDARY_HEADER_SIZE + 2 - 1,
    )
    tc_sec = PusTcSecondary(
        service_type=PUS_SERVICE_TEST,
        service_subtype=PUS_17_SUBTYPE_ARE_YOU_ALIVE_TC,
        source_id=source_id,
    )
    body = primary.pack() + tc_sec.pack()
    return body + struct.pack(">H", crc16_ccitt_false(body))


@dataclass
class Pus17Tm:
    primary: CcsdsPrimary
    secondary: PusTmSecondary
    crc_ok: bool

    @classmethod
    def decode(cls, packet: bytes) -> "Pus17Tm":
        if len(packet) != PUS17_TM_PACKET_SIZE:
            raise ValueError(
                f"PUS-17[2] TM packet must be {PUS17_TM_PACKET_SIZE} bytes, "
                f"got {len(packet)}"
            )
        primary = CcsdsPrimary.unpack(packet[:CCSDS_PRIMARY_HEADER_SIZE])
        secondary = PusTmSecondary.unpack(
            packet[
                CCSDS_PRIMARY_HEADER_SIZE : CCSDS_PRIMARY_HEADER_SIZE
                + PUS_TM_SECONDARY_HEADER_SIZE
            ]
        )
        on_wire = struct.unpack(">H", packet[-2:])[0]
        computed = crc16_ccitt_false(packet[:-2])
        return cls(primary=primary, secondary=secondary, crc_ok=(on_wire == computed))
