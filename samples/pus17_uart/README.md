# `samples/pus17_uart`

Slice **fsw-4** — first on-board PUS service in the Migris flight-
software framework. The application boots Zephyr on the platform-
pinned `nucleo_h753zi` board, listens on **USART3** for a single
CCSDS Space Packet carrying a **PUS-17[1]** "are-you-alive"
telecommand, validates it byte for byte against the wire-format
spec, and replies with a **PUS-17[2]** "are-you-alive report"
telemetry packet on the same UART.

Wire-format authority: [`docs/wire/pus-17.md`](../../docs/wire/pus-17.md).

## Build

From the west workspace root (the parent of `fsw-core/`):

```shell
west build -b nucleo_h753zi fsw-core/samples/pus17_uart --pristine=always \
  --build-dir build/zephyr-pus17
```

Output ELF: `build/zephyr-pus17/zephyr/zephyr.elf`.

## Run in Renode

```shell
pytest fsw-core/tests/renode -k pus17 -v
# Override the ELF location: FSW_CORE_PUS17_ELF=/abs/path/to/zephyr.elf
```

Or interactively:

```text
$ renode
(monitor) $elf = @/abs/path/to/zephyr.elf
(monitor) $uart_port = 4444
(monitor) i @fsw-core/tests/renode/scripts/pus17_nucleo_h753zi.resc
(monitor) start
```

Then send a PUS-17[1] TC to `127.0.0.1:4444`; the FSW replies with a
PUS-17[2] TM on the same connection.

## What the sample is and isn't

**It is** the smallest production-credible PUS service: a
freestanding C codec linked into both flight and host builds, a
strict byte-for-byte decoder, and a round-trip exercised end-to-end
in CI on an emulated Cortex-M7.

**It isn't** the eventual PUS-17 production residency. PUS-17 will
move into mission FSW (e.g. `cry4-fsw`) once `fsw-core` exposes its
proper Zephyr-module surface and PUS-1 verification lands. For now,
this sample is the on-board *integration test* for the wire format.
