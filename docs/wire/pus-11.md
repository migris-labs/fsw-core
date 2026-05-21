# PUS-11 on-board scheduling — wire format

Authoritative byte-level specification for **PUS-11 — On-board
(time-based) scheduling** in the Migris flight-software framework.
Pinned by slice fsw-10. This document **inherits** the CCSDS primary
header, UART framing, CRC, and PUS-C TC/TM secondary headers pinned in
[`pus-17.md`](pus-17.md) — only the PUS-11-specific surface (subtypes,
application data, source data, the request identifier, release rules)
is specified here. Changes to either document are breaking changes to
the platform's TM/TC interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-11 is §8.11 (on-board
  scheduling). Migris implements a pragmatic core subset.

## Scope of this slice

A scheduled **activity** is one telecommand plus an absolute release
time; at release time the FSW hands the stored TC back to the TC
router for normal dispatch. Slice fsw-10 ships the core subset:

| Subtype | Message                                    | Dir | In fsw-10 |
|---------|--------------------------------------------|-----|-----------|
| 1       | Enable the schedule execution function     | TC  | ✅        |
| 2       | Disable the schedule execution function    | TC  | ✅        |
| 3       | Reset the schedule (delete all activities) | TC  | ✅        |
| 4       | Insert activities into the schedule        | TC  | ✅        |
| 5       | Delete activities, by request identifier   | TC  | ✅        |
| 11      | Summary-report activities, by request id   | TC  | ✅        |
| 12      | Time-based schedule summary report         | TM  | ✅        |

Deliberately deferred: the **detail** report (ECSS 11,9/11,10) echoes
each activity's telecommand verbatim, so its packet size is unbounded
— that is "large data" and pairs with PUS-13; the summary report
(11,12) carries only release time + request identifier per activity
and is bounded. Time-shift (11,7/8/15), sub-schedules, groups, and
filter-based selection are also out of this slice.

## Pinned platform decisions (additional to pus-17.md)

| Decision                | Value                                                       |
|-------------------------|-------------------------------------------------------------|
| Service Type            | `11`                                                        |
| Activity count `N`      | 1 byte, first field of every insert / delete / report application data; `0` .. `MIGRIS_PUS11_MAX_PER_TC` (default `8`) |
| Release time            | 4 bytes, **big-endian**, absolute CUC coarse seconds         |
| Request identifier      | 4 bytes — the first four bytes of a telecommand (the CCSDS packet identification + packet sequence control), identical to the PUS-1 request identifier |
| Schedule enabled state  | Starts **disabled** — a freshly booted FSW does not autonomously fire a stale schedule until ground sends [11,1] |
| TM sequence count       | Shared per-APID across **all** services — not per-service    |
| Message Counter         | Per (service, subtype) — the [12] report has its own         |
| Destination ID          | Echoes the triggering TC's source ID                         |

The request identifier uniquely identifies a scheduled activity:
ground assigns the sequence count of every telecommand it schedules,
so it owns and knows each identifier. The schedule keeps them unique.

## The release mechanism

The schedule is a store; releasing is driven by the application. Each
main-loop iteration, while the schedule is enabled, the FSW releases
the **one** activity with the earliest release time whose release time
has been reached (`release_time <= now`), removing it from the
schedule and dispatching its stored telecommand through the TC router
exactly as if it had just arrived — including its own PUS-1
verification, gated by the released TC's ack flags. A release time in
the past (e.g. already due when inserted) is released on the next
iteration. While the schedule is disabled, nothing is released;
activities are retained.

## Subtypes [11,1] / [11,2] / [11,3] — enable / disable / reset (TC)

No application data (application-data length 0). [11,1] enables and
[11,2] disables release; [11,3] deletes every activity (the
enabled/disabled state is unchanged — that is owned by [11,1]/[11,2]).
None emits telemetry; the PUS-1 completion report, if requested, is
the confirmation.

## Subtype [11,4] — insert activities (TC)

Application data:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       NN           N = activity count
        TT TT TT TT  release time (u32, big-endian, absolute CUC)
        PP..PP       the telecommand — a complete CCSDS Space Packet,
                     self-delimiting via its own Packet Data Length
...                  ... the (release time, telecommand) pair N times ...
```

Each embedded telecommand is stored verbatim and is **not** validated
at insert time beyond its declared length fitting — semantic
validation happens at release, when the router dispatches it. An
embedded telecommand longer than the per-activity limit
(`MIGRIS_SCHEDULE_TC_MAX`, default 64 bytes) is rejected.

## Subtype [11,5] — delete activities (TC)

Application data: a 1-byte count followed by that many 4-byte request
identifiers.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       NN           N = activity count
1..4    RR RR RR RR  request identifier #1
...                  ... repeated N times ...
                     application data length = 1 + 4*N
```

## Subtype [11,11] — summary-report activities (TC)

Application data is byte-identical to [11,5] — a 1-byte count followed
by that many 4-byte request identifiers. The FSW replies with one
[11,12] summary report.

## Subtype [11,12] — time-based schedule summary report (TM)

Source data:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       MM           M = number of activities reported
        TT TT TT TT  release time (u32, big-endian)
        RR RR RR RR  request identifier
...                  ... the (release time, request id) pair M times ...
                     source data length = 1 + 8*M
```

`M` is the number of *requested* identifiers that are currently
scheduled — a [11,11] request may name an identifier that is not
scheduled, and it is simply omitted (`M <= N`). Worst-case packet:
`6 + 10 + (1 + 8*N) + 2 = 19 + 8*N` bytes.

## Emission and failure rules

A PUS-11 TC is validated by the generic accept stage first (CCSDS
framing, length, CRC, PUS-C version, routable service). Past
acceptance, every PUS-11-specific problem is an **execution-stage**
failure: the TC is *accepted* (PUS-1[1] if requested), then its
completion stage fails with **PUS-1[8]**, failure code
`EXEC_FAILURE`. The execution failures are:

- application data malformed (a [1]/[2]/[3] carrying any data; a
  declared count inconsistent with the byte count; an embedded
  telecommand whose declared length overruns the application data);
- activity count over `MIGRIS_PUS11_MAX_PER_TC`;
- an embedded telecommand over the per-activity size limit;
- an insert whose request identifier is already scheduled, or
  repeated within the same insert;
- an insert that would exceed the schedule capacity;
- a delete naming a request identifier that is not scheduled;
- no schedule is wired on the application process.

An unsupported subtype (anything other than [1], [2], [3], [4], [5]
or [11] inbound) is the one PUS-11 failure mapped to PUS-1[8]
`UNKNOWN_SUBTYPE` rather than `EXEC_FAILURE`.

**Insert ([4]) and delete ([5]) are all-or-nothing**: every item is
decoded and fully validated before any change to the schedule, so a
failed request leaves the schedule exactly as it was. The summary
report ([11]) is a query — a not-scheduled identifier is omitted from
the report, not an error.

## Worked example — insert one activity

A [11,4] inserting one activity — a 13-byte PUS-17[1] telecommand
released at absolute CUC time `0x00010000` — on APID `0x100`. The
application data is `1 + (4 + 13) = 18` bytes; the whole TC is
`6 + 5 + 18 + 2 = 31` bytes:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    19 00 C0 SS 00 18   primary hdr: TC, APID 0x100, data length 24
6       2N           PUS-C ver (2) + ack flags (N)
7       0B           Service Type = 11
8       04           Subtype = 4 (insert activities)
9..10   DD DD        Source ID
11      01           N = 1
12..15  00 01 00 00  release time = 0x00010000, big-endian
16..28  ..13 bytes.. the embedded PUS-17[1] telecommand, verbatim
29..30  ?? ??        CRC-16-CCITT-FALSE over bytes 0..28
```

Bytes 16..19 of the embedded telecommand are its request identifier
— the value ground later uses in a [11,5] delete or a [11,11] report.

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

**Widening** `MIGRIS_PUS11_MAX_PER_TC` is non-breaking; **narrowing**
it is breaking. Adding a deferred subtype (the detail report,
time-shift, sub-schedules, groups) with its own layout is
non-breaking provided the subtypes and rules above are unchanged.
