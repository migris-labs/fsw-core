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

### Defined record types (slice fsw-16)

| `type` | name       | body | source |
|--------|------------|------|--------|
| `1`    | `DATAPOOL` | the serialised parameter datapool (see below) | `lib/datapool/` |

Schedule, hkstore and mode persistence are deferred follow-on slices;
each will claim a new record type in this table.

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
