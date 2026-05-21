// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// Parameter datapool — typed store + big-endian value codec.
// Pure (gtest-free) helpers keep the test bodies inside the clang-tidy
// cognitive-complexity budget; the tests exercise the value codec
// (each type, big-endian, signed two's-complement, IEEE-754, bounds,
// round-trip), the store (init / find / get / set), and the
// validation and stateless-failure contracts.

#include "migris/fsw/datapool/datapool.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace migris::fsw::datapool::test {
namespace {

// Build a parameter without touching the value union directly (the
// constructors do that, in datapool.c).
migris_dp_param_t
param(migris_dp_param_id_t id, migris_dp_access_t access, migris_dp_value_t value) {
    migris_dp_param_t out{};
    out.id = id;
    out.access = access;
    out.value = value;
    return out;
}

// An undefined parameter type. 7 is inside migris_dp_type_t's
// representational range (the largest defined enumerator is 6) but is
// not a defined type — GCC's -Wconversion rejects a literal outside
// that range, so this is the sentinel for the "unknown type" paths.
constexpr migris_dp_type_t bad_type = static_cast<migris_dp_type_t>(7);

// Encode `v`, decode the bytes back as `v.type`, and require the
// decoded value to re-encode to the identical bytes — bit-exact and
// type-agnostic (no float `==`).
bool roundtrips(const migris_dp_value_t& v) {
    std::array<std::uint8_t, 8U> wire{};
    const int written = migris_dp_value_encode(&v, wire.data(), wire.size());
    if (written <= 0) {
        return false;
    }
    migris_dp_value_t back{};
    const int read =
        migris_dp_value_decode(&back, v.type, wire.data(), static_cast<std::size_t>(written));
    if (read != written || back.type != v.type) {
        return false;
    }
    std::array<std::uint8_t, 8U> wire2{};
    return migris_dp_value_encode(&back, wire2.data(), wire2.size()) == written && wire == wire2;
}

TEST(Datapool, TypeWidthMatchesTheWireContract) {
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_U8), 1U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_I8), 1U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_U16), 2U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_I16), 2U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_U32), 4U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_I32), 4U);
    EXPECT_EQ(migris_dp_type_width(MIGRIS_DP_TYPE_F32), 4U);
    EXPECT_EQ(migris_dp_type_width(bad_type), 0U);
}

TEST(Datapool, ConstructorsAndAccessorsRoundTrip) {
    const migris_dp_value_t v_u8 = migris_dp_u8(0xABU);
    EXPECT_EQ(v_u8.type, MIGRIS_DP_TYPE_U8);
    EXPECT_EQ(migris_dp_as_u8(&v_u8), 0xABU);

    const migris_dp_value_t v_u16 = migris_dp_u16(0xBEEFU);
    EXPECT_EQ(migris_dp_as_u16(&v_u16), 0xBEEFU);

    const migris_dp_value_t v_u32 = migris_dp_u32(0xCAFEF00DU);
    EXPECT_EQ(migris_dp_as_u32(&v_u32), 0xCAFEF00DU);

    const migris_dp_value_t v_i8 = migris_dp_i8(-3);
    EXPECT_EQ(migris_dp_as_i8(&v_i8), -3);

    const migris_dp_value_t v_i16 = migris_dp_i16(-2000);
    EXPECT_EQ(migris_dp_as_i16(&v_i16), -2000);

    const migris_dp_value_t v_i32 = migris_dp_i32(-123456789);
    EXPECT_EQ(migris_dp_as_i32(&v_i32), -123456789);

    const migris_dp_value_t v_f32 = migris_dp_f32(2.5F);
    EXPECT_EQ(v_f32.type, MIGRIS_DP_TYPE_F32);
    EXPECT_FLOAT_EQ(migris_dp_as_f32(&v_f32), 2.5F);
}

TEST(Datapool, AccessorsAreNullSafe) {
    EXPECT_EQ(migris_dp_as_u32(nullptr), 0U);
    EXPECT_EQ(migris_dp_as_i16(nullptr), 0);
    EXPECT_FLOAT_EQ(migris_dp_as_f32(nullptr), 0.0F);
}

TEST(Datapool, ValueEncodeIsBigEndian) {
    const migris_dp_value_t v32 = migris_dp_u32(0xAABBCCDDU);
    std::array<std::uint8_t, 4U> b32{};
    ASSERT_EQ(migris_dp_value_encode(&v32, b32.data(), b32.size()), 4);
    EXPECT_EQ(b32[0], 0xAAU);
    EXPECT_EQ(b32[1], 0xBBU);
    EXPECT_EQ(b32[2], 0xCCU);
    EXPECT_EQ(b32[3], 0xDDU);

    const migris_dp_value_t v16 = migris_dp_u16(0x1234U);
    std::array<std::uint8_t, 2U> b16{};
    ASSERT_EQ(migris_dp_value_encode(&v16, b16.data(), b16.size()), 2);
    EXPECT_EQ(b16[0], 0x12U);
    EXPECT_EQ(b16[1], 0x34U);

    const migris_dp_value_t v8 = migris_dp_u8(0x7FU);
    std::array<std::uint8_t, 1U> b8{};
    ASSERT_EQ(migris_dp_value_encode(&v8, b8.data(), b8.size()), 1);
    EXPECT_EQ(b8[0], 0x7FU);
}

TEST(Datapool, ValueEncodeIsTwosComplementForSignedTypes) {
    const migris_dp_value_t i8v = migris_dp_i8(-1);
    std::array<std::uint8_t, 4U> buf{};
    ASSERT_EQ(migris_dp_value_encode(&i8v, buf.data(), buf.size()), 1);
    EXPECT_EQ(buf[0], 0xFFU);

    const migris_dp_value_t i16v = migris_dp_i16(-2);
    ASSERT_EQ(migris_dp_value_encode(&i16v, buf.data(), buf.size()), 2);
    EXPECT_EQ(buf[0], 0xFFU);
    EXPECT_EQ(buf[1], 0xFEU);

    const migris_dp_value_t i32v = migris_dp_i32(-1);
    ASSERT_EQ(migris_dp_value_encode(&i32v, buf.data(), buf.size()), 4);
    EXPECT_EQ(buf[0], 0xFFU);
    EXPECT_EQ(buf[3], 0xFFU);
}

TEST(Datapool, ValueEncodeF32IsIeee754BigEndian) {
    // 1.5F == 0x3FC00000 in IEEE-754 single precision.
    const migris_dp_value_t v = migris_dp_f32(1.5F);
    std::array<std::uint8_t, 4U> buf{};
    ASSERT_EQ(migris_dp_value_encode(&v, buf.data(), buf.size()), 4);
    EXPECT_EQ(buf[0], 0x3FU);
    EXPECT_EQ(buf[1], 0xC0U);
    EXPECT_EQ(buf[2], 0x00U);
    EXPECT_EQ(buf[3], 0x00U);
}

TEST(Datapool, ValueDecodeReadsBigEndianAndF32) {
    const std::array<std::uint8_t, 4U> u32_wire{0xDEU, 0xADU, 0xBEU, 0xEFU};
    migris_dp_value_t v{};
    ASSERT_EQ(migris_dp_value_decode(&v, MIGRIS_DP_TYPE_U32, u32_wire.data(), u32_wire.size()), 4);
    EXPECT_EQ(v.type, MIGRIS_DP_TYPE_U32);
    EXPECT_EQ(migris_dp_as_u32(&v), 0xDEADBEEFU);

    const std::array<std::uint8_t, 2U> i16_wire{0xFFU, 0xFEU};
    ASSERT_EQ(migris_dp_value_decode(&v, MIGRIS_DP_TYPE_I16, i16_wire.data(), i16_wire.size()), 2);
    EXPECT_EQ(migris_dp_as_i16(&v), -2);

    const std::array<std::uint8_t, 4U> f32_wire{0x3FU, 0xC0U, 0x00U, 0x00U};
    ASSERT_EQ(migris_dp_value_decode(&v, MIGRIS_DP_TYPE_F32, f32_wire.data(), f32_wire.size()), 4);
    EXPECT_FLOAT_EQ(migris_dp_as_f32(&v), 1.5F);
}

TEST(Datapool, ValueCodecRoundTripsEveryType) {
    EXPECT_TRUE(roundtrips(migris_dp_u8(0xC3U)));
    EXPECT_TRUE(roundtrips(migris_dp_u16(0x9A7BU)));
    EXPECT_TRUE(roundtrips(migris_dp_u32(0x12345678U)));
    EXPECT_TRUE(roundtrips(migris_dp_i8(-128)));
    EXPECT_TRUE(roundtrips(migris_dp_i16(-32768)));
    EXPECT_TRUE(roundtrips(migris_dp_i32(-2147483647 - 1)));
    EXPECT_TRUE(roundtrips(migris_dp_f32(-3.14159265F)));
    EXPECT_TRUE(roundtrips(migris_dp_f32(0.0F)));
}

TEST(Datapool, ValueEncodeRejectsSmallBufferAndBadType) {
    const migris_dp_value_t v = migris_dp_u32(1U);
    std::array<std::uint8_t, 3U> small{};
    EXPECT_EQ(migris_dp_value_encode(&v, small.data(), small.size()),
              MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL);

    migris_dp_value_t bad{};
    bad.type = bad_type;
    std::array<std::uint8_t, 4U> buf{};
    EXPECT_EQ(migris_dp_value_encode(&bad, buf.data(), buf.size()), MIGRIS_DATAPOOL_ERR_TYPE);
    EXPECT_EQ(migris_dp_value_encode(nullptr, buf.data(), buf.size()), MIGRIS_DATAPOOL_ERR_BAD_ARG);
}

TEST(Datapool, ValueDecodeRejectsShortInputAndBadType) {
    migris_dp_value_t v{};
    const std::array<std::uint8_t, 2U> two{0x01U, 0x02U};
    EXPECT_EQ(migris_dp_value_decode(&v, MIGRIS_DP_TYPE_U32, two.data(), two.size()),
              MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(migris_dp_value_decode(&v, bad_type, two.data(), two.size()),
              MIGRIS_DATAPOOL_ERR_TYPE);
    EXPECT_EQ(migris_dp_value_decode(nullptr, MIGRIS_DP_TYPE_U16, two.data(), two.size()),
              MIGRIS_DATAPOOL_ERR_BAD_ARG);
}

TEST(Datapool, InitCopiesParamsAndSetsCount) {
    const std::array<migris_dp_param_t, 2U> defs{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(86400U)),
        param(0x0102U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_i16(-7)),
    };
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(dp.count, 2U);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(got.type, MIGRIS_DP_TYPE_U32);
    EXPECT_EQ(migris_dp_as_u32(&got), 86400U);
    ASSERT_EQ(migris_datapool_get(&dp, 0x0102U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_i16(&got), -7);
}

TEST(Datapool, InitWithZeroParamsIsAValidEmptyPool) {
    migris_datapool_t dp{};
    EXPECT_EQ(migris_datapool_init(&dp, nullptr, 0U), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(dp.count, 0U);
    EXPECT_EQ(migris_datapool_find(&dp, 0x0001U), nullptr);
}

TEST(Datapool, InitRejectsOverCapacityAndLeavesPoolEmpty) {
    std::array<migris_dp_param_t, MIGRIS_DATAPOOL_CAPACITY + 1U> too_many{};
    for (std::size_t i = 0U; i < too_many.size(); ++i) {
        too_many[i] = param(static_cast<migris_dp_param_id_t>(0x0100U + i),
                            MIGRIS_DP_ACCESS_READ_WRITE,
                            migris_dp_u8(0U));
    }
    migris_datapool_t dp{};
    EXPECT_EQ(migris_datapool_init(&dp, too_many.data(), too_many.size()),
              MIGRIS_DATAPOOL_ERR_CAPACITY);
    EXPECT_EQ(dp.count, 0U);
}

TEST(Datapool, InitRejectsDuplicateIds) {
    const std::array<migris_dp_param_t, 2U> dup{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u8(1U)),
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u8(2U)),
    };
    migris_datapool_t dp{};
    EXPECT_EQ(migris_datapool_init(&dp, dup.data(), dup.size()), MIGRIS_DATAPOOL_ERR_DUPLICATE);
    EXPECT_EQ(dp.count, 0U);
}

TEST(Datapool, InitRejectsOutOfRangeType) {
    migris_dp_param_t bad = param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u8(0U));
    bad.value.type = bad_type;
    migris_datapool_t dp{};
    EXPECT_EQ(migris_datapool_init(&dp, &bad, 1U), MIGRIS_DATAPOOL_ERR_TYPE);
    EXPECT_EQ(dp.count, 0U);
}

TEST(Datapool, InitRejectsNullArgs) {
    migris_datapool_t dp{};
    EXPECT_EQ(migris_datapool_init(nullptr, nullptr, 0U), MIGRIS_DATAPOOL_ERR_BAD_ARG);
    EXPECT_EQ(migris_datapool_init(&dp, nullptr, 1U), MIGRIS_DATAPOOL_ERR_BAD_ARG);
}

TEST(Datapool, FindReturnsParamOrNull) {
    const std::array<migris_dp_param_t, 1U> defs{
        param(0x0042U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_u16(9U)),
    };
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);

    const migris_dp_param_t* found = migris_datapool_find(&dp, 0x0042U);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, 0x0042U);
    EXPECT_EQ(found->access, MIGRIS_DP_ACCESS_READ_ONLY);
    EXPECT_EQ(migris_datapool_find(&dp, 0x0043U), nullptr);
    EXPECT_EQ(migris_datapool_find(nullptr, 0x0042U), nullptr);
}

TEST(Datapool, GetRejectsUnknownIdAndNullArgs) {
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, nullptr, 0U), MIGRIS_DATAPOOL_OK);
    migris_dp_value_t got{};
    EXPECT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_ERR_NOT_FOUND);
    EXPECT_EQ(migris_datapool_get(&dp, 0x0001U, nullptr), MIGRIS_DATAPOOL_ERR_BAD_ARG);
    EXPECT_EQ(migris_datapool_get(nullptr, 0x0001U, &got), MIGRIS_DATAPOOL_ERR_BAD_ARG);
}

TEST(Datapool, SetMutatesAReadWriteParameter) {
    const std::array<migris_dp_param_t, 1U> defs{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(10U)),
    };
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);

    const migris_dp_value_t next = migris_dp_u32(999U);
    ASSERT_EQ(migris_datapool_set(&dp, 0x0001U, &next), MIGRIS_DATAPOOL_OK);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 999U);
}

TEST(Datapool, SetRejectsReadOnlyWithoutChange) {
    const std::array<migris_dp_param_t, 1U> defs{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_ONLY, migris_dp_u32(10U)),
    };
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);

    const migris_dp_value_t next = migris_dp_u32(999U);
    EXPECT_EQ(migris_datapool_set(&dp, 0x0001U, &next), MIGRIS_DATAPOOL_ERR_READ_ONLY);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(migris_dp_as_u32(&got), 10U);  // unchanged
}

TEST(Datapool, SetRejectsTypeMismatchWithoutChange) {
    const std::array<migris_dp_param_t, 1U> defs{
        param(0x0001U, MIGRIS_DP_ACCESS_READ_WRITE, migris_dp_u32(10U)),
    };
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, defs.data(), defs.size()), MIGRIS_DATAPOOL_OK);

    const migris_dp_value_t wrong_type = migris_dp_u16(7U);
    EXPECT_EQ(migris_datapool_set(&dp, 0x0001U, &wrong_type), MIGRIS_DATAPOOL_ERR_TYPE);

    migris_dp_value_t got{};
    ASSERT_EQ(migris_datapool_get(&dp, 0x0001U, &got), MIGRIS_DATAPOOL_OK);
    EXPECT_EQ(got.type, MIGRIS_DP_TYPE_U32);
    EXPECT_EQ(migris_dp_as_u32(&got), 10U);  // unchanged
}

TEST(Datapool, SetRejectsUnknownIdAndNullArgs) {
    migris_datapool_t dp{};
    ASSERT_EQ(migris_datapool_init(&dp, nullptr, 0U), MIGRIS_DATAPOOL_OK);
    const migris_dp_value_t v = migris_dp_u8(1U);
    EXPECT_EQ(migris_datapool_set(&dp, 0x0001U, &v), MIGRIS_DATAPOOL_ERR_NOT_FOUND);
    EXPECT_EQ(migris_datapool_set(&dp, 0x0001U, nullptr), MIGRIS_DATAPOOL_ERR_BAD_ARG);
    EXPECT_EQ(migris_datapool_set(nullptr, 0x0001U, &v), MIGRIS_DATAPOOL_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::datapool::test
