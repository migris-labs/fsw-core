# PUS-5 event reporting — wire format

Authoritative byte-level specification for **PUS-5 — Event reporting**
in the Migris flight-software framework. Pinned by slice fsw-6. This
document **inherits** the CCSDS primary header, UART framing, CRC, and
PUS-C TM secondary header pinned in [`pus-17.md`](pus-17.md) — only
the PUS-5-specific surface (subtypes, source data, emission rules) is
specified here. Changes to either document are breaking changes to the
platform's TM/TC interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-5 is §8.5 (event reporting).
  Migris implements a pragmatic subset (workspace `CLAUDE.md`); this
  document specifies exactly the surface needed.

## Scope of this slice

PUS-5 has four asynchronous event-report TM subtypes and four
TC-driven control subtypes. Slice fsw-6 ships **all four event-report
subtypes**; the control subtypes are deliberately excluded —
TC-driven event-generation reconfiguration overlaps PUS-20 (onboard
parameter management, P1) and there is no driving use case yet.

| Subtype | Report                          | In fsw-6 |
|---------|---------------------------------|----------|
| 1       | Informative event report        | ✅       |
| 2       | Low-severity anomaly report     | ✅       |
| 3       | Medium-severity anomaly report  | ✅       |
| 4       | High-severity anomaly report    | ✅       |
| 5 / 6   | Enable / disable event report   | excluded |
| 7       | Report disabled event defns (TC)| excluded |
| 8       | Disabled event definitions list | excluded |

Severity is carried entirely by the subtype: `subtype = severity + 1`
(INFO→[1], LOW→[2], MEDIUM→[3], HIGH→[4]).

## Pinned platform decisions (additional to pus-17.md)

| Decision                | Value                                                       |
|-------------------------|-------------------------------------------------------------|
| Service Type            | `5`                                                         |
| Event-definition ID     | 2 bytes, **big-endian**, first field of the source data     |
| Auxiliary data          | 0 .. `32` bytes, appended verbatim after the event ID       |
| TM sequence count       | Shared per-APID across **all** services (CCSDS 133.0-B-2: one count space per APID per direction) — not per-service |
| Message Counter          | Per (service, subtype) — i.e. one counter per severity      |
| Destination ID          | `0` for a spontaneous event (no triggering TC)              |
| Event-ID range — framework | `0x0001`–`0x00FF` reserved for **fsw-core** events       |
| Event-ID range — mission   | `0x0100`+ owned by mission FSW (`cry4-fsw`); scheme pinned when that repo bootstraps |

This mirrors the pinned "PUS-128+ vendor assignments live in `cry4` /
`cry4-fsw`, not in `fsw-core`" decision (workspace `CLAUDE.md`). Only
the framework event IDs fsw-core actually emits are defined; the
mission-side numbering is deferred to its first consumer.

### Defined framework event IDs

| Event ID | Name       | Severity | Meaning                          |
|----------|------------|----------|----------------------------------|
| `0x0001` | `FSW_BOOT` | info [1] | Flight software (re)started      |

## PUS-5 is asynchronous

PUS-5 has **no inbound TC** of its own (the control subtypes that
would carry one are excluded above). Unlike PUS-1 and PUS-17, a PUS-5
report is emitted **spontaneously** at the point a condition is
detected — it is *not* gated by any TC's ack flags and is *not* part
of a TC-triggered burst. The shared per-APID sequence count still
applies: a spontaneous event consumes the next count in the same
space as every other TM packet on that APID, keeping the wire
strictly monotonic across the boot event and all subsequent
verification / service TM.

The encoder is a pure serialiser: the caller decides an event has
fired, owns the event identity, and owns the output buffer. There is
deliberately **no event queue** in this slice — see the rationale in
`lib/pus/include/migris/fsw/pus/pus5.h` and `CHANGELOG.md`.

## Source data

```
all subtypes:  | event ID (2, big-endian) | auxiliary data (0..32) |
```

Auxiliary data is event-specific and opaque at this layer (its
meaning is defined per event ID by the emitting code / mission). A
bare event carries no auxiliary data.

## PUS-5[1] — informative event report (FSW boot)

Total packet size: **20 bytes** (primary 6 + TM sec 10 + event ID 2 +
CRC 2). The `tc_uart` sample emits exactly this on reset, on APID
`0x100`, sequence count 0 (the first TM the FSW produces).

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    09 00        primary hdr [0..1]: ver=0, T=0 (TM), S=1, APID=0x100
2..3    C0 00        primary hdr [2..3]: SeqF=11, Count=0 (shared, FSW-assigned)
4..5    00 0D        Packet Data Length = 14−1 = 13
6       20           PUS-C ver (2) + spacecraft time ref status (0)
7       05           Service Type = 5
8       01           Subtype = 1 (informative)
9       MM           Message Counter (per service+subtype)
10..11  00 00        Destination ID = 0 (spontaneous, no triggering TC)
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
16..17  00 01        Event ID = 0x0001 (FSW_BOOT), big-endian
18..19  ?? ??        CRC-16-CCITT-FALSE over bytes 0..17
```

## PUS-5[2..4] — anomaly reports

Byte-identical to PUS-5[1] with **Subtype** = `02` (low), `03`
(medium) or `04` (high), plus optional auxiliary data appended after
the event ID. Example: a low-severity anomaly, event ID `0x00AB`,
4 bytes of aux — total 24 bytes, Packet Data Length = `10 + 2 + 4 +
2 − 1 = 17`.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    09 00 C0 NN 00 11   primary hdr (data length = 17)
6       20           PUS-C ver (2) + time ref status (0)
7       05           Service Type = 5
8       02           Subtype = 2 (low-severity anomaly)
9       MM           Message Counter
10..11  DD DD        Destination ID
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
16..17  00 AB        Event ID, big-endian
18..21  AA AA AA AA  Auxiliary data (4 bytes, event-specific)
22..23  ?? ??        CRC-16-CCITT-FALSE over bytes 0..21
```

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

**Widening** `MIGRIS_PUS5_AUX_MAX_LEN` is non-breaking; **narrowing**
it is breaking. Adding framework event IDs within the reserved range,
or adding the excluded control subtypes ([5]–[8]) with their own
source data, is non-breaking provided the existing subtypes and the
rules above are unchanged.
