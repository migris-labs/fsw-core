# PUS-13 large data transfer — wire format

Authoritative byte-level specification for **PUS-13 — Large data
transfer (downlink)** in the Migris flight-software framework. Pinned
by slice fsw-12. This document **inherits** the CCSDS primary header,
UART framing, CRC, and PUS-C TM secondary header pinned in
[`pus-17.md`](pus-17.md) — only the PUS-13-specific surface (subtypes,
the part header, the transfer model, reassembly) is specified here.
Changes to either document are breaking changes to the platform's
TM/TC interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-13 is §8.13 (large packet / large
  message transfer). Migris implements a pragmatic downlink-only
  subset over a single transfer at a time.

## Scope of this slice

A spacecraft sometimes has to downlink a unit of data larger than a
single CCSDS Space Packet — a schedule detail report, a window of
stored telemetry, a payload product. PUS-13 splits such a unit into
an ordered sequence of telemetry **part** packets the ground
reassembles. Slice fsw-12 ships the downlink direction:

| Subtype | Message                    | Dir | In fsw-12 |
|---------|----------------------------|-----|-----------|
| 1       | First downlink part         | TM  | ✅        |
| 2       | Intermediate downlink part  | TM  | ✅        |
| 3       | Last downlink part          | TM  | ✅        |
| 16      | Downlink abort report       | TM  | ❌ deferred |

PUS-13 is **telemetry-only** this slice: it has no inbound subtype, so
the TC router does not route service 13 — a TC with service type 13 is
rejected at acceptance with `UNKNOWN_SERVICE`.

Deliberately deferred (each with a trigger to revisit):

- **The uplink direction** ([13,9]/[13,10]/[13,11] and the uplink
  abort). No on-board feature ingests a data unit larger than one
  Space Packet — the largest inbound TC is a 192-byte PUS-11[4]
  insert. *Trigger*: the first on-board consumer of a large uplinked
  data unit.
- **The [13,16] downlink abort report.** Nothing in fsw-12 interrupts
  a transfer in progress — every transfer runs to completion.
  *Trigger*: the first slice where a transfer can be interrupted (a
  closing pass window, or PUS-15 retrieval rewired onto PUS-13 where a
  delete races the transfer).
- **Concurrent transactions.** One transfer at a time. *Trigger*: a
  second producer of large data that must downlink while the first is
  still in progress.

## Pinned platform decisions (additional to pus-17.md)

| Decision                | Value                                                       |
|-------------------------|-------------------------------------------------------------|
| Service Type            | `13`                                                        |
| Part header             | 6 bytes — transaction id (2) + part number (2) + total parts (2), all big-endian |
| Part number             | 0-based, big-endian `uint16`                                |
| Transaction id          | big-endian `uint16`, constant across every part of one transfer |
| Part size               | sender-side chunking parameter (`MIGRIS_PUS13_PART_SIZE`, default 64 bytes); the last part may be shorter |
| CCSDS sequence flags    | `UNSEGMENTED` (3) on every part — see *Sequence flags* below |
| TM sequence count       | shared per-APID across **all** services                      |
| Message Counter         | per (service, subtype) — [13,1] / [13,2] / [13,3] each have their own |
| Destination ID          | echoes the triggering TC's source ID, or `0` for a spontaneous downlink |

## The transfer model

A large data unit is downlinked as `N` part packets. The unit is
sliced into chunks of at most `MIGRIS_PUS13_PART_SIZE` bytes; the last
chunk carries the remainder. Each chunk becomes one part packet:

- the part at index `0` of a multi-part transfer is a **[13,1]** first
  part;
- the part at index `N - 1` is a **[13,3]** last part;
- any part in between is a **[13,2]** intermediate part;
- a **single-part** transfer (`N = 1`) is one **[13,3]** last part.

Every part carries the same 6-byte part header — transaction id,
0-based part number, total part count — which is the authoritative
reassembly key. The on-board sender emits one part per main-loop
iteration, so a transfer drips out interleaved with the rest of the
telemetry stream rather than monopolising the link.

### Sequence flags

Every part packet sets the CCSDS sequence flags to `UNSEGMENTED` (3),
identically to every other packet the framework emits. PUS-13
segmentation is a **service-layer** concern carried by the part
header; each part is itself a complete, standalone Space Packet. The
parts of a transfer also share the APID's single TM sequence-count
space with all other telemetry, so they are not contiguous in
sequence count — CCSDS-level segment reassembly would be incorrect
here. Reassembly is by the part header alone.

## Subtypes [13,1] / [13,2] / [13,3] — downlink part (TM)

The source data of every part is the 6-byte part header followed by
the part's payload:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    TT TT        transaction id (u16, big-endian)
2..3    PP PP        part number, 0-based (u16, big-endian)
4..5    NN NN        total parts in this transfer (u16, big-endian)
6..     <payload>    1..MIGRIS_PUS13_PART_SIZE bytes of the data unit
```

A part packet on the wire is therefore:

```
primary header (6) + PUS-C TM secondary header (10)
  + part header (6) + payload (1..PART_SIZE) + CRC (2)
```

— that is `24 + payload` bytes. With the default 64-byte part size the
largest part packet is 88 bytes, inside the framework's 128-byte
telemetry buffers.

## Reassembly (ground side)

A receiver reconstructs a large data unit from its parts as follows:

1. Group received [13,1] / [13,2] / [13,3] packets by their part-header
   **transaction id**. Every part of one transfer carries the same id.
2. Within a group, every part declares the same **total parts** count.
   The transfer is complete once all part numbers `0 .. total-1` have
   been received exactly once.
3. The reconstructed data unit is the concatenation of the parts'
   payloads in ascending **part-number** order.
4. Each part's payload length is the packet's CCSDS data length minus
   the TM secondary header, the part header and the CRC — the receiver
   does not need to know the sender's part size.

Reassembly keys on the part header, not on the CCSDS sequence count or
the subtype: the subtype ([13,1] first, [13,3] last) and the sequence
flags are positional labels; the part header is authoritative.

## Worked example — a four-part transfer

A 200-byte data unit, transaction id `0x0001`, sliced with the default
64-byte part size, downlinks as four parts:

| Part | Subtype | Part no. | Total | Payload | Packet size |
|------|---------|----------|-------|---------|-------------|
| 0    | [13,1]  | 0        | 4     | 64      | 88          |
| 1    | [13,2]  | 1        | 4     | 64      | 88          |
| 2    | [13,2]  | 2        | 4     | 64      | 88          |
| 3    | [13,3]  | 3        | 4     | 8       | 32          |

The first part [13,1] on APID `0x100`, total `88` bytes:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    09 00 Cs ss 00 51   primary hdr: TM, APID 0x100, seq count s,
                            data length 81
6       20           PUS-C version (2) + time-ref status (0)
7       0D           Service Type = 13
8       01           Subtype = 1 (first downlink part)
9       MM           message counter for [13,1]
10..11  00 00        destination ID = 0 (spontaneous downlink)
12..15  tt tt tt tt  CUC coarse seconds
16..17  00 01        transaction id = 0x0001
18..19  00 00        part number = 0
20..21  00 04        total parts = 4
22..85  <64 bytes>   payload — unit bytes 0..63
86..87  ?? ??        CRC-16-CCITT-FALSE over bytes 0..85
```

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

Adding a deferred subtype (the [13,16] abort report, the uplink
direction) with its own layout is non-breaking provided the subtypes
and rules above are unchanged.
