# `samples/tc_uart`

Slice **fsw-5** — TC reception, verification and routing in the
Migris flight-software framework. The application boots Zephyr on the
platform-pinned `nucleo_h753zi` board, listens on **USART3** for a
CCSDS Space Packet, and hands every received packet to the **TC
router** (`migris_tc_router_dispatch`). The router validates the TC,
emits **PUS-1** verification reports as requested by the TC's ack
flags, routes accepted commands to their service (today only
**PUS-17** connection test), and writes every resulting TM back on
the same UART.

A single inbound TC can therefore produce up to three packets,
back-to-back:

| TC ack flags | Packets emitted |
|--------------|-----------------|
| none | PUS-17[2] only |
| `ACK_ACCEPTANCE` | PUS-1[1] · PUS-17[2] |
| `ACK_ACCEPTANCE \| ACK_COMPLETION` | PUS-1[1] · PUS-17[2] · PUS-1[7] |
| `ACK_COMPLETION` | PUS-17[2] · PUS-1[7] |

A TC that fails acceptance (bad CRC, unknown service, wrong PUS
version, …) yields a single PUS-1[2] failure report if it asked for
acceptance verification, and is not routed or completed. A malformed
length is always reported. A TC not addressed to this application
process (APID `0x100`) produces no output.

Wire-format authority:
[`docs/wire/pus-1.md`](../../docs/wire/pus-1.md) and
[`docs/wire/pus-17.md`](../../docs/wire/pus-17.md).

## Build

From the west workspace root (the parent of `fsw-core/`):

```shell
west build -b nucleo_h753zi fsw-core/samples/tc_uart --pristine=always \
  --build-dir build/zephyr-tc
```

Output ELF: `build/zephyr-tc/zephyr/zephyr.elf`.

## Run in Renode

```shell
pytest fsw-core/tests/renode -k tc -v
# Override the ELF location: FSW_CORE_TC_ELF=/abs/path/to/zephyr.elf
```

Or interactively:

```text
$ renode
(monitor) $elf = @/abs/path/to/zephyr.elf
(monitor) $uart_port = 4444
(monitor) i @fsw-core/tests/renode/scripts/tc_nucleo_h753zi.resc
(monitor) start
```

Then send a TC to `127.0.0.1:4444`; the FSW replies with the
verification / service TM stream on the same connection.

## What the sample is and isn't

**It is** the smallest production-credible TC handling path: a
freestanding C codec + router linked into both flight and host
builds, a strict byte-for-byte decoder, and a round-trip exercised
end-to-end in CI on an emulated Cortex-M7.

**It isn't** the eventual production residency. The router and its
services move into mission FSW (e.g. `cry4-fsw`) once `fsw-core`
exposes its proper Zephyr-module surface. For now, this sample is the
on-board *integration test* for the wire format.
