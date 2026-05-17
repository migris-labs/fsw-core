# PUS-3 housekeeping & diagnostic data reporting — wire format

Authoritative byte-level specification for **PUS-3 — Housekeeping &
diagnostic data reporting** in the Migris flight-software framework.
Pinned by slice fsw-7. This document **inherits** the CCSDS primary
header, UART framing, CRC, PUS-C TC secondary header and PUS-C TM
secondary header pinned in [`pus-17.md`](pus-17.md) — only the
PUS-3-specific surface (subtypes, structure ID, source data, emission
rules) is specified here. Changes to either document are breaking
changes to the platform's TM/TC interface and must be versioned
accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-3 is §8.3 (housekeeping & diagnostic
  data reporting). Migris implements a pragmatic subset (workspace
  `CLAUDE.md`); this document specifies exactly the surface needed.

## Scope of this slice

PUS-3 has a large structure-management surface. Slice fsw-7 ships the
**spontaneous + polled report path** against a single predefined
framework structure; everything that presupposes a *parameter datapool*
(a typed, addressable on-board parameter pool a ground-defined
structure can select from) is deliberately deferred until that datapool
exists — defining it against a non-existent pool would be a wire
contract we cannot honour.

| Subtype | Direction | Meaning                                         | In fsw-7 |
|---------|-----------|-------------------------------------------------|----------|
| 25      | TM        | Housekeeping parameter report                   | ✅       |
| 27      | TC        | Generate a one-shot housekeeping report         | ✅       |
| 1 / 2   | TC        | Create / delete a housekeeping structure        | deferred |
| 3 / 4   | TC        | Create / delete a diagnostic structure          | deferred |
| 5 / 6   | TC        | Enable / disable periodic HK generation         | deferred |
| 9 / 11  | TC        | Report HK / diagnostic structure definitions    | deferred |
| 26      | TM        | Diagnostic parameter report                     | deferred |

Deferred subtypes 1–6/9/11 need the datapool (structure definitions
select datapool parameters; enable/disable needs per-structure runtime
state). The function-pointer service-dispatch table is likewise
deferred: a `switch` over two services in `tc_router.c` is correct and
minimal; the table is earned at a third independent service.

## Pinned platform decisions (additional to pus-17.md)

| Decision                    | Value                                                       |
|-----------------------------|-------------------------------------------------------------|
| Service Type                | `3`                                                         |
| Structure ID (SID)          | 2 bytes, **big-endian**, first field of the report source data |
| SID range — framework       | `0x0001`–`0x00FF` reserved for **fsw-core** structures       |
| SID range — mission         | `0x0100`+ owned by mission FSW (`cry4-fsw`); scheme pinned when that repo bootstraps |
| TM sequence count           | Shared per-APID across **all** services (CCSDS 133.0-B-2: one count space per APID per direction) — not per-service |
| Message Counter             | Per (service, subtype) — one counter for [25]               |
| Destination ID              | `0` for a spontaneous periodic report; the triggering TC's source ID for a [27]-polled report |
| TC[3,27] application data   | exactly one SID (2 bytes, big-endian) — a SID *list* is deferred with the datapool |

This mirrors the pinned "PUS-128+ vendor assignments live in `cry4` /
`cry4-fsw`, not in `fsw-core`" decision and the PUS-5 event-ID block
split. Only the framework structure fsw-core actually emits is defined;
mission-side numbering is deferred to its first consumer.

### Defined framework structures

| SID      | Name             | Contents                                            |
|----------|------------------|-----------------------------------------------------|
| `0x0001` | `FRAMEWORK_DIAG` | Framework diagnostic state — see *Source data* below |

## PUS-3 is mixed synchronous / asynchronous

The framework structure is reported two ways, **using the same
encoder** (they differ only in Destination ID, a caller argument):

- **Spontaneously**, on a fixed period (the application's main loop
  owns the cadence; a coarse FSW-clock elapsed-time check, not a kernel
  timer). Destination ID = `0`.
- **On demand**, in response to a TC[3,27] one-shot poll, as part of
  that TC's verification burst (gated by the TC's ack flags exactly
  like any routed service). Destination ID = the TC's source ID.

The shared per-APID sequence count applies to both: a spontaneous
report consumes the next count in the same space as every other TM
packet on that APID, keeping the wire strictly monotonic across the
boot event, all verification / service TM, and every periodic report.

## Source data

```
[25]:  | SID (2, BE) | parameter block (27, fixed layout) |
[27]:  application data = | SID (2, BE) |
```

The parameter block is a **frozen, fixed layout** — there is no
parameter-definition descriptor on the wire this slice (that arrives
with the datapool). Widening or narrowing it is a breaking change;
adding a *new* structure under a new SID is not.

## PUS-3[25] — framework housekeeping parameter report

Total packet size: **47 bytes** — primary 6 + TM sec 10 + source data
29 (SID 2 + parameter block 27) + CRC 2. Packet Data Length =
`10 + 29 + 2 − 1 = 40`.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    09 00        primary hdr [0..1]: ver=0, T=0 (TM), S=1, APID=0x100
2..3    Cx xx        primary hdr [2..3]: SeqF=11, Count (shared, FSW-assigned)
4..5    00 28        Packet Data Length = 41−1 = 40
6       20           PUS-C ver (2) + spacecraft time ref status (0)
7       03           Service Type = 3
8       19           Subtype = 25 (housekeeping parameter report)
9       MM           Message Counter (per service+subtype)
10..11  DD DD        Destination ID — 0 spontaneous, else poll source ID
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
                     --- source data (29 bytes) ---
16..17  00 01        Structure ID = 0x0001 (FRAMEWORK_DIAG), big-endian
18..21  UU UU UU UU  uptime seconds (u32, BE) — same value as bytes 12..15
22..23  SS SS        shared TM sequence count snapshot (u16, BE) — the
                     count this very report consumed (pre-advance)
24      P0           PUS-1 msg counter [0] acceptance-success
25      P1           PUS-1 msg counter [1] acceptance-failure
26      P2           PUS-1 msg counter [2] completion-success
27      P3           PUS-1 msg counter [3] completion-failure
28      E0           PUS-5 msg counter [0] info       (see note)
29      E1           PUS-5 msg counter [1] low        (see note)
30      E2           PUS-5 msg counter [2] medium     (see note)
31      E3           PUS-5 msg counter [3] high       (see note)
32      T0           PUS-17 msg counter
33..36  AA AA AA AA  TC accepted count (u32, BE)
37..40  RR RR RR RR  TC rejected count (u32, BE)
41..44  XX XX XX XX  UART RX-ring overflow drop count (u32, BE)
45..46  ?? ??        CRC-16-CCITT-FALSE over bytes 0..44
```

The three diagnostic counters (bytes 33..44) are `u32` so a long-lived
mission does not visibly wrap them in an operations review. The
per-service message counters (bytes 24..32) are `u8` because they *are*
the on-wire mod-2⁸ counters — reporting them wider would misrepresent
the wire.

### Note — PUS-5 counter sub-block (bytes 28..31)

These reflect the framework PUS-5 counters. **As of slice fsw-8 the TC
router owns the PUS-5 context**, so *both* the spontaneous periodic
report and a TC[3,27]-polled report carry the **live** values — they
are now identical here. The fsw-7 asymmetry (a polled report carried
`00 00 00 00` because the router did not own the PUS-5 context) is
**resolved**: the router gained a PUS-5 producer (FDIR anomaly events),
exactly the condition this note previously pinned as the trigger. See
the *Versioning* section for the breaking-change classification of
this change.

## TC[3,27] — generate a one-shot report

Total packet size: **15 bytes** — primary 6 + TC sec 5 + application
data 2 (the SID) + CRC 2. Packet Data Length = `5 + 2 + 2 − 1 = 8`.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    1C 00 Cx xx 00 08   primary hdr (TC, APID 0x100, data length = 8)
6       2A           PUS-C ver (2) + ack flags (bits, here accept|complete)
7       03           Service Type = 3
8       1B           Subtype = 27 (generate one-shot report)
9..10   II II        Source ID, big-endian
11..12  00 01        Structure ID = 0x0001, big-endian (application data)
13..14  ?? ??        CRC-16-CCITT-FALSE over bytes 0..12
```

An application-data length other than exactly 2, or a SID that is not a
defined structure, is an **execution-stage** failure: the TC is
*accepted* (PUS-1[1] if requested) and then the completion stage fails
with `UNKNOWN_SUBTYPE` (PUS-1[8]) — no PUS-3[25] is emitted. This
matches the existing PUS-17 unknown-subtype model exactly.

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

The parameter block (bytes 16..44) is a fixed layout: **widening or
narrowing it, or reordering fields, is breaking**. Adding a new
framework structure under a new SID, or adding the deferred subtypes
with their own source data, is non-breaking provided the existing
structure and the rules above are unchanged.

### Breaking change in slice fsw-8 — bytes 28..31 on the polled path

The byte *layout* is unchanged, but the *value semantic* of bytes
28..31 (the PUS-5 counter sub-block) on a **TC[3,27]-polled** report
changed from a pinned constant `00 00 00 00` to the live PUS-5
counters. By the rule above this is a **breaking change** (a
value-semantic change to an emitted field), and it is recorded in
`CHANGELOG.md` under *Changed (breaking)*. It is the *planned,
pre-blessed* resolution of the asymmetry this document deliberately
pinned in fsw-7 ("it disappears when the router gains a PUS-5
producer"), not an accidental drift. The project is pre-1.0 (SemVer
0.y.z), so no major-version bump is required, but any ground decoder
that special-cased "a polled housekeeping report ⇒ bytes 28..31 are
zero" must be updated to read them as live counters (identical to the
spontaneous report).
