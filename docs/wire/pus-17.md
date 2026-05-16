# PUS-17 connection test — wire format

Authoritative byte-level specification for the Migris flight-software
framework's first on-board PUS service: **PUS-17 — Test (connection
test)**. Pinned by slice fsw-4. Every future PUS service that we add
inherits the primary header / framing / CRC choices made here, so
changes to this document are breaking changes to the platform's TM/TC
interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol (link-layer-independent
  packet format; the wire format we use over UART today and over the
  CCSDS RF link later).
- **ECSS-E-ST-70-41C** — Telemetry and telecommand packet utilisation
  (PUS-C). We implement a pragmatic subset (workspace `CLAUDE.md`,
  *Decisions Pinned → PUS service baseline*); this document specifies
  exactly the surface needed for PUS-17.

## Pinned platform decisions

| Decision                       | Value                                          |
|--------------------------------|------------------------------------------------|
| Packet protocol                | CCSDS Space Packet Protocol (CCSDS 133.0-B-2)  |
| PUS revision                   | PUS-C (ECSS-E-ST-70-41C)                       |
| APID for the fsw-core test AP  | `0x100` (256)                                  |
| Sequence flags                 | `11` (unsegmented, single-packet)              |
| Packet error control           | CRC-16-CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no XOR-out) |
| UART framing                   | None beyond CCSDS — `Packet Data Length` is the frame delimiter |
| TM time field                  | 4-byte CCSDS Unsegmented Code (CUC), coarse seconds, no fine    |
| Time epoch (placeholder)       | Boot-relative (mission-config later, see PUS-9 follow-up)       |

`APID 0x100` is the *fsw-core test application process*. APIDs `0x000`
through `0x0FF` are reserved for future spacecraft routing structure
(per-subsystem APIDs, broadcast slots, etc.) — do not allocate from
that range opportunistically. Mission FSW (e.g. `cry4-fsw`) will pick
its own APIDs and document them in its own wire-format docs.

## Byte-level layout

### CCSDS Space Packet primary header (6 bytes, all big-endian)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Ver |T|S|        APID         | SeqF|     Sequence Count       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Packet Data Length                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field              | Bits | Value used                                   |
|--------------------|------|----------------------------------------------|
| Packet Version     | 3    | `000`                                        |
| Packet Type (T)    | 1    | `0` for TM, `1` for TC                       |
| Sec. Header Flag (S)| 1   | `1` (present — PUS packets always have one)  |
| APID               | 11   | `0x100`                                      |
| Sequence Flags     | 2    | `11` (unsegmented)                           |
| Sequence Count     | 14   | Monotonic per APID per direction — **one shared count space across all services** the AP emits (CCSDS 133.0-B-2) |
| Packet Data Length | 16   | `(bytes in data field) − 1`                  |

The *data field* = secondary header + user data + (optional) packet
error control. With CRC enabled (which we do), `Packet Data Length`
covers the secondary header, the user data, and the trailing 2-byte
CRC.

### PUS-C TC secondary header (5 bytes)

Used on every TC packet we send to the FSW.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| PUS Ver| Ack |   Service Type|  Subtype      |   Source ID    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     ...        |
+-+-+-+-+-+-+-+-+
```

| Field          | Bits | Value used                                       |
|----------------|------|--------------------------------------------------|
| PUS Version    | 4    | `0010` (PUS-C)                                   |
| Ack Flags      | 4    | Honoured as of slice fsw-5 — each set bit requests the matching PUS-1 verification report (see [`pus-1.md`](pus-1.md)). `0000` requests none |
| Service Type   | 8    | `17` for the Test service                        |
| Service Subtype| 8    | `1` for "perform an are-you-alive connection test"|
| Source ID      | 16   | Ground-side identifier (operator-assignable; `0x0000` is fine for tests) |

### PUS-C TM secondary header (10 bytes)

Used on every TM packet the FSW emits.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| PUS Ver| TimeRef|  Service Type |   Subtype     |  Msg Counter  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        Destination ID         |        Time (CUC, 4 bytes)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field              | Bits | Value used                                  |
|--------------------|------|---------------------------------------------|
| PUS Version        | 4    | `0010` (PUS-C)                              |
| Spacecraft Time Ref| 4    | `0000` (status: time correlation undefined) |
| Service Type       | 8    | `17`                                        |
| Service Subtype    | 8    | `2` for "are-you-alive connection test report" |
| Message Counter    | 8    | Monotonically increasing per service+subtype |
| Destination ID     | 16   | Echoes the source ID of the triggering TC   |
| Time (CUC, 4 bytes)| 32   | Coarse seconds since boot, big-endian       |

The Time field is a 4-octet CCSDS Unsegmented Code (T-field only — the
P-field is implicit, format pinned here). Today the epoch is *boot*;
when PUS-9-equivalent time correlation lands, the epoch becomes
mission-configurable without changing the wire shape.

### Packet error control (CRC, 2 bytes)

The last two bytes of every packet (TC and TM) are a CRC-16-CCITT-FALSE
computed over **all preceding bytes** (primary header + secondary
header + user data). Parameters:

| Parameter           | Value         |
|---------------------|---------------|
| Polynomial          | `0x1021`      |
| Initial value       | `0xFFFF`      |
| Input reflection    | None          |
| Output reflection   | None          |
| Output XOR          | `0x0000`      |
| Wire byte order     | Big-endian    |

Known-good test vector (used by host unit tests):

```
input:  "123456789"  (9 ASCII bytes)
crc16:  0x29B1
```

## PUS-17[1] TC — "perform an are-you-alive connection test"

User data field is **empty**. Total packet size: 13 bytes.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    19 00        primary hdr [0..1]: ver=0, T=1 (TC), S=1, APID=0x100
2..3    C0 00        primary hdr [2..3]: SeqF=11, Count=0
4..5    00 06        Packet Data Length = 7−1 = 6
6       20           PUS-C ver (2) + ack flags (0)
7       11           Service Type = 17
8       01           Subtype = 1
9..10   00 00        Source ID
11..12  ?? ??        CRC-16-CCITT-FALSE over bytes 0..10
```

## PUS-17[2] TM — "are-you-alive connection test report"

User data field is **empty**. Total packet size: 18 bytes.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    09 00        primary hdr [0..1]: ver=0, T=0 (TM), S=1, APID=0x100
2..3    C0 NN        primary hdr [2..3]: SeqF=11, Count (FSW-assigned)
4..5    00 0B        Packet Data Length = 12−1 = 11
6       20           PUS-C ver (2) + spacecraft time ref status (0)
7       11           Service Type = 17
8       02           Subtype = 2
9       MM           Message Counter (FSW-assigned, per service+subtype)
10..11  00 00        Destination ID (echoes TC source ID)
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
16..17  ?? ??        CRC-16-CCITT-FALSE over bytes 0..15
```

The FSW echoes the Destination ID from the TC's Source ID. The TM
Sequence Count is independent of the TC's count (TM and TC have
separate count spaces, per CCSDS), and as of slice fsw-5 it is a
single per-APID counter shared with PUS-1 and every other service the
AP emits — consecutive packets in a verification burst (e.g.
`PUS-1[1] · PUS-17[2] · PUS-1[7]`) carry strictly increasing counts.
Slice fsw-5 reshaped the on-board C API (the sequence count moved out
of the per-service context into the TC router) but **the bytes on the
wire are unchanged** — this is not a wire-breaking change.

## UART framing

There is none beyond CCSDS itself. The receiver reads the 6-byte
primary header, decodes `Packet Data Length`, then reads exactly
`length + 1` more bytes to complete the packet. Verifies the CRC. No
start-of-frame marker, no byte-stuffing, no escape sequences.

This is robust enough for the emulated link in slice fsw-4 (the Renode
UART is lossless and byte-aligned by construction). When real RF
layers come in via the ground segment, framing moves up the stack to
the CCSDS TC/TM transfer frames (CCSDS 232.0-B / 132.0-B), which
provide their own synchronisation marker (`0x1ACFFC1D`) and forward
error correction. The Space Packet layer on top stays bit-identical.

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, or value semantic above is a **breaking
change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in the `CHANGELOG.md` *Changed (breaking)* section.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

Adding new fields with their own length is non-breaking *if* parsers
gate on the standard CCSDS length and service-type/subtype dispatch
remains compatible.
