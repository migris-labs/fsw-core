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

## Status — slice fsw-1 (engineering scaffold)

This commit is the engineering scaffold only:

- Modern CMake (≥ 3.25) host-side build, Conan 2 recipe.
- GoogleTest unit tests, ASan / UBSan / TSan / clang-tidy presets.
- GitHub Actions CI (Ubuntu + macOS, Debug + Release, ASan/UBSan, format,
  clang-tidy).
- Pre-commit hooks (Conventional Commits enforced).

There is **no Zephyr / west / Cortex-M code yet** — that arrives in the
next slice (fsw-2). Today the repository builds a tiny host library
(`migris::fsw-core`) that exposes a version surface, with a single
GoogleTest case verifying it.

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
- Zephyr (incoming, slice fsw-2).
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

## Layout

```
fsw-core/
├── CMakeLists.txt
├── CMakePresets.json
├── conanfile.py
├── cmake/              # Reusable CMake helpers (warnings, sanitizers, clang-tidy)
├── include/migris/fsw/ # Public headers (consumed as <migris/fsw/…>)
├── src/                # Implementation
└── tests/              # GoogleTest unit tests
```

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
