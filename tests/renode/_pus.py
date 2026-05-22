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
PUS5_EVT_MODE_CHANGED = 0x0004  # aux: previous mode id, new mode id (2 bytes, u8 each)

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

# PUS-20 on-board parameter management (docs/wire/pus-20.md).
PUS_SERVICE_ONBOARD_PARAMETER = 20
PUS_20_SUBTYPE_REPORT_REQUEST = 1  # TC, report parameter values
PUS_20_SUBTYPE_VALUE_REPORT = 2  # TM, parameter value report
PUS_20_SUBTYPE_SET_REQUEST = 3  # TC, set parameter values

# Datapool scalar type codes and their on-wire widths. The type code
# is carried only in the MIB, never in a PUS-20 packet — the wire is
# not self-describing, so a decoder needs the id->type map.
DP_TYPE_U8 = 0
DP_TYPE_U16 = 1
DP_TYPE_U32 = 2
DP_TYPE_I8 = 3
DP_TYPE_I16 = 4
DP_TYPE_I32 = 5
DP_TYPE_F32 = 6
DP_TYPE_WIDTH = {
    DP_TYPE_U8: 1,
    DP_TYPE_U16: 2,
    DP_TYPE_U32: 4,
    DP_TYPE_I8: 1,
    DP_TYPE_I16: 2,
    DP_TYPE_I32: 4,
    DP_TYPE_F32: 4,
}

# Framework datapool parameter IDs the tc_uart sample registers
# (reserved range 0x0001..0x00FF).
DP_PARAM_HK_PERIOD_SEC = 0x0001

# PUS-11 on-board (time-based) scheduling (docs/wire/pus-11.md).
PUS_SERVICE_SCHEDULING = 11
PUS_11_SUBTYPE_ENABLE = 1  # TC, enable the schedule
PUS_11_SUBTYPE_DISABLE = 2  # TC, disable the schedule
PUS_11_SUBTYPE_RESET = 3  # TC, delete all activities
PUS_11_SUBTYPE_INSERT = 4  # TC, insert activities
PUS_11_SUBTYPE_DELETE = 5  # TC, delete by request id
PUS_11_SUBTYPE_SUMMARY_REPORT_REQUEST = 11  # TC, summary-report by request id
PUS_11_SUBTYPE_SUMMARY_REPORT = 12  # TM, the summary report

# A scheduled activity's request identifier is the first 4 bytes of
# its telecommand (the same identifier PUS-1 uses).
SCHEDULE_REQUEST_ID_SIZE = 4

# PUS-15 on-board storage and retrieval (docs/wire/pus-15.md).
PUS_SERVICE_STORAGE = 15
PUS_15_SUBTYPE_ENABLE_STORAGE = 1  # TC, enable storage
PUS_15_SUBTYPE_DISABLE_STORAGE = 2  # TC, disable storage
PUS_15_SUBTYPE_DOWNLINK_RANGE = 9  # TC, start a by-time retrieval
PUS_15_SUBTYPE_DELETE_RANGE = 11  # TC, delete content up to a time
PUS_15_SUBTYPE_REPORT_REQUEST = 12  # TC, report the packet store
PUS_15_SUBTYPE_STORE_REPORT = 13  # TM, the packet store report

# [15,13] store report source data: enabled(1) + count(2) + oldest(4) + newest(4).
PUS15_STORE_REPORT_SOURCE_SIZE = 11
# primary 6 + TM sec 10 + source data 11 + CRC 2.
PUS15_STORE_REPORT_PACKET_SIZE = 29

# PUS-13 large data transfer, downlink (docs/wire/pus-13.md).
PUS_SERVICE_LARGE_DATA = 13
PUS_13_SUBTYPE_FIRST_PART = 1  # TM, first downlink part
PUS_13_SUBTYPE_INTERMEDIATE_PART = 2  # TM, intermediate downlink part
PUS_13_SUBTYPE_LAST_PART = 3  # TM, last downlink part

# A PUS-13 part's source data is a 6-byte part header — transaction id
# (2) + part number (2) + total parts (2), big-endian — then the payload.
PUS13_PART_HEADER_SIZE = 6


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


def build_pus20_report_request_tc(
    *,
    param_ids: list[int],
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-20[1] report-parameter-values TC.
    Application data is a 1-byte count followed by the 2-byte
    big-endian parameter IDs."""
    app = struct.pack(">B", len(param_ids))
    for pid in param_ids:
        app += struct.pack(">H", pid)
    return build_tc(
        service_type=PUS_SERVICE_ONBOARD_PARAMETER,
        service_subtype=PUS_20_SUBTYPE_REPORT_REQUEST,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=app,
    )


def build_pus20_set_request_tc(
    *,
    params: list[tuple[int, bytes]],
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-20[3] set-parameter-values TC. ``params``
    is a list of (parameter ID, encoded big-endian value bytes) pairs;
    each value's width must match the parameter's registered type (the
    PUS-20 wire is not self-describing)."""
    app = struct.pack(">B", len(params))
    for pid, value in params:
        app += struct.pack(">H", pid) + value
    return build_tc(
        service_type=PUS_SERVICE_ONBOARD_PARAMETER,
        service_subtype=PUS_20_SUBTYPE_SET_REQUEST,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=app,
    )


def build_pus11_enable_tc(
    *,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-11[1] enable-schedule TC (no application
    data)."""
    return build_tc(
        service_type=PUS_SERVICE_SCHEDULING,
        service_subtype=PUS_11_SUBTYPE_ENABLE,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
    )


def build_pus11_insert_tc(
    *,
    activities: list[tuple[int, bytes]],
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-11[4] insert TC. ``activities`` is a list
    of (absolute release time, embedded telecommand bytes) pairs;
    application data is a 1-byte count followed by each pair's 4-byte
    big-endian release time and verbatim telecommand."""
    app = struct.pack(">B", len(activities))
    for release_time, tc in activities:
        app += struct.pack(">I", release_time) + bytes(tc)
    return build_tc(
        service_type=PUS_SERVICE_SCHEDULING,
        service_subtype=PUS_11_SUBTYPE_INSERT,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=app,
    )


def build_pus11_report_tc(
    *,
    request_ids: list[bytes],
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-11[11] summary-report-request TC.
    ``request_ids`` is a list of 4-byte request identifiers;
    application data is a 1-byte count followed by them."""
    app = struct.pack(">B", len(request_ids))
    for rid in request_ids:
        app += bytes(rid)
    return build_tc(
        service_type=PUS_SERVICE_SCHEDULING,
        service_subtype=PUS_11_SUBTYPE_SUMMARY_REPORT_REQUEST,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=app,
    )


def build_pus15_enable_storage_tc(
    *,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-15[1] enable-storage TC (no application
    data)."""
    return build_tc(
        service_type=PUS_SERVICE_STORAGE,
        service_subtype=PUS_15_SUBTYPE_ENABLE_STORAGE,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
    )


def build_pus15_disable_storage_tc(
    *,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-15[2] disable-storage TC (no application
    data)."""
    return build_tc(
        service_type=PUS_SERVICE_STORAGE,
        service_subtype=PUS_15_SUBTYPE_DISABLE_STORAGE,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
    )


def build_pus15_downlink_tc(
    *,
    from_time: int,
    to_time: int,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-15[9] start-a-by-time-period-retrieval TC.
    Application data is the inclusive window: a 4-byte big-endian
    from-time followed by a 4-byte big-endian to-time."""
    return build_tc(
        service_type=PUS_SERVICE_STORAGE,
        service_subtype=PUS_15_SUBTYPE_DOWNLINK_RANGE,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=struct.pack(">II", from_time, to_time),
    )


def build_pus15_delete_tc(
    *,
    time_seconds: int,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-15[11] delete-content-up-to-a-time TC.
    Application data is the 4-byte big-endian cutoff time."""
    return build_tc(
        service_type=PUS_SERVICE_STORAGE,
        service_subtype=PUS_15_SUBTYPE_DELETE_RANGE,
        ack_flags=ack_flags,
        apid=apid,
        seq_count=seq_count,
        source_id=source_id,
        app_data=struct.pack(">I", time_seconds),
    )


def build_pus15_report_request_tc(
    *,
    ack_flags: int = 0,
    apid: int = FSW_APID,
    seq_count: int = 0,
    source_id: int = 0,
) -> bytes:
    """Encode a complete PUS-15[12] report-the-packet-store TC (no
    application data). The FSW replies with one [15,13] report."""
    return build_tc(
        service_type=PUS_SERVICE_STORAGE,
        service_subtype=PUS_15_SUBTYPE_REPORT_REQUEST,
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


def decode_pus20_report(tm: DecodedTm, type_map: dict[int, int]) -> dict[int, bytes]:
    """Split a PUS-20[2] parameter value report's source data into an
    ``{parameter ID: raw value bytes}`` map. ``type_map`` gives each
    parameter's datapool type code so the variable-width values can be
    cut — the PUS-20 wire is not self-describing (docs/wire/pus-20.md).
    The caller interprets the value bytes (e.g. big-endian int)."""
    data = tm.source_data
    count = data[0]
    out: dict[int, bytes] = {}
    pos = 1
    for _ in range(count):
        pid = int.from_bytes(data[pos : pos + 2], "big")
        pos += 2
        width = DP_TYPE_WIDTH[type_map[pid]]
        out[pid] = data[pos : pos + width]
        pos += width
    return out


def decode_pus11_summary_report(tm: DecodedTm) -> list[tuple[int, bytes]]:
    """Parse a PUS-11[12] schedule summary report's source data into a
    list of (release time, 4-byte request identifier) tuples."""
    data = tm.source_data
    count = data[0]
    out: list[tuple[int, bytes]] = []
    pos = 1
    for _ in range(count):
        release_time = int.from_bytes(data[pos : pos + 4], "big")
        request_id = data[pos + 4 : pos + 4 + SCHEDULE_REQUEST_ID_SIZE]
        out.append((release_time, request_id))
        pos += 4 + SCHEDULE_REQUEST_ID_SIZE
    return out


@dataclass
class Pus15StoreReport:
    """A decoded PUS-15[13] packet store report (docs/wire/pus-15.md)."""

    enabled: bool
    count: int
    oldest_time: int
    newest_time: int


def decode_pus15_store_report(tm: DecodedTm) -> Pus15StoreReport:
    """Parse a PUS-15[13] packet store report's 11-byte source data: a
    1-byte storage-enabled flag, a 2-byte big-endian packet count, and
    the 4-byte big-endian oldest / newest storage times."""
    data = tm.source_data
    if len(data) != PUS15_STORE_REPORT_SOURCE_SIZE:
        raise ValueError(
            f"PUS-15[13] source data must be {PUS15_STORE_REPORT_SOURCE_SIZE} "
            f"bytes, got {len(data)}"
        )
    enabled, count, oldest, newest = struct.unpack(">BHII", data)
    return Pus15StoreReport(
        enabled=bool(enabled),
        count=count,
        oldest_time=oldest,
        newest_time=newest,
    )


@dataclass
class Pus13Part:
    """A decoded PUS-13 downlink part (docs/wire/pus-13.md)."""

    transaction_id: int
    part_number: int
    total_parts: int
    payload: bytes


def decode_pus13_part(tm: DecodedTm) -> Pus13Part:
    """Parse a PUS-13 [13,1] / [13,2] / [13,3] part: a 6-byte part
    header — transaction id, 0-based part number, total parts, all
    big-endian — followed by the part payload."""
    data = tm.source_data
    if len(data) < PUS13_PART_HEADER_SIZE:
        raise ValueError(
            f"PUS-13 part needs at least {PUS13_PART_HEADER_SIZE} source bytes, "
            f"got {len(data)}"
        )
    transaction_id, part_number, total_parts = struct.unpack(
        ">HHH", data[:PUS13_PART_HEADER_SIZE]
    )
    return Pus13Part(
        transaction_id=transaction_id,
        part_number=part_number,
        total_parts=total_parts,
        payload=data[PUS13_PART_HEADER_SIZE:],
    )


def reassemble_pus13(parts: list[DecodedTm]) -> bytes:
    """Reassemble a large data unit from its PUS-13 part packets: the
    concatenation of the part payloads in ascending part-number order.
    The caller has filtered ``parts`` to a single transaction."""
    decoded = sorted(
        (decode_pus13_part(p) for p in parts), key=lambda d: d.part_number
    )
    return b"".join(d.payload for d in decoded)
