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

## Status — slice fsw-2 (Zephyr workspace + hello-world)

The repository now has two coherent faces:

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
  new `zephyr-build` CI job. A Renode-driven UART smoke test that
  actually boots the ELF and asserts on console output is the next
  slice (fsw-3).

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

### Run on Renode (manual; automated in slice fsw-3)

```text
$ renode
(monitor) mach create "h7"
(monitor) machine LoadPlatformDescription @platforms/boards/nucleo_h753zi.repl
(monitor) sysbus LoadELF @build/zephyr/zephyr.elf
(monitor) showAnalyzer sysbus.usart3
(monitor) start
```

Expected on the UART analyzer:

```
Hello, fsw-core / nucleo_h753zi
boot ok; idling
```

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
├── tests/              # GoogleTest unit tests (host)
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
