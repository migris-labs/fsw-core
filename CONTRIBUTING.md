# Contributing to `migris-labs/fsw-core`

Thanks for contributing. This document is intentionally short; it captures
the quality gates every change must clear before landing on `main`.

## Workflow

1. Branch off `main` using a descriptive name (`feat/…`, `fix/…`,
   `docs/…`, `refactor/…`, `test/…`, `chore/…`).
2. Make atomic commits following [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat: …`, `fix: …`, `docs: …`, `refactor: …`, `test: …`, `chore: …`,
   `build: …`, `ci: …`). The `conventional-pre-commit` hook enforces this
   at commit time.
3. Open a pull request against `main`. CI must be green and at least one
   reviewer must approve before merge.

## Prerequisites (one-time)

```shell
# From the repo root
pre-commit install
pre-commit install --hook-type commit-msg
conan profile detect --force
```

## Local checks before pushing

```shell
conan install . -s build_type=Debug --build=missing --output-folder=build
cmake --preset asan -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake
cmake --build   --preset asan --parallel
ctest           --preset asan
cmake --preset tidy -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake
cmake --build   --preset tidy --parallel
```

## Code style

- C++20. No extensions (`-std=c++20`, not `-std=gnu++20`).
- Formatting is enforced by `.clang-format`; never merge unformatted code.
- Naming: `lower_case` for namespaces, functions, variables, constexpr
  values, and member data; `CamelCase` for classes / structs / enums /
  type aliases; `UPPER_CASE` for macros (macros are used sparingly).
- Private members end in a trailing underscore (`foo_`).
- Header files use `#pragma once`. Every source file starts with an
  SPDX license identifier: `// SPDX-License-Identifier: Apache-2.0`.
- Prefer small, single-purpose translation units. No "utils" dumping
  grounds.

## Tests

- Every non-trivial change includes tests. Unit tests are first; add
  integration or closed-loop tests where a subsystem needs them.
- Sanitizer builds (ASan / UBSan / TSan) must pass on the matrix.
- Flaky tests are bugs — fix or remove them, don't retry-loop them.

## Public API changes

- Update Doxygen comments on every changed public symbol.
- Add an entry to `CHANGELOG.md` under `[Unreleased]`.
- Breaking changes bump the major version (pre-1.0: minor) and are
  explicitly called out in the PR description.

## Reviews

- One approving review is the minimum for merge.
- Reviewers may and should challenge design; we'd rather re-scope a PR
  than land an inadequate one.
- Squash-merge with the conventional-commit title as the squash subject.
