/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board parameter datapool — typed parameter store plus the
 * big-endian wire codec for typed values. See
 * migris/fsw/datapool/datapool.h for the contract and rationale.
 *
 * This translation unit is the only place the typed-value union is
 * accessed (the constructors, the accessors, and the codec helpers);
 * the tag discipline is enforced here. lib/datapool/.clang-tidy
 * silences cppcoreguidelines-pro-type-union-access accordingly — a
 * tagged union is the idiomatic variant type in freestanding C99.
 */

#include "migris/fsw/datapool/datapool.h"

#include <stddef.h>
#include <stdint.h>

size_t migris_dp_type_width(migris_dp_type_t type) {
    switch (type) {
    case MIGRIS_DP_TYPE_U8:
    case MIGRIS_DP_TYPE_I8:
        return 1U;
    case MIGRIS_DP_TYPE_U16:
    case MIGRIS_DP_TYPE_I16:
        return 2U;
    case MIGRIS_DP_TYPE_U32:
    case MIGRIS_DP_TYPE_I32:
    case MIGRIS_DP_TYPE_F32:
        return 4U;
    }
    /* Out-of-range type (e.g. a corrupt or future enumerator). No
     * `default:` arm — that keeps -Wswitch checking every defined type
     * is handled while -Wcovered-switch-default stays quiet. */
    return 0U;
}

/* --- Typed value constructors ------------------------------------- */

migris_dp_value_t migris_dp_u8(uint8_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_U8;
    out.as.u8 = value;
    return out;
}

migris_dp_value_t migris_dp_u16(uint16_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_U16;
    out.as.u16 = value;
    return out;
}

migris_dp_value_t migris_dp_u32(uint32_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_U32;
    out.as.u32 = value;
    return out;
}

migris_dp_value_t migris_dp_i8(int8_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_I8;
    out.as.i8 = value;
    return out;
}

migris_dp_value_t migris_dp_i16(int16_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_I16;
    out.as.i16 = value;
    return out;
}

migris_dp_value_t migris_dp_i32(int32_t value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_I32;
    out.as.i32 = value;
    return out;
}

migris_dp_value_t migris_dp_f32(float value) {
    migris_dp_value_t out = {0};
    out.type = MIGRIS_DP_TYPE_F32;
    out.as.f32 = value;
    return out;
}

/* --- Typed value accessors --------------------------------------- */

uint8_t migris_dp_as_u8(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0U;
    }
    return value->as.u8;
}

uint16_t migris_dp_as_u16(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0U;
    }
    return value->as.u16;
}

uint32_t migris_dp_as_u32(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0U;
    }
    return value->as.u32;
}

int8_t migris_dp_as_i8(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0;
    }
    return value->as.i8;
}

int16_t migris_dp_as_i16(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0;
    }
    return value->as.i16;
}

int32_t migris_dp_as_i32(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0;
    }
    return value->as.i32;
}

float migris_dp_as_f32(const migris_dp_value_t* value) {
    if (value == NULL) {
        return 0.0F;
    }
    return value->as.f32;
}

/* --- Value codec ------------------------------------------------- */

/* Extract the up-to-32-bit representation of a value, ready to be
 * written big-endian: an integer as its value (sign-correctly widened
 * via its own typed member, never via the overlaid u32 — that would be
 * host-endianness dependent), a float as its IEEE-754 bit pattern
 * (read through the union's overlaid u32, which is the well-defined
 * freestanding-C float reinterpret — UBSan-clean, unlike a pointer
 * cast). */
static uint32_t dp_value_bits(const migris_dp_value_t* value) {
    switch (value->type) {
    case MIGRIS_DP_TYPE_U8:
        return (uint32_t)value->as.u8;
    case MIGRIS_DP_TYPE_U16:
        return (uint32_t)value->as.u16;
    case MIGRIS_DP_TYPE_U32:
        return value->as.u32;
    case MIGRIS_DP_TYPE_I8:
        return (uint32_t)(uint8_t)value->as.i8;
    case MIGRIS_DP_TYPE_I16:
        return (uint32_t)(uint16_t)value->as.i16;
    case MIGRIS_DP_TYPE_I32:
        return (uint32_t)value->as.i32;
    case MIGRIS_DP_TYPE_F32:
        return value->as.u32;
    }
    return 0U;
}

/* Inverse of dp_value_bits: store `bits` into the union member the
 * `type` selects. The union is zeroed first so a decoded value is
 * byte-deterministic regardless of the type's width. */
static void dp_value_from_bits(migris_dp_value_t* value, migris_dp_type_t type, uint32_t bits) {
    value->type = type;
    value->as.u32 = 0U;
    switch (type) {
    case MIGRIS_DP_TYPE_U8:
        value->as.u8 = (uint8_t)bits;
        break;
    case MIGRIS_DP_TYPE_U16:
        value->as.u16 = (uint16_t)bits;
        break;
    case MIGRIS_DP_TYPE_U32:
        value->as.u32 = bits;
        break;
    case MIGRIS_DP_TYPE_I8:
        value->as.i8 = (int8_t)(uint8_t)bits;
        break;
    case MIGRIS_DP_TYPE_I16:
        value->as.i16 = (int16_t)(uint16_t)bits;
        break;
    case MIGRIS_DP_TYPE_I32:
        value->as.i32 = (int32_t)bits;
        break;
    case MIGRIS_DP_TYPE_F32:
        value->as.u32 = bits;
        break;
    }
}

int migris_dp_value_encode(const migris_dp_value_t* value, uint8_t* out, size_t out_cap) {
    if (value == NULL || out == NULL) {
        return MIGRIS_DATAPOOL_ERR_BAD_ARG;
    }
    const size_t width = migris_dp_type_width(value->type);
    if (width == 0U) {
        return MIGRIS_DATAPOOL_ERR_TYPE;
    }
    if (out_cap < width) {
        return MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL;
    }
    const uint32_t bits = dp_value_bits(value);
    for (size_t i = 0U; i < width; ++i) {
        const size_t shift = 8U * (width - 1U - i);
        out[i] = (uint8_t)((bits >> shift) & 0xFFU);
    }
    return (int)width;
}

int migris_dp_value_decode(migris_dp_value_t* value,
                           migris_dp_type_t type,
                           const uint8_t* in,
                           size_t in_len) {
    if (value == NULL || in == NULL) {
        return MIGRIS_DATAPOOL_ERR_BAD_ARG;
    }
    const size_t width = migris_dp_type_width(type);
    if (width == 0U) {
        return MIGRIS_DATAPOOL_ERR_TYPE;
    }
    if (in_len < width) {
        return MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL;
    }
    uint32_t bits = 0U;
    for (size_t i = 0U; i < width; ++i) {
        bits = (bits << 8U) | (uint32_t)in[i];
    }
    dp_value_from_bits(value, type, bits);
    return (int)width;
}

/* --- Store ------------------------------------------------------- */

/* True iff `id` appears among the first `upto` parameters. Pulled out
 * so the validation loop stays flat (cognitive complexity). */
static int dp_id_seen(const migris_dp_param_t* params, size_t upto, migris_dp_param_id_t id) {
    for (size_t i = 0U; i < upto; ++i) {
        if (params[i].id == id) {
            return 1;
        }
    }
    return 0;
}

/* Validate the caller's init set: every value type in range, no
 * duplicate IDs. Returns MIGRIS_DATAPOOL_OK or a negative code. */
static int dp_validate(const migris_dp_param_t* params, size_t n) {
    for (size_t i = 0U; i < n; ++i) {
        if (migris_dp_type_width(params[i].value.type) == 0U) {
            return MIGRIS_DATAPOOL_ERR_TYPE;
        }
        if (dp_id_seen(params, i, params[i].id) != 0) {
            return MIGRIS_DATAPOOL_ERR_DUPLICATE;
        }
    }
    return MIGRIS_DATAPOOL_OK;
}

int migris_datapool_init(migris_datapool_t* dp, const migris_dp_param_t* params, size_t n) {
    if (dp == NULL || (params == NULL && n > 0U)) {
        return MIGRIS_DATAPOOL_ERR_BAD_ARG;
    }
    /* Empty until validation passes — a failed init leaves no
     * half-populated pool behind (stateless failure). */
    dp->count = 0U;
    if (n > MIGRIS_DATAPOOL_CAPACITY) {
        return MIGRIS_DATAPOOL_ERR_CAPACITY;
    }
    const int rc = dp_validate(params, n);
    if (rc != MIGRIS_DATAPOOL_OK) {
        return rc;
    }
    for (size_t i = 0U; i < n; ++i) {
        dp->params[i] = params[i];
    }
    dp->count = n;
    return MIGRIS_DATAPOOL_OK;
}

const migris_dp_param_t* migris_datapool_find(const migris_datapool_t* dp,
                                              migris_dp_param_id_t id) {
    if (dp == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < dp->count; ++i) {
        if (dp->params[i].id == id) {
            return &dp->params[i];
        }
    }
    return NULL;
}

int migris_datapool_get(const migris_datapool_t* dp,
                        migris_dp_param_id_t id,
                        migris_dp_value_t* out) {
    if (dp == NULL || out == NULL) {
        return MIGRIS_DATAPOOL_ERR_BAD_ARG;
    }
    const migris_dp_param_t* param = migris_datapool_find(dp, id);
    if (param == NULL) {
        return MIGRIS_DATAPOOL_ERR_NOT_FOUND;
    }
    *out = param->value;
    return MIGRIS_DATAPOOL_OK;
}

int migris_datapool_set(migris_datapool_t* dp,
                        migris_dp_param_id_t id,
                        const migris_dp_value_t* value) {
    if (dp == NULL || value == NULL) {
        return MIGRIS_DATAPOOL_ERR_BAD_ARG;
    }
    for (size_t i = 0U; i < dp->count; ++i) {
        if (dp->params[i].id != id) {
            continue;
        }
        if (value->type != dp->params[i].value.type) {
            return MIGRIS_DATAPOOL_ERR_TYPE;
        }
        if (dp->params[i].access != MIGRIS_DP_ACCESS_READ_WRITE) {
            return MIGRIS_DATAPOOL_ERR_READ_ONLY;
        }
        dp->params[i].value = *value;
        return MIGRIS_DATAPOOL_OK;
    }
    return MIGRIS_DATAPOOL_ERR_NOT_FOUND;
}
