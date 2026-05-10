# migris-labs/fsw-core

**Flight-software framework** for the
[Migris](../) commercial NewSpace platform.

This repository is the reusable on-board software infrastructure that every
Migris spacecraft product builds on top of: boot, board-support package, HAL,
drivers, PUS service library, FDIR primitives, mode manager, scheduler
patterns. It is generic across spacecraft products — it knows nothing about
any specific mission. Mission-specific application software (e.g.
[`cry4-fsw`](../cry4-fsw)) consumes `migris::fsw-core` as a versioned
dependency.

## Status — slice fsw-3 (Renode UART smoke test)

The repository now has three coherent faces:

- **Host-side** (slice fsw-1): modern CMake (≥ 3.25) build, Conan 2
  recipe, GoogleTest unit tests, ASan / UBSan / TSan / clang-tidy
  presets, GitHub Actions CI (Ubuntu + macOS, Debug + Release,
  ASan/UBSan, format, clang-tidy), pre-commit hooks (Conventional
  Commits enforced). The host library `migris::fsw-core` exposes a
  version surface today.
- **Embedded-side** (slice fsw-2): Zephyr west T2 manifest repository
  pinned to v3.7 LTS, `samples/hello` application for the platform's
  pinned target (STM32H753ZI / ARM Cortex-M7 on `nucleo_h753zi`),
  cross-compiled with ARM GCC 13.2.Rel1 + Zephyr SDK 0.16.8 under a
  new `zephyr-build` CI job.
- **Closed-loop on the emulated target** (slice fsw-3): pytest-driven
  Renode 1.16.1 UART smoke test (`tests/renode/`) that boots the
  artefact ELF on the bundled `nucleo_h753zi` platform, attaches a
  TCP socket terminal to USART3, and asserts the hello-world contract
  strings appear within a bounded timeout. New `renode-smoke-hello`
  CI job depends on `zephyr-build` and consumes its ELF artefact.

## Pinned target (workspace-level)

Per the workspace `CLAUDE.md` `Decisions Pinned`:

- **On-board processor**: ARM Cortex-M7, reference part **STM32H753ZI**.
- **Virtual board**: Renode's bundled `nucleo_h753zi` (matched by
  Zephyr's upstream `boards/st/nucleo_h753zi`).
- **RTOS**: Zephyr (Apache 2.0).
- **PUS service baseline**: P0 = 1, 3, 5, 17; P1 = 11, 13, 15, 20; P2 =
  128+ vendor envelope. See workspace `CLAUDE.md` for the full per-service
  rationale and exclusions.

## Tech stack

- C++20, modern CMake (≥ 3.25), Conan 2 (host build).
- Zephyr v3.7 LTS, west T2 manifest, ARM GCC 13.2.Rel1, Zephyr SDK
  0.16.8 (embedded build, `nucleo_h753zi`).
- GoogleTest for host-side units.
- clang-format + clang-tidy.
- ASan / UBSan / TSan matrix builds.
- GitHub Actions CI on Linux + macOS.

## Quick start (host build — slice fsw-1)

Prerequisites: a C++20 compiler (GCC 13+, Clang 17+, or AppleClang 15+),
CMake ≥ 3.25, [Ninja](https://ninja-build.org/), Python 3.10+ and
[Conan 2](https://docs.conan.io/2/installation.html).

```shell
# First time only
conan profile detect --force
pre-commit install
pre-commit install --hook-type commit-msg

# Configure, build, test (Debug)
conan install . -s build_type=Debug --build=missing --output-folder=build
cmake --preset debug -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake
cmake --build   --preset debug --parallel
ctest           --preset debug

# With sanitizers
conan install . -s build_type=Debug --build=missing --output-folder=build
cmake --preset asan  -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake
cmake --build  --preset asan --parallel
ctest          --preset asan
```

See `CMakePresets.json` for all presets (`debug`, `release`, `asan`, `tsan`,
`tidy`, `docs`).

## Embedded build (Zephyr — slice fsw-2)

`fsw-core` is a [west](https://docs.zephyrproject.org/latest/develop/west/index.html)
T2 manifest repository. The embedded build for the platform's pinned
on-board target — **`nucleo_h753zi`** (STM32H753ZI / ARM Cortex-M7) —
lives alongside, not inside, the host build above.

### Prerequisites

- `arm-none-eabi-gcc` **13.2.Rel1** ([ARM official toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)).
- Zephyr SDK **0.16.8** (the `arm-zephyr-eabi` variant is enough).
- `west`: `pipx install west` or `python -m pip install --user west`.

### One-time bootstrap (fresh machine)

```shell
# Pick any empty parent directory — it becomes the west workspace root.
mkdir migris-fsw-ws && cd migris-fsw-ws
git clone https://github.com/migris-labs/fsw-core.git
west init -l fsw-core
west update                       # clones zephyr + modules as siblings
west zephyr-export

# Workspace root now contains:
#   fsw-core/   zephyr/   modules/   .west/
```

### Build the hello-world app

```shell
# From the workspace root (the parent of fsw-core):
west build -b nucleo_h753zi fsw-core/samples/hello --pristine=always

ls build/zephyr/zephyr.elf
arm-none-eabi-size build/zephyr/zephyr.elf
```

### Renode UART smoke test (slice fsw-3)

The `renode-smoke-hello` CI job boots the artefact ELF in Renode and
asserts that the hello-world contract strings appear on USART3. The
same suite runs locally after a `west build`:

```shell
pytest tests/renode -v
# macOS: driver auto-discovers ~/Applications/Renode.app
# Linux: put `renode` on PATH or set RENODE_BIN
# Override ELF location: FSW_CORE_HELLO_ELF=/abs/path/to/zephyr.elf
```

For manual interactive debugging (attach the GUI analyzer, etc.):

```text
$ renode
(monitor) $elf = @/abs/path/to/zephyr.elf
(monitor) $uart_port = 4444
(monitor) i @tests/renode/scripts/hello_nucleo_h753zi.resc
(monitor) start
```

Expected on USART3 (read with `nc 127.0.0.1 4444` or
`showAnalyzer sysbus.usart3`):

```
Hello, fsw-core / nucleo_h753zi
boot ok; idling
```

See [`tests/renode/README.md`](tests/renode/README.md) for the
fixture internals and troubleshooting.

## Layout

```
fsw-core/
├── CMakeLists.txt
├── CMakePresets.json
├── conanfile.py
├── west.yml            # Zephyr west manifest (T2 topology)
├── cmake/              # Reusable CMake helpers (warnings, sanitizers, clang-tidy)
├── include/migris/fsw/ # Public headers (consumed as <migris/fsw/…>)
├── src/                # Host-side implementation
├── tests/              # Tests (GoogleTest host + pytest Renode smoke)
│   ├── (host gtest sources)
│   └── renode/         # fsw-3 Renode-driven UART smoke (Python)
└── samples/            # Zephyr applications
    └── hello/          # fsw-2 hello-world for nucleo_h753zi
```

`.west/`, `zephyr/`, `modules/` are workspace siblings created by
`west update` — they are not committed (see `.gitignore`).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). In short: Conventional Commits,
pre-commit hooks, tests for new code, CI must stay green.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

## Platform context

`migris-labs/fsw-core` is one of several independent repositories that
make up the Migris commercial NewSpace platform:

| Repo                              | Role                                           |
|-----------------------------------|------------------------------------------------|
| `hw-catalog`                      | HW models and buses                            |
| `fsw-core`                        | Flight-software framework (this repo)          |
| `constellation`                   | N-spacecraft orchestration engine              |
| `ground-segment`                  | Ground station simulator                       |
| `mcs`                             | Modern mission control system                  |
| `cry4`                            | Cry4 spacecraft assembly + mission configs    |
| `cry4-fsw`                        | Cry4 flight software apps                      |

The platform-level context — architecture, pinned decisions, brand and
naming philosophy — lives in the umbrella `CLAUDE.md` (one directory up).
