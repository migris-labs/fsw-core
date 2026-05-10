# `tests/renode/` — Renode-driven smoke tests (slice fsw-3)

A small pytest suite that boots cross-compiled fsw-core artefacts in
[Renode](https://renode.io/) (1.16.1) and asserts on their externally
observable behaviour. The first test (`test_hello_uart.py`) is the
fsw-3 contract: the `samples/hello` ELF must print its banner on
USART3 within a bounded timeout.

This is the same closed-loop pattern as the
[`hw-catalog/adapters/renode/tests/`](https://github.com/migris-labs/hw-catalog/tree/main/adapters/renode/tests)
suite (`_renode_driver.py` + `pytest`), specialised for asserting on
UART console output instead of memory-mapped register sentinels.

## How it works

1. `_renode_driver.py::RenodeMonitor` spawns `renode --port <ephemeral>
   --disable-xwt --plain --hide-log` and connects to the resulting
   telnet monitor.
2. The `hello_running` fixture (`conftest.py`) sets two monitor
   variables (`$elf`, `$uart_port`) and executes
   `scripts/hello_nucleo_h753zi.resc`, which:
   - loads the Renode-bundled `@platforms/boards/nucleo_h753zi.repl`
   - loads the ELF onto the sysbus
   - exposes USART3 as a TCP server-socket terminal on `$uart_port`
3. `_renode_driver.py::UartCapture` attaches a background reader to
   that TCP port *before* `start` is issued, so no boot bytes are
   dropped.
4. The test polls `uart.expect(b"...", timeout=...)` for the contract
   strings; on timeout it surfaces the running UART buffer for triage.

## Run it locally

### Prerequisites

- **Renode 1.16.1** — on macOS, install the official `.dmg` to
  `~/Applications/Renode.app/` (the driver auto-discovers it). On
  Linux, put `renode` on `$PATH`, or set `RENODE_BIN=/abs/path/renode`.
- A built **hello-world ELF**. From the west workspace root:

  ```shell
  west build -b nucleo_h753zi fsw-core/samples/hello --pristine=always
  ```

  produces `build/zephyr/zephyr.elf` (which the suite finds via its
  default search path; see `_find_hello_elf` in `conftest.py`).

- **Python 3.10+** and `pytest`:

  ```shell
  python -m pip install pytest
  ```

### Run

From the `fsw-core/` directory:

```shell
pytest tests/renode -v
```

Override the ELF location explicitly if your build dir differs:

```shell
FSW_CORE_HELLO_ELF=/abs/path/to/zephyr.elf pytest tests/renode -v
```

A successful run completes in well under 30 s wall-clock.

If Renode or the ELF is missing, the suite **skips** cleanly rather
than failing — the `pytestmark` skipifs in `test_hello_uart.py` print
the exact reason and the env var that would unblock it.

## Manual interactive debugging

The `.resc` script is parametrised but otherwise standalone; you can
drive it by hand from the Renode monitor:

```text
$ renode
(monitor) $elf = @/abs/path/to/zephyr.elf
(monitor) $uart_port = 4444
(monitor) i @tests/renode/scripts/hello_nucleo_h753zi.resc
(monitor) start
```

In another terminal:

```shell
nc 127.0.0.1 4444
```

…and you should see the two contract strings stream in. Attach
`showAnalyzer sysbus.usart3` instead if you want the GUI analyzer
window (requires the desktop Renode binary, not the headless tarball).

## CI

The `renode-smoke-hello` job in `.github/workflows/ci.yml` runs after
`zephyr-build`, downloads the `fsw-core-hello-nucleo_h753zi` artefact,
installs Renode 1.16.1 via the
[portable tarball](https://github.com/renode/renode/releases) (the
`.deb` package is known to 404 on Azure-hosted Ubuntu mirrors), and
runs `pytest tests/renode -v`.

## Layout

```
tests/renode/
├── _renode_driver.py                 # RenodeMonitor + UartCapture
├── conftest.py                       # ELF discovery, hello_running fixture
├── test_hello_uart.py                # the fsw-3 smoke test
├── scripts/
│   └── hello_nucleo_h753zi.resc      # parametrised Renode startup script
└── README.md                         # this file
```
