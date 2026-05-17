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

PUS_SERVICE_VERIFICATION = 1
PUS_1_SUBTYPE_ACCEPTANCE_SUCCESS = 1
PUS_1_SUBTYPE_ACCEPTANCE_FAILURE = 2
PUS_1_SUBTYPE_COMPLETION_SUCCESS = 7
PUS_1_SUBTYPE_COMPLETION_FAILURE = 8

# PUS-C TC ack-flag bit masks (pus_tc.h / docs/wire/pus-17.md).
ACK_ACCEPTANCE = 0x1
ACK_START = 0x2
ACK_PROGRESS = 0x4
ACK_COMPLETION = 0x8

# PUS-1 source-data sizes and failure codes (docs/wire/pus-1.md).
PUS1_REQUEST_ID_SIZE = 4
PUS1_SUCCESS_TM_PACKET_SIZE = 22
PUS1_FAILURE_TM_PACKET_SIZE = 23

FC_NONE = 0
FC_BAD_PRIMARY = 1
FC_ILLEGAL_APID = 2
FC_LENGTH_ERROR = 3
FC_CRC_FAILURE = 4
FC_BAD_PUS_VERSION = 5
FC_UNKNOWN_SERVICE = 6
FC_UNKNOWN_SUBTYPE = 7
FC_EXEC_FAILURE = 8

# PUS-5 event reporting (docs/wire/pus-5.md).
PUS_SERVICE_EVENT_REPORTING = 5
PUS_5_SUBTYPE_INFO = 1
PUS_5_SUBTYPE_LOW = 2
PUS_5_SUBTYPE_MEDIUM = 3
PUS_5_SUBTYPE_HIGH = 4

PUS5_EVENT_ID_SIZE = 2
# Bare event (no auxiliary data): primary 6 + TM sec 10 + event ID 2 + CRC 2.
PUS5_BARE_TM_PACKET_SIZE = 20

# fsw-core framework event-definition IDs (reserved block 0x0001..0x00FF).
PUS5_EVT_FSW_BOOT = 0x0001
PUS5_EVT_TC_REJECTED = 0x0002  # aux: fc, service_type, service_subtype (3 bytes)
PUS5_EVT_RX_OVERFLOW = 0x0003  # aux: bytes-dropped delta (u32, big-endian)

# PUS-3 housekeeping & diagnostic data reporting (docs/wire/pus-3.md).
PUS_SERVICE_HOUSEKEEPING = 3
PUS_3_SUBTYPE_HK_PARAM_REPORT = 25  # TM, the report
PUS_3_SUBTYPE_ONE_SHOT_POLL = 27  # TC, "generate one shot"

PUS3_SID_SIZE = 2
# Reserved framework-structure block 0x0001..0x00FF (mission 0x0100+).
PUS3_SID_FRAMEWORK_DIAG = 0x0001
PUS3_HK_SOURCE_DATA_SIZE = 29  # SID(2) + 27-byte param block
# primary 6 + TM sec 10 + source data 29 + CRC 2.
PUS3_HK_TM_PACKET_SIZE = 47


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
# Convenience builders / decoders for the on-board TC/TM wire.
# ---------------------------------------------------------------------------

PUS17_TC_PACKET_SIZE = 13
PUS17_TM_PACKET_SIZE = 18

# Pinned by docs/wire/pus-17.md.
FSW_APID = 0x100


def build_tc(
    *,
    service_type: int,
    service_subtype: int,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
    app_data: bytes = b"",
    data_length: int | None = None,
) -> bytes:
    """Encode a complete PUS-C TC packet (CRC included). ``app_data`` is
    appended after the TC secondary header (e.g. a PUS-3[27] Structure
    ID). ``data_length`` overrides the (correct) CCSDS Packet Data
    Length so length-error paths can be exercised."""
    primary = CcsdsPrimary(
        type=PACKET_TYPE_TC,
        sec_hdr_flag=1,
        apid=apid,
        seq_count=seq_count,
        data_length=(PUS_TC_SECONDARY_HEADER_SIZE + len(app_data) + 2 - 1)
        if data_length is None
        else data_length,
    )
    tc_sec = PusTcSecondary(
        ack_flags=ack_flags,
        service_type=service_type,
        service_subtype=service_subtype,
        source_id=source_id,
    )
    body = primary.pack() + tc_sec.pack() + app_data
    return body + struct.pack(">H", crc16_ccitt_false(body))


def build_pus3_oneshot_poll_tc(
    *,
    sid: int = PUS3_SID_FRAMEWORK_DIAG,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-3[27] one-shot-poll TC. Application data is
    exactly the 2-byte big-endian Structure ID."""
    return build_tc(
        service_type=PUS_SERVICE_HOUSEKEEPING,
        service_subtype=PUS_3_SUBTYPE_ONE_SHOT_POLL,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=struct.pack(">H", sid),
    )


def build_pus17_are_you_alive_tc(
    *,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-17[1] TC packet (13 bytes, CRC included)."""
    return build_tc(
        service_type=PUS_SERVICE_TEST,
        service_subtype=PUS_17_SUBTYPE_ARE_YOU_ALIVE_TC,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
    )


def split_packets(stream: bytes) -> list[bytes]:
    """Walk a back-to-back TM byte stream into individual CCSDS Space
    Packets using each primary header's Packet Data Length. Trailing
    bytes that cannot form a full packet are ignored (the caller
    asserts on the count)."""
    out: list[bytes] = []
    pos = 0
    while pos + CCSDS_PRIMARY_HEADER_SIZE <= len(stream):
        p = CcsdsPrimary.unpack(stream[pos : pos + CCSDS_PRIMARY_HEADER_SIZE])
        size = CCSDS_PRIMARY_HEADER_SIZE + p.data_length + 1
        if pos + size > len(stream):
            break
        out.append(stream[pos : pos + size])
        pos += size
    return out


@dataclass
class DecodedTm:
    """A decoded TM Space Packet. Works for any service: PUS-17[2]
    has empty source data; PUS-1 reports carry a 4-byte request ID
    (plus a failure-code byte on the failure subtypes); PUS-5 event
    reports carry a 2-byte event ID plus optional auxiliary data."""

    primary: CcsdsPrimary
    secondary: PusTmSecondary
    source_data: bytes
    crc_ok: bool

    @classmethod
    def decode(cls, packet: bytes) -> "DecodedTm":
        if len(packet) < CCSDS_PRIMARY_HEADER_SIZE + PUS_TM_SECONDARY_HEADER_SIZE + 2:
            raise ValueError(f"TM packet too short: {len(packet)} bytes")
        primary = CcsdsPrimary.unpack(packet[:CCSDS_PRIMARY_HEADER_SIZE])
        sec_end = CCSDS_PRIMARY_HEADER_SIZE + PUS_TM_SECONDARY_HEADER_SIZE
        secondary = PusTmSecondary.unpack(packet[CCSDS_PRIMARY_HEADER_SIZE:sec_end])
        source_data = packet[sec_end:-2]
        on_wire = struct.unpack(">H", packet[-2:])[0]
        computed = crc16_ccitt_false(packet[:-2])
        return cls(
            primary=primary,
            secondary=secondary,
            source_data=source_data,
            crc_ok=(on_wire == computed),
        )

    @property
    def request_id(self) -> bytes:
        """The 4-byte PUS-1 request ID (empty for non-PUS-1 TM)."""
        return self.source_data[:PUS1_REQUEST_ID_SIZE]

    @property
    def failure_code(self) -> int | None:
        """The PUS-1 failure code, or ``None`` for success / non-PUS-1."""
        if len(self.source_data) > PUS1_REQUEST_ID_SIZE:
            return self.source_data[PUS1_REQUEST_ID_SIZE]
        return None

    @property
    def event_id(self) -> int:
        """The PUS-5 event-definition ID (first 2 source-data bytes,
        big-endian). Only meaningful for PUS-5 TM."""
        return int.from_bytes(self.source_data[:PUS5_EVENT_ID_SIZE], "big")

    @property
    def event_aux(self) -> bytes:
        """The PUS-5 auxiliary data (source data after the 2-byte
        event ID). Only meaningful for PUS-5 TM."""
        return self.source_data[PUS5_EVENT_ID_SIZE:]

    @property
    def pus3_sid(self) -> int:
        """The PUS-3 Structure ID (first 2 source-data bytes,
        big-endian). Only meaningful for PUS-3[25] TM."""
        return int.from_bytes(self.source_data[:PUS3_SID_SIZE], "big")
