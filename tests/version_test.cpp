// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris

#include "migris/fsw/version.hpp"

#include <gtest/gtest.h>

#include <string>

namespace migris::fsw {
namespace {

TEST(Version, ConstantsMatchPinnedProjectVersion) {
    EXPECT_EQ(version_major, 0U);
    EXPECT_EQ(version_minor, 1U);
    EXPECT_EQ(version_patch, 0U);
}

TEST(Version, StringIsNonEmpty) {
    EXPECT_FALSE(version_string().empty());
}

TEST(Version, StringMatchesConstants) {
    const std::string expected = std::to_string(version_major) + "." +
                                 std::to_string(version_minor) + "." +
                                 std::to_string(version_patch);
    EXPECT_EQ(version_string(), expected);
}

}  // namespace
}  // namespace migris::fsw
