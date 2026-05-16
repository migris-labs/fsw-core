# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Slice fsw-7: PUS-3 housekeeping & diagnostic telemetry.** New
  freestanding C housekeeping-report encoder (`lib/pus/pus3.{h,c}`),
  used by **both** a spontaneous periodic emitter and a TC[3,27]
  one-shot poll, for one predefined framework structure
  (`SID 0x0001 FRAMEWORK_DIAG`) carrying a frozen 27-byte parameter
  block: uptime, the shared TM sequence count it consumed, the
  per-service PUS message counters, and three new TC-router counters —
  **TC accepted**, **TC rejected**, and **UART RX-ring overflow drops**
  (previously silently dropped in the RX ISR, now counted). The TC
  router is generalised from a hardcoded-PUS-17-only accept+dispatch to
  a service-type `switch` (PUS-17 + PUS-3); the `tc_uart` sample emits
  the periodic report from its main loop on a coarse FSW-clock
  elapsed-time check (Kconfig `CONFIG_FSW_PUS3_HK_PERIOD_SEC`),
  threading the router's shared per-APID sequence count so it stays
  strictly monotonic across the boot event, every verification /
  service burst, and each periodic report. New host suite
  `tests/pus3_test.cpp`; `tests/tc_router_test.cpp` extended for the
  generalised accept and TC[3,27] routing/counters; a new
  short-period Renode build drives `tests/renode/test_tc_uart_hk.py`
  (periodic appearance, FSW-clock cadence, one-shot-poll round-trip,
  RX-drop wiring). Wire format pinned in
  [`docs/wire/pus-3.md`](docs/wire/pus-3.md). Scoped decisions:
  - Structure-management subtypes (`[1]/[2]/[3]/[4]` create/delete) and
    periodic-generation control (`[5]/[6]` enable/disable) are
    **deferred**: all presuppose a parameter datapool the framework
    does not have yet. Only one predefined structure; dynamic
    creation lands with the datapool.
  - Structure IDs `0x0001`–`0x00FF` are reserved for fsw-core
    *framework* structures; `0x0100`+ is mission-owned (scheme pinned
    when `cry4-fsw` bootstraps), mirroring the PUS-5 event-ID split and
    the pinned "PUS-128+ vendor assignments live downstream" decision.
  - **`switch`, not a registration table.** A service-type switch over
    two services is correct and minimal; a function-pointer dispatch
    table is the *next* abstraction, earned at a third independent
    service.
  - **PUS-5 counters are zero on the [27]-polled path.** The router
    does not own the PUS-5 context; hoisting it in is the deferred
    "FDIR raises events from inside the router" abstraction. The
    spontaneous report (emitted by the context owner) carries live
    values. Asymmetry pinned in `docs/wire/pus-3.md`.
  - **No event/FIFO for the RX-drop counter.** A single-writer (ISR) /
    single-reader (loop) `volatile uint32_t` snapshot is the minimal
    correct mechanism; the bounded event FIFO stays deferred (same
    rationale as fsw-6).
- **Slice fsw-6: PUS-5 event reporting.** New freestanding C
  event-report encoder (`lib/pus/pus5.{h,c}`) for the four severity
  subtypes — informative [1], low [2], medium [3], high [4] anomaly —
  carrying a 2-byte big-endian event-definition ID plus optional
  auxiliary data (≤ 32 bytes). This is the framework's first
  *asynchronous* TM service: a report is emitted spontaneously at the
  point a condition is detected, not as a side effect of an inbound
  TC. The `tc_uart` sample emits one PUS-5[1] `FSW_BOOT` informative
  event on reset — the first TM it produces — threading the router's
  shared per-APID CCSDS sequence count so the boot event consumes
  count 0 and the per-APID sequence stays strictly monotonic across it
  and every subsequent verification / service packet. New host suite
  `tests/pus5_test.cpp`; `tests/renode/test_tc_uart.py` asserts the
  boot event end-to-end and its four pre-existing tests are rebased
  past the leading boot packet (+1 shared sequence count) — an
  intended consequence of the sample now emitting boot TM, not a
  regression. Wire format pinned in
  [`docs/wire/pus-5.md`](docs/wire/pus-5.md). Scoped decisions:
  - The control subtypes [5]/[6] (enable/disable event generation),
    [7] and [8] are **deliberately excluded** — TC-driven event
    reconfiguration overlaps PUS-20 (onboard parameter management,
    P1) and has no driving use case yet.
  - Event-definition IDs `0x0001`–`0x00FF` are reserved for fsw-core
    *framework* events; `0x0100`+ is mission-owned (scheme pinned
    when `cry4-fsw` bootstraps), mirroring the pinned "PUS-128+
    vendor assignments live downstream" decision.
  - **No event queue.** PUS-5 stays a pure stateless encoder (the
    proven pus1/pus17 shape). A freestanding bounded event FIFO is
    the explicit *next* abstraction, earned when a producer that does
    not own a TM output buffer first exists (an FDIR monitor, PUS-3
    housekeeping, or the ISR-context UART RX-ring overflow event).
  - **Router-side anomaly emission on TC rejection is deferred** to
    the FDIR slice (its first real consumer); the TC router is
    untouched, so all fsw-5 host suites stay green unchanged and the
    pinned `docs/wire/pus-1.md` rule-3 "no-ack ⇒ silence" invariant
    is preserved.
- **Slice fsw-5: PUS-1 TC verification (acceptance + completion).**
  New freestanding C TC reception / acceptance / routing layer
  (`lib/pus/tc_router.{h,c}`) — the framework's first on-board
  dispatcher — plus a PUS-1 verification-report encoder
  (`lib/pus/pus1.{h,c}`). A received TC is validated (CCSDS primary,
  length, CRC, PUS-C version, routable service) and, gated by its
  ack-flag bits, the FSW emits PUS-1[1]/[2] acceptance and
  PUS-1[7]/[8] completion reports around the routed service response,
  all back-to-back on the existing UART. Reports carry the verified
  TC's 4-byte request ID (and a 1-byte failure code on failures).
  PUS-1 start ([3]/[4]) and progress ([5]/[6]) are deferred until a
  long-running command exists to exercise them (workspace
  `CLAUDE.md`). New host unit suites `tests/pus1_test.cpp` and
  `tests/tc_router_test.cpp`; `tests/renode/test_tc_uart.py` drives
  the full PUS-1 + PUS-17 round-trip on the emulated `nucleo_h753zi`.
  Wire format pinned in [`docs/wire/pus-1.md`](docs/wire/pus-1.md).

### Changed
- **`MIGRIS_TC_ROUTER_MAX_TM` raised 64 → 96** (fsw-7). The worst-case
  single-TC burst is now `PUS-1[1] (22) + PUS-3[25] (47) + PUS-1[7]
  (22) = 91`. This is a C-API / caller-buffer-size change only — **the
  bytes on the wire are unchanged**. The `tc_uart` sample picks it up
  automatically (`out[MIGRIS_TC_ROUTER_MAX_TM]`).
- **Renode tc_uart is now built twice** (fsw-7). Renode fast-forwards
  idle virtual time, so a fixed short housekeeping period would race
  `test_tc_uart.py`'s fixed-offset reads non-deterministically. The
  verification-stream ELF pins the period to "never within a test"
  (`test_tc_uart.py` is unchanged and still deterministic); a dedicated
  short-period ELF drives the new `test_tc_uart_hk.py`. The
  `zephyr-build` / `renode-smoke` matrices and `conftest.py` gain the
  `tc-hk` entry / `tc_hk_running` fixture / `FSW_CORE_TC_HK_ELF` env
  var.
- **TM sequence count is now shared per-APID.** It moved out of the
  per-service context into the new TC router context, so PUS-1 and
  PUS-17 packets emitted for one TC share a single, strictly
  monotonic CCSDS sequence space (CCSDS 133.0-B-2: one count per APID
  per direction). `migris_pus17_handle_are_you_alive` is replaced by
  `migris_pus17_execute` (generic TC validation moved to the router).
  **The bytes on the wire are unchanged** — this is a C-API reshape,
  not a wire-breaking change.
- **`samples/pus17_uart` renamed to `samples/tc_uart`** (it is now a
  generic TC-reception + verification demonstrator, not PUS-17-only).
  The `zephyr-build` / `renode-smoke` CI matrix entries, the Renode
  `.resc` script, the `tc_running` fixture, and the
  `FSW_CORE_TC_ELF` override env var are renamed to match.

- **Slice fsw-4: PUS-17 connection test over UART.** New freestanding
  C codec under `lib/pus/` (CCSDS Space Packet primary header
  pack/unpack, CRC-16-CCITT-FALSE, PUS-C TC/TM secondary headers,
  PUS-17 service handler), compiled into both `migris::fsw-core`
  (host) and the new `samples/pus17_uart` Zephyr application
  (Cortex-M7) so the wire-format bytes are covered by ASan / UBSan /
  clang-tidy on every PR. New `tests/renode/test_pus17_uart.py`
  drives a round-trip on the emulated `nucleo_h753zi`: ground sends
  a PUS-17[1] TC on USART3, FSW replies with a PUS-17[2] TM. New
  CI job `renode-smoke · pus17` parallels the existing
  `renode-smoke · hello`. Wire format is authoritatively pinned in
  [`docs/wire/pus-17.md`](docs/wire/pus-17.md).
- **Slice fsw-3: Renode-driven UART smoke test on `nucleo_h753zi`.**
  New `tests/renode/` pytest suite boots the fsw-2 hello-world ELF on
  Renode 1.16.1 against the bundled `nucleo_h753zi` platform and
  asserts the hello-world contract strings on USART3. CI gains the
  `renode-smoke-hello` job, depending on `zephyr-build` and consuming
  its ELF artefact via the v4 download-artifact layout.
- **Slice fsw-2: Zephyr west workspace + `nucleo_h753zi` hello-world.**
  Repository is now a west T2 manifest repo (`west.yml`) pinned to
  Zephyr v3.7 LTS. New `samples/hello/` Zephyr application builds for
  `nucleo_h753zi` (STM32H753ZI / ARM Cortex-M7) under ARM GCC
  13.2.Rel1 and Zephyr SDK 0.16.8, prints
  `Hello, fsw-core / nucleo_h753zi` on the UART console.
- New `zephyr-build` CI job builds the ELF and uploads
  `zephyr.elf` / `zephyr.bin` / `zephyr.map` as artefacts.
- `samples/` directory added to the clang-format scope (now covers
  `.c` files in addition to the existing C++ extensions).
- Initial repository scaffold: modern CMake (≥ 3.25) build, Conan 2 recipe,
  CMake presets for `debug` / `release` / `asan` / `tsan` / `tidy` / `docs`.
- GoogleTest integration with `gtest_discover_tests` and a sanitizer build
  matrix (AddressSanitizer + UndefinedBehaviorSanitizer + ThreadSanitizer).
- Static analysis via clang-tidy (comprehensive check set, warnings treated
  as errors) and clang-format (custom LLVM-derived style; identical to
  hw-catalog).
- Pre-commit configuration: trailing-whitespace, EOF, large-file guard,
  clang-format, cmake-format/cmake-lint, Conventional Commits.
- GitHub Actions CI: build + test matrix on Ubuntu and macOS for Debug,
  Release, and ASan+UBSan; clang-format check; clang-tidy job.
- Apache License 2.0, SECURITY policy, CONTRIBUTING guide, CODEOWNERS,
  issue and PR templates.
- Public API placeholder: `migris::fsw::version_major/minor/patch` and
  `migris::fsw::version_string()`.

[Unreleased]: https://github.com/migris-labs/fsw-core/commits/main
