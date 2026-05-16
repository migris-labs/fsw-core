# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
