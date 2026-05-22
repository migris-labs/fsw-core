# PUS-3 housekeeping & diagnostic data reporting — wire format

Authoritative byte-level specification for **PUS-3 — Housekeeping &
diagnostic data reporting** in the Migris flight-software framework.
Pinned by slice fsw-7; extended by slice fsw-15. This document **inherits** the CCSDS primary
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

## Scope

PUS-3 has a large structure-management surface. Slice fsw-7 shipped the
**spontaneous + polled report path** against a single predefined
framework structure; slice fsw-15 adds **ground-defined (dynamic)
housekeeping structures** and the structure-management subtypes that
create, delete, enable and disable them.

| Subtype | Direction | Meaning                                         | Status   |
|---------|-----------|-------------------------------------------------|----------|
| 25      | TM        | Housekeeping parameter report                   | fsw-7    |
| 27      | TC        | Generate a one-shot housekeeping report         | fsw-7    |
| 1 / 2   | TC        | Create / delete a housekeeping structure        | fsw-15   |
| 5 / 6   | TC        | Enable / disable a structure's periodic report  | fsw-15   |
| 3 / 4   | TC        | Create / delete a diagnostic structure          | deferred |
| 9 / 10  | TC / TM   | Report a housekeeping structure's definition    | deferred |
| 7 / 8   | TC        | Append to / clear a super-commutated group      | deferred |
| 26      | TM        | Diagnostic parameter report                     | deferred |

A **dynamic** structure selects a list of parameters from the on-board
parameter datapool ([`pus-20.md`](pus-20.md)); its [25] report
serialises those parameters' values in order. The predefined framework
structure FRAMEWORK_DIAG (SID `0x0001`) keeps its frozen layout
unchanged — fsw-15 is **purely additive and non-breaking**.

Still deferred: the diagnostic-structure subtypes [3]/[4] and the
diagnostic report [26] (a separate report stream — no driving use case
yet); structure-definition reporting [9]/[10] (ground reading back a
structure's parameter list — no in-platform consumer until the MCS);
super-commutated parameter groups [7]/[8]; in-place modification of an
existing structure; create-time validation of parameter IDs against the
datapool (a structure naming an absent parameter is caught at emission
time instead — see below).

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

A [3,27] poll resolves its SID two ways: `0x0001` selects the frozen
framework structure (the report above); any other SID is looked up in
the dynamic housekeeping-structure store (slice fsw-15). A SID found
there is reported with the dynamic [25] layout below; a SID found in
neither is the unknown-structure case above (`UNKNOWN_SUBTYPE`).

## PUS-3[25] — dynamic-structure housekeeping parameter report

A [25] report for a ground-created (dynamic) structure has the same
packet shape as the framework report but a **variable-length,
structure-defined** source data.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    09 00 Cx xx LL LL   primary hdr (TM, APID 0x100, len from data field)
6       20           PUS-C ver (2) + spacecraft time ref status (0)
7       03           Service Type = 3
8       19           Subtype = 25 (housekeeping parameter report)
9       MM           Message Counter (shared with the framework [25])
10..11  DD DD        Destination ID — 0 spontaneous, else poll source ID
12..15  TT TT TT TT  CUC coarse seconds, big-endian
                     --- source data (variable) ---
16..17  SS SS        Structure ID (the dynamic SID, >= 0x0100), big-endian
18..    VV ...       each parameter's value, in the order the structure
                     names them — MIB-decoded, big-endian, at the on-wire
                     width its datapool type dictates (1, 2 or 4 bytes)
N..N+1  ?? ??        CRC-16-CCITT-FALSE over every preceding byte
```

The parameter values are **not self-describing** — there is no type or
ID tag on the wire. Ground decodes them from the structure definition
(the parameter ID list supplied at [3,1] create) and the MIB
(ID → type), exactly as for any PUS-3 housekeeping structure. A
structure of *N* four-byte parameters is a `6 + 10 + (2 + 4·N) + 2`
byte packet; the framework caps *N* at `MIGRIS_HKSTORE_MAX_PARAMS`.

If the structure names a parameter the datapool does not define, the
**whole report fails** — no partial packet is emitted, and the packet
length stays deterministic. On the [3,27]-polled path this is a
completion-stage `FC_EXEC_FAILURE`.

The `[25]` message counter is **shared** between the framework and
dynamic reports — they are the same (service, subtype). The framework
FRAMEWORK_DIAG report (SID `0x0001`) is unaffected by this section: its
27-byte fixed parameter block stays exactly as specified above.

## TC[3,1] — create a housekeeping structure

Total packet size: **17 + 2·N bytes** for an *N*-parameter structure —
primary 6 + TC sec 5 + application data `7 + 2·N` + CRC 2.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    1C 00 Cx xx LL LL   primary hdr (TC, APID 0x100)
6       2A           PUS-C ver (2) + ack flags
7       03           Service Type = 3
8       01           Subtype = 1 (create a housekeeping structure)
9..10   II II        Source ID, big-endian
                     --- application data (7 + 2·N bytes) ---
11..12  SS SS        Structure ID (>= 0x0100), big-endian
13      NN           parameter count N (1 .. MIGRIS_HKSTORE_MAX_PARAMS)
14..    PP PP ...    N parameter IDs, 2 bytes each, big-endian
14+2N.. JJ JJ JJ JJ  reporting interval, seconds (u32, BE); 0 = poll-only
...     ?? ??        CRC-16-CCITT-FALSE
```

The structure is created **disabled** — ground enables it with a [3,5].
It does not emit a periodic report until it is *both* enabled and its
interval has elapsed; an interval of `0` means it is never reported
periodically (it can still be polled with a [3,27]).

The Structure ID **must be `0x0100` or above** — the `0x0001..0x00FF`
block is reserved for fsw-core framework structures and a create naming
one is rejected. A create is likewise rejected for a SID that already
defines a structure, a parameter count of 0 or above
`MIGRIS_HKSTORE_MAX_PARAMS`, or a full structure store.

Parameter IDs are **not** validated against the datapool at create
time — a structure may name a parameter not (yet) defined. That is
caught when a [25] report is built (see the dynamic report above).

## TC[3,2] / [3,5] / [3,6] — delete / enable / disable a structure

Application data is exactly one Structure ID (2 bytes, big-endian) —
the structure to delete ([3,2]), enable ([3,5]) or disable ([3,6]).
Total packet size: **15 bytes** (primary 6 + TC sec 5 + SID 2 + CRC 2),
identical in shape to a [3,27] poll. A SID that names no structure is
an execution failure.

## Structure-management failure model

The four structure-management subtypes ([3,1]/[3,2]/[3,5]/[3,6])
produce **no telemetry of their own** — only the PUS-1 verification the
telecommand requested. A telecommand that is *accepted* but whose
*execution* fails — malformed application data, an unknown / duplicate /
framework-range SID, a parameter list too long, a full store, or no
structure store wired on this application process — yields a PUS-1[8]
completion failure with `FC_EXEC_FAILURE`.

This differs deliberately from the [3,27] poll, whose unknown-SID and
bad-length failures map to `FC_UNKNOWN_SUBTYPE` (the fsw-7 contract:
on the poll path the Structure ID space is the addressable unit). A
structure-management subtype is a *known, valid* subtype whose
*execution* failed — hence `FC_EXEC_FAILURE`, consistent with the
PUS-11 / PUS-15 / PUS-20 routed-service model.

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

### Non-breaking additions in slice fsw-15 — structure management

Slice fsw-15 adds the structure-management subtypes [3,1]/[3,2]/[3,5]/
[3,6], the dynamic [25] report, and the dynamic-SID resolution on the
[3,27] poll. Every addition is **non-breaking** by the rule above: the
frozen FRAMEWORK_DIAG [25] report (SID `0x0001`) — its 47-byte packet
and 27-byte parameter block — is byte-for-byte unchanged, the [3,27]
poll of SID `0x0001` is unchanged, and the new subtypes occupy
previously unused subtype numbers. Recorded in `CHANGELOG.md` under
*Added*, not *Changed (breaking)*.
