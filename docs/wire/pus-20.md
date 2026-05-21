# PUS-20 on-board parameter management — wire format

Authoritative byte-level specification for **PUS-20 — On-board
parameter management** in the Migris flight-software framework. Pinned
by slice fsw-9. This document **inherits** the CCSDS primary header,
UART framing, CRC, and PUS-C TC/TM secondary headers pinned in
[`pus-17.md`](pus-17.md) — only the PUS-20-specific surface (subtypes,
application data, source data, value encoding, emission rules) is
specified here. Changes to either document are breaking changes to the
platform's TM/TC interface and must be versioned accordingly.

Standards reference:

- **CCSDS 133.0-B-2** — Space Packet Protocol.
- **ECSS-E-ST-70-41C** — PUS-C. PUS-20 is §8.20 (on-board parameter
  management). Migris implements the standard service's complete
  three-message core; the parameter set is fixed in the MIB.

## Scope of this slice

PUS-20's standard service is exactly three messages — slice fsw-9
ships **all three**. There is no create / delete: the set of
parameters is fixed (the on-board datapool, `lib/datapool/`), defined
by the application at start-up.

| Subtype | Message                          | Dir | In fsw-9 |
|---------|----------------------------------|-----|----------|
| 1       | Report parameter values          | TC  | ✅       |
| 2       | Parameter value report           | TM  | ✅       |
| 3       | Set parameter values             | TC  | ✅       |

Parameter-definition reporting and any vendor-extension subtypes are
out of scope — no driving use case yet (workspace `CLAUDE.md`).

## Pinned platform decisions (additional to pus-17.md)

| Decision                | Value                                                       |
|-------------------------|-------------------------------------------------------------|
| Service Type            | `20`                                                        |
| Parameter ID            | 2 bytes, **big-endian** — the addressable unit               |
| Parameter count `N`     | 1 byte, first field of every application / source data; `0` .. `MIGRIS_PUS20_MAX_PARAMS_PER_TC` (default `8`) |
| Value encoding          | Per the parameter's registered type — see *Value encoding*  |
| TM sequence count       | Shared per-APID across **all** services (CCSDS 133.0-B-2: one count space per APID per direction) — not per-service |
| Message Counter         | Per (service, subtype) — the [20,2] report has its own      |
| Destination ID          | Echoes the triggering TC's source ID                         |
| Param-ID range — framework | `0x0001`–`0x00FF` reserved for **fsw-core** parameters    |
| Param-ID range — mission   | `0x0100`+ owned by mission FSW (`cry4-fsw`); scheme pinned when that repo bootstraps |

This mirrors the PUS-3 SID and PUS-5 event-ID block splits and the
pinned "PUS-128+ vendor assignments live in `cry4` / `cry4-fsw`, not
in `fsw-core`" decision. fsw-core itself hard-codes **no** parameters:
the datapool is a generic store and the application supplies the
parameter set (the `tc_uart` sample registers a framework-range
parameter to demonstrate the service — see `samples/tc_uart`).

## Value encoding

A parameter value is encoded at the fixed width its **registered
type** dictates. Integers are big-endian two's-complement; `f32` is
IEEE-754 single precision, big-endian.

| Type code | Name  | Wire width | Encoding                          |
|-----------|-------|------------|-----------------------------------|
| 0         | `u8`  | 1 byte     | unsigned                          |
| 1         | `u16` | 2 bytes    | unsigned, big-endian              |
| 2         | `u32` | 4 bytes    | unsigned, big-endian              |
| 3         | `i8`  | 1 byte     | two's-complement                  |
| 4         | `i16` | 2 bytes    | two's-complement, big-endian      |
| 5         | `i32` | 4 bytes    | two's-complement, big-endian      |
| 6         | `f32` | 4 bytes    | IEEE-754 single, big-endian       |

The type code is **not carried in PUS-20 packets**. A packet carries
only `(parameter ID, value bytes)` pairs; the value width is implied
by the parameter's type. Both ends therefore need the MIB (the
id → type map) to parse a packet — PUS-20 packets are deliberately
not self-describing (a self-describing width byte would bloat every
packet for information the MIB already holds).

The type-code column is the `migris_dp_type_t` enumerator value. The
enum is **append-only**: a new type takes the next free code and never
renumbers an existing one, so widening the type set later (e.g. 64-bit
or double-precision types) is a non-breaking change — existing
parameters encode identically.

## Subtype [20,1] — report parameter values (TC)

Application data (after the 5-byte PUS-C TC secondary header, before
the 2-byte CRC):

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       NN           N = parameter count
1..2    II II        parameter ID #1 (u16, big-endian)
...                  ... repeated N times ...
                     application data length = 1 + 2*N
```

The FSW resolves every ID and emits one [20,2] report. If any ID is
not a defined parameter the request fails — see *Emission and failure
rules*.

## Subtype [20,2] — parameter value report (TM)

Source data (after the 10-byte PUS-C TM secondary header, before the
2-byte CRC):

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       NN           N = parameter count (echoes the request)
        II II        parameter ID (u16, big-endian)
        VV..VV       value, 1/2/4 bytes per the parameter's type
...                  ... the (ID, value) pair repeated N times ...
                     source data length = 1 + sum(2 + width_i)
```

The report lists the parameters in the order the request named them.
Worst case is `N` 4-byte values: source data `1 + 6*N`, total packet
`6 + 10 + (1 + 6*N) + 2 = 19 + 6*N` bytes
(`MIGRIS_PUS20_TM_MAX_PACKET_SIZE`).

## Subtype [20,3] — set parameter values (TC)

Application data:

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0       NN           N = parameter count
        II II        parameter ID (u16, big-endian)
        VV..VV       value, 1/2/4 bytes per the parameter's type
...                  ... the (ID, value) pair repeated N times ...
                     application data length = 1 + sum(2 + width_i)
```

A [20,3] emits **no** telemetry of its own; the PUS-1 completion
report (if the TC requested it) is the operator's confirmation.

The set is **all-or-nothing**: the FSW decodes and fully validates
every pair — each ID is defined, is read-write, and its value bytes
are present — before applying any write. Any failure leaves the
datapool **untouched**.

A [20,3] **may name the same parameter ID more than once**. That is
permitted: validation passes for every entry and the writes apply in
order, so the **last value wins**.

> **Note for ground tooling.** Because a value's width depends on the
> parameter's type, an unknown ID makes every byte after it
> unparseable — the FSW aborts the walk at the first unknown ID.
> Ground tooling must build a [20,3] from defined parameters only.

## Emission and failure rules

A PUS-20 TC is validated by the generic accept stage first (CCSDS
framing, length, CRC, PUS-C version, routable service — see
[`pus-1.md`](pus-1.md)). Past acceptance, every PUS-20-specific
problem is an **execution-stage** failure: the TC is *accepted*
(PUS-1[1] if requested), then its completion stage fails with
**PUS-1[8]**, failure code `EXEC_FAILURE` — and no [20,2] report is
emitted, and for a [20,3] no datapool write occurs. The execution
failures are:

- application data malformed (missing count byte, declared count
  inconsistent with the byte count, value bytes truncated, trailing
  bytes unaccounted for);
- parameter count over `MIGRIS_PUS20_MAX_PARAMS_PER_TC`;
- a referenced parameter ID is not defined;
- a [20,3] names a read-only parameter;
- no datapool is wired on the application process.

An unsupported subtype (anything other than [1] or [3] inbound) is
the one PUS-20 failure mapped to PUS-1[8] `UNKNOWN_SUBTYPE` rather
than `EXEC_FAILURE`, consistent with PUS-17 and PUS-3.

## Worked example — report one u32 parameter

A [20,1] requesting parameter `0x0001` on APID `0x100`, total 16
bytes (primary 6 + TC sec 5 + app data 3 + CRC 2):

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    19 00 C0 SS 00 09   primary hdr: TC, APID 0x100, data length 9
6       2N           PUS-C ver (2) + ack flags (N)
7       14           Service Type = 20
8       01           Subtype = 1 (report parameter values)
9..10   DD DD        Source ID
11      01           N = 1
12..13  00 01        parameter ID 0x0001
14..15  ?? ??        CRC-16-CCITT-FALSE over bytes 0..13
```

The resulting [20,2] report, for `0x0001` registered as `u32` with
value `0x0AABBCCD`, total 25 bytes (primary 6 + TM sec 10 + source 7 +
CRC 2):

```
offset  bytes        meaning
------  -----------  ----------------------------------------
0..5    09 00 C0 NN 00 12   primary hdr: TM, APID 0x100, data length 18
6       20           PUS-C ver (2) + time ref status (0)
7       14           Service Type = 20
8       02           Subtype = 2 (parameter value report)
9       MM           Message Counter
10..11  DD DD        Destination ID = the request's Source ID
12..15  TT TT TT TT  CUC coarse seconds, big-endian
16      01           N = 1
17..18  00 01        parameter ID 0x0001
19..22  0A AB BC CD  value, u32 big-endian
23..24  ?? ??        CRC-16-CCITT-FALSE over bytes 0..22
```

## Versioning of this document

This file specifies wire-visible structure. Any change to a byte
layout, field width, value semantic, or emission rule above is a
**breaking change** to the platform's TM/TC interface and requires:

1. A new major version of `migris-fsw-core`.
2. A note in `CHANGELOG.md` *Changed (breaking)*.
3. A coordinated bump of any downstream consumer (mission FSW, MCS,
   ground segment) that hard-codes the old layout.

**Appending** a value type (the next free `migris_dp_type_t` code) is
non-breaking — existing parameters encode identically. **Widening**
`MIGRIS_PUS20_MAX_PARAMS_PER_TC` is non-breaking; **narrowing** it is
breaking. Adding a parameter ID within the reserved framework range is
non-breaking. Renumbering a value type, or changing a type's wire
width or encoding, is breaking.
