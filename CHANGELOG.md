# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
