# Non-volatile storage — on-flash image format

Authoritative byte-level specification for the `lib/nvstore/`
flash-backed persistence image. Pinned by slice fsw-16.

Unlike `docs/wire/*.md`, this is **not a cross-repo wire contract** —
the image lives inside the spacecraft's own flash partition and is
read and written only by `fsw-core`. It still has to stay readable
across firmware versions, so the rules below are versioned and a
breaking change to the layout requires bumping `format_version`.

## Geometry

The store occupies **two flash sectors** of the board's
`storage_partition`. On `nucleo_h753zi` the Zephyr v3.7.1 default DTS
allocates only one 128 KB sector for `storage_partition` (the default
partition table reserves the other slots for MCUboot, image-0/1 and
image-scratch); a board overlay at
[`samples/tc_uart/boards/nucleo_h753zi.overlay`](../samples/tc_uart/boards/nucleo_h753zi.overlay)
replaces that table with a 256 KB `storage_partition` spanning the
last two sectors of flash bank 1 (offsets `0xC0000` and `0xE0000`,
absolute addresses `0x080C0000` and `0x080E0000`). The two sectors
are used as an **A/B ping-pong** pair; the store keeps exactly one
*image* per sector.

## A/B ping-pong

Each `migris_nvstore_save` writes the new image to the sector NOT
holding the currently-loaded copy:

```
loaded sector = i                   target sector = (i + 1) mod 2
loaded seq    = N                   new image seq = N + 1
```

If no valid image exists yet (first ever save), the target is sector 0
and the new sequence number is 1.

`migris_nvstore_load` reads both sectors, validates each (magic +
`format_version` + CRC), and picks the valid image with the higher
sequence number. A power loss mid-`save` leaves one intact older copy
in the OTHER sector; the next `load` recovers to it.

## Image layout

One image fits inside one flash sector. Layout, all big-endian:

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0..3    4      magic = 'M' 'N' 'V' '1'  (0x4D 0x4E 0x56 0x31)
4..5    2      format_version           (current = 1)
6..9    4      seq                      (sequence number)
10..11  2      payload_len              (bytes in the payload section)
                                        --- payload ---
12..    payload_len  records, concatenated (see below)
                                        --- CRC ---
12+L    2      CRC-16-CCITT-FALSE over bytes 0..(11+L) — header + payload
                                        --- padding ---
...     ...    `0xFF` bytes up to the next backend `write_block`
               multiple (32 bytes on STM32H7). The CRC is over the
               un-padded image; padding is alignment only.
```

The total *un-padded* image size is `MIGRIS_NVSTORE_HEADER_SIZE (12) +
payload_len + 2`. The image is padded to the backend's `write_block`
for the single contiguous flash write that `save` issues.

The CRC algorithm is the framework-wide CRC-16-CCITT-FALSE
(polynomial `0x1021`, init `0xFFFF`, no reflection, no XOR-out) — the
same algorithm the CCSDS Space Packet error control uses; see
`lib/pus/include/migris/fsw/pus/ccsds.h`.

## Records

The payload is a sequence of typed records, each:

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0       1      type   — a value from migris_nvstore_record_type_t
1..2    2      len    — record body length in bytes (big-endian)
3..     len    body   — record-specific bytes
```

Records are append-only (`migris_nvstore_put` removes any existing
record of the same type, then appends the new one — so the payload
holds at most one record per type). `migris_nvstore_get` returns the
first matching record's body bytes; unknown record types in a loaded
payload are silently ignored on read, so a structural rollback (a
firmware version that did not know about a later record type) is
non-fatal.

### Defined record types

| `type` | name       | body | source | added |
|--------|------------|------|--------|-------|
| `1`    | `DATAPOOL` | the serialised parameter datapool (see below) | `lib/datapool/` | fsw-16 |
| `2`    | `SCHEDULE` | the serialised on-board schedule (see below) | `lib/schedule/` | fsw-17 |
| `3`    | `HKSTORE`  | the serialised housekeeping-structure store (see below) | `lib/hkstore/` | fsw-17 |
| `4`    | `MODE`     | the current operating-mode ID (one byte, see below) | `lib/mode/` | fsw-17 |

### `DATAPOOL` record body

The datapool record body is the output of `migris_datapool_serialize`
(`lib/datapool/src/datapool.c`):

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0..1    2      count   — number of parameters that follow (big-endian)
                                    --- repeated `count` times ---
0       2      id      — parameter ID (big-endian)
2       1      type    — migris_dp_type_t code (1 byte)
3..     w      value   — `w` bytes, where `w` is the on-wire width of
                         the type (1/2/4) — byte-identical to the
                         parameter's PUS-20 value on the wire
```

The access policy (`migris_dp_access_t`) is **not** serialised — it is
a code-defined attribute, not operator state. On `deserialize`, each
record's value is restored only when both the parameter ID is present
in the running datapool AND the type matches; unknown ids and type
mismatches are silently skipped, so a parameter set that has evolved
across a firmware update degrades to "the changed parameters keep
their defaults" rather than failing the whole restore.

### `SCHEDULE` record body

The schedule record body is the output of `migris_schedule_serialize`
(`lib/schedule/src/schedule.c`):

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0..1    2      count        — number of activities that follow (big-endian)
2       1      enabled      — 0 or 1 (the operator-set state)
                                    --- repeated `count` times ---
0..3    4      release_time — absolute CUC coarse seconds (big-endian)
4..5    2      tc_len       — telecommand length in bytes (big-endian)
6..     tc_len tc           — the telecommand, verbatim
```

Variable-length per entry so the image stays proportional to actual
scheduled volume — a future `MIGRIS_SCHEDULE_TC_MAX` change does not
balloon the worst case for under-filled schedules. On `deserialize` a
`tc_len` over `MIGRIS_SCHEDULE_TC_MAX` (firmware downgrade) or below
the 4-byte request-id floor is rejected; the schedule is left empty
on any error (stateless failure). The `generation` counter is
**not** serialised — it is RAM-only, used by the application's
"have I changed since the last save?" loop.

### `HKSTORE` record body

The hkstore record body is the output of `migris_hkstore_serialize`
(`lib/hkstore/src/hkstore.c`):

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0..1    2      count        — number of structures that follow (big-endian)
                                    --- repeated `count` times ---
0..1    2      sid          — Structure ID, >= 0x0100 (big-endian)
2..5    4      interval_sec — reporting period, seconds (big-endian)
6       1      enabled      — 0 or 1
7       1      param_count  — 0..MIGRIS_HKSTORE_MAX_PARAMS
8..     2·N    param_ids    — datapool parameter IDs, each 2 BE
```

Only the in-use slots are written, packed back-to-back. The
`last_emit_sec` and `in_use` fields are **deliberately not
serialised**. `in_use` is an array-slot artefact — on `deserialize`
the restored structures compact into the low slots, so `in_use` is
implicitly 1 for every persisted entry. `last_emit_sec` is excluded
because the FSW clock resets to 0 on boot; a stale persisted value
would make `(now - last_emit_sec) >= interval_sec` arithmetic
underflow as `uint32_t` and the structure would fire on every tick
until the clock caught up. Restored structures re-arm with
`last_emit_sec = 0`, matching post-`create` behaviour. The SID floor
(`MIGRIS_HKSTORE_SID_MIN = 0x0100`) and the duplicate-SID guard
mirror `migris_hkstore_create` and are enforced on `deserialize`.

### `MODE` record body

The mode record body is the output of `migris_mode_serialize`
(`lib/mode/src/mode.c`):

```
offset  size   meaning
------  -----  ---------------------------------------------------------
0       1      current      — the active mode ID
```

One byte. The mode set, the allowed-target bitmasks and the optional
event sink are code-defined at every `migris_mode_init` and are not
persisted — a stale on-flash table would outlive a firmware change to
the rules. On `deserialize_current` the restored ID is validated
against the declared mode set; if it is unknown (firmware shipped a
new mode table and dropped the old mode), the init-time current mode
is silently kept and the call still returns `MIGRIS_MODE_OK`
(rollback is non-fatal — the spacecraft must boot). The deserialise
does **not** emit a `MODE_CHANGED` event (a boot restore is not a
runtime transition) and does **not** bump the generation counter.

## Versioning of this document

This file specifies on-flash bytes. A change to the layout above is a
breaking change to the persistence contract and requires:

1. Bumping `MIGRIS_NVSTORE_FORMAT_VERSION` (currently `1`).
2. A note in `CHANGELOG.md` under *Changed (breaking)*.
3. Old images are then rejected by `migris_nvstore_load` with
   `MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE` and the datapool falls back to
   its Kconfig defaults — operators must reapply their tuned values.

Adding a *new* record type (without changing the header or the
record-frame layout) is **non-breaking**: old firmware ignores the
unknown type on read; new firmware emits both old and new records.
