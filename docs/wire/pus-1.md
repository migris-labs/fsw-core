# PUS-1 telecommand verification — wire format

Authoritative byte-level specification for **PUS-1 — Telecommand
verification** in the Migris flight-software framework. Pinned by
slice fsw-5. This document **inherits** the CCSDS primary header,
UART framing, CRC, and PUS-C TM secondary header pinned in
[`pus-17.md`](pus-17.md) — only the PUS-1-specific surface (subtypes,
source data, verification rules) is specified here. Changes to either
document are breaking changes to the platform's TM/TC interface and
must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-1 is §8.1 (request verification).
  Migris implements a pragmatic subset (workspace `CLAUDE.md`); this
  document specifies exactly the surface needed.

## Scope of this slice

PUS-1 has four verification stages. Slice fsw-5 ships **acceptance**
and **completion**; **start** and **progress** are deliberately
deferred until a long-running command exists to exercise them
(workspace `CLAUDE.md`, `CHANGELOG.md`).

| Subtype | Report                                  | In fsw-5 |
|---------|-----------------------------------------|----------|
| 1       | Successful acceptance verification      | ✅       |
| 2       | Failed acceptance verification          | ✅       |
| 3 / 4   | Successful / failed start of execution  | deferred |
| 5 / 6   | Successful / failed progress of exec.   | deferred |
| 7       | Successful completion of execution      | ✅       |
| 8       | Failed completion of execution          | ✅       |

## Pinned platform decisions (additional to pus-17.md)

| Decision                       | Value                                          |
|--------------------------------|------------------------------------------------|
| Service Type                   | `1`                                            |
| Request ID                     | The verified TC's CCSDS Packet ID + Packet Sequence Control — i.e. the **first 4 bytes of the TC primary header**, copied verbatim |
| Failure code field             | 1 byte, present **only** in failure reports ([2]/[8]) |
| TM sequence count              | Shared per-APID across **all** services (CCSDS 133.0-B-2: one count space per APID per direction) — not per-service |
| Destination ID                 | Echoes the triggering TC's Source ID (same convention as PUS-17) |

PUS-1 has **no inbound TC** of its own: reports are emitted as a side
effect of processing another service's TC.

## PUS-1 is gated by the TC's ack flags

The PUS-C TC secondary header carries a 4-bit ack field (see
`pus-17.md`). A verification report is emitted **only if the
triggering TC requested that stage**:

| Ack bit (mask)        | Enables          |
|-----------------------|------------------|
| `ACK_ACCEPTANCE` 0x1  | PUS-1[1] / [2]   |
| `ACK_COMPLETION` 0x8  | PUS-1[7] / [8]   |
| `ACK_START`     0x2   | (deferred)       |
| `ACK_PROGRESS`  0x4   | (deferred)       |

Rules pinned by slice fsw-5:

1. **Not addressed to this AP.** A packet that is not a well-formed
   TC for this application process (bad primary header, wrong APID)
   produces **no output at all** — it may be line noise or a TM
   loopback, and unsolicited TM is worse than silence.
2. **Length error is always reported.** A TC addressed to this AP
   whose declared CCSDS length disagrees with the bytes received
   cannot have its ack flags trusted, so a PUS-1[2] with failure code
   `LENGTH_ERROR` is emitted **regardless** of ack flags. No routing,
   no completion.
3. **Other acceptance failure** (CRC, PUS version, unknown service):
   the ack flags *are* read from the packet (even on CRC failure — we
   surface the requested verification); a PUS-1[2] is emitted iff
   `ACK_ACCEPTANCE` is set. The TC is **not** routed and there is
   **no** completion report.
4. **Accepted.** If `ACK_ACCEPTANCE` is set, a PUS-1[1] is emitted.
   The TC is routed to its service. After execution, if
   `ACK_COMPLETION` is set, a PUS-1[7] (service reported success) or
   PUS-1[8] (service reported failure) is emitted.

Acceptance covers structural validity **and** that the service type
is routable. Subtype validity is an *execution* concern: a known
service that does not implement the requested subtype is **accepted**
(PUS-1[1]) and then fails completion (PUS-1[8], `UNKNOWN_SUBTYPE`).

One inbound TC therefore yields 0–3 packets, written back-to-back on
the wire (e.g. `PUS-1[1] · PUS-17[2] · PUS-1[7]`). The shared per-APID
sequence count makes the burst strictly monotonic across services.

## Failure codes

Serialised as a single byte in the source data of a failure report.
Acceptance-stage codes never appear in a completion report and vice
versa.

| Code | Name             | Stage      | Meaning                                   |
|------|------------------|------------|-------------------------------------------|
| 0    | `NONE`           | —          | Never on the wire (success carries none)  |
| 1    | `BAD_PRIMARY`    | acceptance | CCSDS primary header malformed            |
| 2    | `ILLEGAL_APID`   | acceptance | APID is not this application process      |
| 3    | `LENGTH_ERROR`   | acceptance | Declared length / packet size mismatch    |
| 4    | `CRC_FAILURE`    | acceptance | Packet error-control CRC mismatch         |
| 5    | `BAD_PUS_VERSION`| acceptance | TC secondary header PUS version not C     |
| 6    | `UNKNOWN_SERVICE`| acceptance | Service type not routable on this AP      |
| 7    | `UNKNOWN_SUBTYPE`| completion | Known service, unsupported subtype        |
| 8    | `EXEC_FAILURE`   | completion | Routed handler ran but reported failure   |

Codes 1 (`BAD_PRIMARY`) and 2 (`ILLEGAL_APID`) are listed for
completeness: in the single-AP model they manifest as *silence*
(rule 1), not a report.

## Source data

Both report flavours carry the **request ID** = the first 4 bytes of
the verified TC's primary header (CCSDS Packet ID + Packet Sequence
Control), copied verbatim. A failure report appends one **failure
code** byte.

```
success ([1]/[7]):  | request ID (4) |
failure ([2]/[8]):  | request ID (4) | failure code (1) |
```

## PUS-1[1] — successful acceptance verification report

Total packet size: **22 bytes** (primary 6 + TM sec 10 + request ID 4
+ CRC 2). Triggered here by a PUS-17[1] TC on APID `0x100`, seq 0,
source ID `0x0000`.

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    09 00        primary hdr [0..1]: ver=0, T=0 (TM), S=1, APID=0x100
2..3    C0 NN        primary hdr [2..3]: SeqF=11, Count (shared, FSW-assigned)
4..5    00 0F        Packet Data Length = 16−1 = 15
6       20           PUS-C ver (2) + spacecraft time ref status (0)
7       01           Service Type = 1
8       01           Subtype = 1 (successful acceptance)
9       MM           Message Counter (per service+subtype)
10..11  00 00        Destination ID (echoes TC source ID)
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
16..19  19 00 C0 00  Request ID = verified TC primary hdr [0..3]
20..21  ?? ??        CRC-16-CCITT-FALSE over bytes 0..19
```

## PUS-1[2] — failed acceptance verification report

Total packet size: **23 bytes** (the [1] layout + 1 failure-code
byte). Example: the same TC rejected for `CRC_FAILURE` (code 4).

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..1    09 00        primary hdr [0..1]: ver=0, T=0 (TM), S=1, APID=0x100
2..3    C0 NN        primary hdr [2..3]: SeqF=11, Count
4..5    00 10        Packet Data Length = 17−1 = 16
6       20           PUS-C ver (2) + time ref status (0)
7       01           Service Type = 1
8       02           Subtype = 2 (failed acceptance)
9       MM           Message Counter
10..11  SS SS        Destination ID (echoes TC source ID; 0 on length error)
12..15  TT TT TT TT  CUC coarse seconds since boot, big-endian
16..19  RR RR RR RR  Request ID = verified TC primary hdr [0..3]
20      04           Failure code (CRC_FAILURE)
21..22  ?? ??        CRC-16-CCITT-FALSE over bytes 0..20
```

## PUS-1[7] / PUS-1[8] — completion reports

Byte-identical to PUS-1[1] / PUS-1[2] respectively, with **Subtype**
= `07` (successful completion) or `08` (failed completion). [8]
carries a completion-stage failure code (`UNKNOWN_SUBTYPE` or
`EXEC_FAILURE`).

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or verification rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

Adding deferred subtypes ([3]–[6]) with their own source data is
non-breaking provided existing subtypes and the rules above are
unchanged.
