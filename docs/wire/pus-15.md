# PUS-15 on-board storage — wire format

Authoritative byte-level specification for **PUS-15 — On-board
storage and retrieval** in the Migris flight-software framework.
Pinned by slice fsw-11. This document **inherits** the CCSDS primary
header, UART framing, CRC, and PUS-C TC/TM secondary headers pinned in
[`pus-17.md`](pus-17.md) — only the PUS-15-specific surface (subtypes,
application data, source data, the storage / retrieval rules) is
specified here. Changes to either document are breaking changes to
the platform's TM/TC interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-15 is §8.15 (on-board storage
  and retrieval). Migris implements a pragmatic core subset over one
  predefined packet store.

## Scope of this slice

A **packet store** captures the telemetry the spacecraft produces
between ground contacts; on a pass, ground retrieves a time window of
stored packets and they are downlinked. Slice fsw-11 ships the core
subset over one predefined packet store:

| Subtype | Message                                  | Dir | In fsw-11 |
|---------|------------------------------------------|-----|-----------|
| 1       | Enable storage in the packet store        | TC  | ✅        |
| 2       | Disable storage in the packet store       | TC  | ✅        |
| 9       | Start a by-time-period retrieval (downlink)| TC | ✅        |
| 11      | Delete the content up to a time           | TC  | ✅        |
| 12      | Report the packet store                   | TC  | ✅        |
| 13      | Packet store report                       | TM  | ✅        |

Deliberately deferred: dynamic packet-store creation / deletion /
configuration, storage-selection management (which packets a store
captures), and the packet-store catalogue — one predefined store
covers the produce-on-orbit / downlink-next-pass model. The store is
**RAM-backed and volatile** (empty after a reboot); non-volatile mass
memory across reset is a future capability needing a flash storage
subsystem (see CHANGELOG.md).

## Pinned platform decisions (additional to pus-17.md)

| Decision                | Value                                                       |
|-------------------------|-------------------------------------------------------------|
| Service Type            | `15`                                                        |
| Storage time            | 4 bytes, **big-endian**, absolute CUC coarse seconds         |
| Overflow policy         | Circular — when the store is full, a new packet overwrites the OLDEST |
| Storage enabled state   | Starts **enabled** — the store only records, so capturing from boot is harmless and loses no early telemetry |
| TM sequence count       | Shared per-APID across **all** services                      |
| Message Counter         | Per (service, subtype) — the [13] report has its own         |
| Destination ID          | Echoes the triggering TC's source ID                         |

## The store and retrieval model

The packet store holds telemetry packets in non-decreasing storage-
time order (the FSW clock is monotonic). When full it is circular —
the oldest packet is overwritten, so the most recent telemetry is
always retained.

A `[15,9]` downlink does **not** itself emit telemetry: it ARMS a
retrieval over the inclusive `[from, to]` time window. The FSW then
re-emits, verbatim, each stored packet whose storage time falls in
the window — one packet per main-loop iteration — keeping each
packet's original headers and sequence counts (a replay of history).
A retrieval is non-destructive; `[15,11]` delete is the separate
destructive operation.

While a retrieval is in progress the store is **frozen**: storage is
suspended and `[15,9]` / `[15,11]` are rejected, so the buffer cannot
shift under the retrieval. (fsw-12's PUS-13 large-data-transfer will
add a chunked-transfer path for the same retrieval.)

## Subtypes [15,1] / [15,2] — enable / disable storage (TC)

No application data (application-data length 0). [15,1] resumes
packet capture, [15,2] suspends it. Neither emits telemetry; the
PUS-1 completion report, if requested, is the confirmation.

## Subtype [15,9] — start a by-time-period retrieval (TC)

Application data is the inclusive retrieval window:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..3    FF FF FF FF  from-time (u32, big-endian, absolute CUC)
4..7    TT TT TT TT  to-time   (u32, big-endian, absolute CUC)
                     application data length = 8
```

`from-time` must not be after `to-time`. The retrieved packets are
downlinked verbatim by the main loop after this TC's verification.

## Subtype [15,11] — delete content up to a time (TC)

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..3    TT TT TT TT  delete every stored packet with storage time
                     at or before this (u32, big-endian)
                     application data length = 4
```

## Subtype [15,12] — report the packet store (TC)

No application data. The FSW replies with one [15,13] report.

## Subtype [15,13] — packet store report (TM)

Source data is a fixed 11-byte summary of the packet store:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       SS           storage enabled — 1 = enabled, 0 = disabled
1..2    NN NN        number of packets currently stored (u16, big-endian)
3..6    OO OO OO OO  oldest stored packet's storage time (u32, big-endian)
7..10   WW WW WW WW  newest stored packet's storage time (u32, big-endian)
```

On an empty store the count is 0 and both times are 0. Total packet
size is a fixed **29 bytes** (primary 6 + TM sec 10 + source 11 +
CRC 2).

## Emission and failure rules

A PUS-15 TC is validated by the generic accept stage first (CCSDS
framing, length, CRC, PUS-C version, routable service). Past
acceptance, every PUS-15-specific problem is an **execution-stage**
failure: the TC is *accepted* (PUS-1[1] if requested), then its
completion stage fails with **PUS-1[8]**, failure code
`EXEC_FAILURE`. The execution failures are:

- application data of the wrong length (a [1]/[2]/[12] carrying any
  data; a [15,9] not exactly 8 bytes; a [15,11] not exactly 4);
- a [15,9] whose from-time is after its to-time;
- a [15,9] or [15,11] issued while a retrieval is already in
  progress;
- no packet store is wired on the application process.

An unsupported subtype (anything other than [1], [2], [9], [11] or
[12] inbound) is the one PUS-15 failure mapped to PUS-1[8]
`UNKNOWN_SUBTYPE` rather than `EXEC_FAILURE`.

## Worked example — start a retrieval

A [15,9] downlink of the window [`0x00001000`, `0x00002000`] on APID
`0x100`, total `6 + 5 + 8 + 2 = 21` bytes:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    19 00 C0 SS 00 0E   primary hdr: TC, APID 0x100, data length 14
6       2N           PUS-C ver (2) + ack flags (N)
7       0F           Service Type = 15
8       09           Subtype = 9 (start a by-time-period retrieval)
9..10   DD DD        Source ID
11..14  00 00 10 00  from-time = 0x00001000, big-endian
15..18  00 00 20 00  to-time   = 0x00002000, big-endian
19..20  ?? ??        CRC-16-CCITT-FALSE over bytes 0..18
```

The FSW accepts the TC, arms the retrieval, and (after this TC's
PUS-1 verification) re-emits each stored packet in the window,
verbatim, one per main-loop iteration.

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

Adding a deferred subtype (packet-store management, storage-selection
management, the catalogue) with its own layout is non-breaking
provided the subtypes and rules above are unchanged.
