// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris

#pragma once

#include <cstdint>
#include <string_view>

/// Migris flight-software framework — public API root.
///
/// This header exposes only the library version. Consumers use it to verify
/// ABI compatibility at link time and to log the running version.
namespace migris::fsw {

inline constexpr std::uint32_t version_major = 0;
inline constexpr std::uint32_t version_minor = 1;
inline constexpr std::uint32_t version_patch = 0;

/// Library version in canonical `MAJOR.MINOR.PATCH` form.
///
/// The returned `string_view` is stable for the lifetime of the process.
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace migris::fsw
