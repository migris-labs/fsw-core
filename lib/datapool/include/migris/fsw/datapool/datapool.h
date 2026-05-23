/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board parameter datapool — a typed, fixed-capacity store of
 * numbered parameters plus the big-endian wire codec for their values.
 * Slice fsw-9, the framework's first parameter store.
 *
 * A "parameter" is one operator-tunable on-board value: a threshold, a
 * gain, a period, a mode flag. The datapool is the addressable pool
 * those values live in. It is generic and mission-agnostic — it knows
 * nothing about PUS. The PUS-20 service (lib/pus/pus20.{h,c}) is the
 * ground-facing face that reports and sets these parameters; PUS-3
 * housekeeping is a future consumer. fsw-core hard-codes no parameters
 * of its own: the caller (the sample app, ultimately mission flight
 * software) supplies the parameter set at init.
 *
 * Parameters are NUMBERED, not named. A 2-byte ID is the addressable
 * unit on the wire; human-readable names live in the ground MIB, never
 * on-board. The ID range 0x0001..0x00FF is reserved for fsw-core
 * framework parameters; mission flight software (cry4-fsw) owns 0x0100
 * and above. This mirrors the PUS-3 SID and PUS-5 event-ID block
 * splits and the pinned "PUS-128+ vendor assignments live downstream"
 * decision.
 *
 * Each parameter carries a typed value — the framework's first
 * tagged-union variant type (``migris_dp_value_t``). The supported
 * scalar types (``migris_dp_type_t``) are the unsigned and signed
 * 8/16/32-bit integers and 32-bit IEEE-754 float. The enum is
 * append-only: a new type takes the next free value and never
 * renumbers an existing one, so widening the type set later is a
 * non-breaking wire change — a value is always encoded at the width
 * its registered type dictates, and ground tooling decodes from the
 * MIB id->type map. Touch a value only through the typed constructors
 * and accessors below, never the union directly.
 *
 * The store is RAM-only and volatile: parameters reset to their
 * initial values whenever ``migris_datapool_init`` runs (on every
 * reboot). Non-volatile persistence across reset is deferred to a
 * future non-volatile-storage capability (a flash storage subsystem —
 * note that the PUS-15 packet store, lib/pktstore/, is itself
 * RAM-backed and does not provide it). Capacity is a compile-time
 * constant — freestanding, no malloc. Freestanding C — no Zephyr, no
 * stdlib.
 *
 * Byte-level specification of the value codec: docs/wire/pus-20.md.
 */

#ifndef MIGRIS_FSW_DATAPOOL_DATAPOOL_H_
#define MIGRIS_FSW_DATAPOOL_DATAPOOL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the datapool, in parameters. Compile-time constant so
 *  the store is statically sized (freestanding, no malloc). Override
 *  with ``-DMIGRIS_DATAPOOL_CAPACITY=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_DATAPOOL_CAPACITY``); the host library uses
 *  the default. */
#ifndef MIGRIS_DATAPOOL_CAPACITY
#    define MIGRIS_DATAPOOL_CAPACITY 16U
#endif

/** Parameter ID — a 2-byte big-endian identifier, the addressable unit
 *  on the wire. 0x0001..0x00FF is reserved for fsw-core framework
 *  parameters; mission flight software owns 0x0100+. */
typedef uint16_t migris_dp_param_id_t;

/** Scalar type of a parameter value. The on-wire width is 1, 2 or 4
 *  bytes by type (see ``migris_dp_type_width``); multi-byte integers
 *  are big-endian two's-complement, ``F32`` is IEEE-754 big-endian.
 *  This enum is APPEND-ONLY — a new type takes the next free value and
 *  never renumbers an existing one, so the type set can widen later
 *  without breaking the wire. */
typedef enum {
    MIGRIS_DP_TYPE_U8 = 0,  /**< uint8_t,  1 byte. */
    MIGRIS_DP_TYPE_U16 = 1, /**< uint16_t, 2 bytes big-endian. */
    MIGRIS_DP_TYPE_U32 = 2, /**< uint32_t, 4 bytes big-endian. */
    MIGRIS_DP_TYPE_I8 = 3,  /**< int8_t,   1 byte two's-complement. */
    MIGRIS_DP_TYPE_I16 = 4, /**< int16_t,  2 bytes big-endian two's-complement. */
    MIGRIS_DP_TYPE_I32 = 5, /**< int32_t,  4 bytes big-endian two's-complement. */
    MIGRIS_DP_TYPE_F32 = 6  /**< float,    4 bytes IEEE-754 big-endian. */
} migris_dp_type_t;

/** Access policy of a parameter. A read-only parameter is reported by
 *  PUS-20[1] but rejects a PUS-20[3] set. */
typedef enum { MIGRIS_DP_ACCESS_READ_ONLY = 0, MIGRIS_DP_ACCESS_READ_WRITE = 1 } migris_dp_access_t;

/** A typed parameter value — the framework's first tagged-union
 *  variant. ``type`` selects the active ``as`` member. Construct one
 *  with a ``migris_dp_uNN`` / ``migris_dp_iNN`` / ``migris_dp_f32``
 *  helper and read it with a ``migris_dp_as_*`` accessor: those keep
 *  the tag and the member in step, and confine union access (a C++
 *  Core Guideline finding with no freestanding-C equivalent) to one
 *  translation unit. Stored inline by value everywhere; the datapool
 *  never holds a pointer into application memory. */
typedef struct {
    migris_dp_type_t type;

    union {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        float f32;
    } as;
} migris_dp_value_t;

/** One parameter: its ID, its access policy, and its current value
 *  (whose ``type`` field is the parameter's declared type). The caller
 *  supplies an array of these to ``migris_datapool_init`` — the
 *  ``value`` of each is that parameter's *initial* value. */
typedef struct {
    migris_dp_param_id_t id;
    migris_dp_access_t access;
    migris_dp_value_t value;
} migris_dp_param_t;

/** The datapool. Caller-owned. Volatile values reset to their initial
 *  contents on every ``migris_datapool_init`` call; the non-volatile
 *  storage layer (``lib/nvstore/``, slice fsw-16) is what restores
 *  operator-tuned values across a reboot.
 *
 *  ``generation`` is monotonically incremented on every successful
 *  ``migris_datapool_set`` — the tc_uart sample (and any future
 *  consumer) polls it to detect a mutation since the last save without
 *  re-reading each parameter. The field is RAM-only: it starts at 0 on
 *  every boot, is bumped by sets, and is NOT included in the on-flash
 *  serialised image (which carries values only, not their counters). */
typedef struct {
    migris_dp_param_t params[MIGRIS_DATAPOOL_CAPACITY];
    size_t count;
    uint32_t generation;
} migris_datapool_t;

/** Datapool / value-codec return and error codes. Same convention as
 *  the rest of the framework: 0 (or a positive byte count) is success,
 *  a negative value is one of these. */
typedef enum {
    MIGRIS_DATAPOOL_OK = 0,
    MIGRIS_DATAPOOL_ERR_BAD_ARG = -1,      /**< NULL pointer argument. */
    MIGRIS_DATAPOOL_ERR_CAPACITY = -2,     /**< More parameters than the capacity. */
    MIGRIS_DATAPOOL_ERR_DUPLICATE = -3,    /**< Repeated parameter ID in the init set. */
    MIGRIS_DATAPOOL_ERR_TYPE = -4,         /**< Type out of range, or value type mismatch. */
    MIGRIS_DATAPOOL_ERR_NOT_FOUND = -5,    /**< Parameter ID is not in the pool. */
    MIGRIS_DATAPOOL_ERR_READ_ONLY = -6,    /**< Set attempted on a read-only parameter. */
    MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL = -7 /**< Value codec buffer shorter than the type width. */
} migris_datapool_status_t;

/** On-wire width in bytes (1, 2 or 4) of a parameter type. Returns 0
 *  for a type outside the defined enum (defensive). */
size_t migris_dp_type_width(migris_dp_type_t type);

/** Typed value constructors — the only blessed way to build a
 *  ``migris_dp_value_t``. Each sets ``type`` and the matching union
 *  member together. */
migris_dp_value_t migris_dp_u8(uint8_t value);
migris_dp_value_t migris_dp_u16(uint16_t value);
migris_dp_value_t migris_dp_u32(uint32_t value);
migris_dp_value_t migris_dp_i8(int8_t value);
migris_dp_value_t migris_dp_i16(int16_t value);
migris_dp_value_t migris_dp_i32(int32_t value);
migris_dp_value_t migris_dp_f32(float value);

/** Typed value accessors. The caller is responsible for matching the
 *  accessor to ``value->type`` (read it first); reading through the
 *  wrong accessor is a caller bug. NULL ``value`` yields 0. */
uint8_t migris_dp_as_u8(const migris_dp_value_t* value);
uint16_t migris_dp_as_u16(const migris_dp_value_t* value);
uint32_t migris_dp_as_u32(const migris_dp_value_t* value);
int8_t migris_dp_as_i8(const migris_dp_value_t* value);
int16_t migris_dp_as_i16(const migris_dp_value_t* value);
int32_t migris_dp_as_i32(const migris_dp_value_t* value);
float migris_dp_as_f32(const migris_dp_value_t* value);

/** Serialise ``value`` big-endian into ``out`` (capacity ``out_cap``).
 *  An integer is encoded two's-complement big-endian at its type's
 *  width; ``F32`` is IEEE-754 big-endian. Returns the positive byte
 *  count written (the type width), ``MIGRIS_DATAPOOL_ERR_BUF_TOO_SMALL``
 *  if ``out_cap`` is below it, ``MIGRIS_DATAPOOL_ERR_TYPE`` for an
 *  out-of-range type, or ``MIGRIS_DATAPOOL_ERR_BAD_ARG`` on NULL. */
int migris_dp_value_encode(const migris_dp_value_t* value, uint8_t* out, size_t out_cap);

/** Deserialise a ``type``-typed value from ``in`` (length ``in_len``),
 *  big-endian, into ``*value`` (its ``type`` field is set to ``type``).
 *  Returns the positive byte count consumed (the type width), or a
 *  negative ``migris_datapool_status_t`` (``_TYPE`` / ``_BUF_TOO_SMALL``
 *  / ``_BAD_ARG``) with ``*value`` left unchanged. */
int migris_dp_value_decode(migris_dp_value_t* value,
                           migris_dp_type_t type,
                           const uint8_t* in,
                           size_t in_len);

/** Initialise ``dp`` with ``n`` parameters copied from ``params``,
 *  each starting at its supplied ``value``. Validates: ``n`` within
 *  capacity; every value ``type`` in range; no duplicate IDs. On any
 *  failure the pool is left empty (``count == 0``) and a negative
 *  ``migris_datapool_status_t`` is returned — stateless failure.
 *  ``n == 0`` is valid (an empty pool). Returns ``MIGRIS_DATAPOOL_OK``
 *  on success. */
int migris_datapool_init(migris_datapool_t* dp, const migris_dp_param_t* params, size_t n);

/** Find the parameter with ID ``id``. Returns a borrowed pointer into
 *  ``dp`` (valid while ``dp`` lives and is not re-initialised), or NULL
 *  if no such parameter exists or ``dp`` is NULL. */
const migris_dp_param_t* migris_datapool_find(const migris_datapool_t* dp, migris_dp_param_id_t id);

/** Read the current value of parameter ``id`` into ``*out``. Returns
 *  ``MIGRIS_DATAPOOL_OK``, ``MIGRIS_DATAPOOL_ERR_NOT_FOUND``, or
 *  ``MIGRIS_DATAPOOL_ERR_BAD_ARG``. */
int migris_datapool_get(const migris_datapool_t* dp,
                        migris_dp_param_id_t id,
                        migris_dp_value_t* out);

/** Write ``*value`` to parameter ``id``. Fails — with no state change —
 *  with ``MIGRIS_DATAPOOL_ERR_NOT_FOUND`` if the ID is not in the pool,
 *  ``MIGRIS_DATAPOOL_ERR_TYPE`` if ``value->type`` differs from the
 *  parameter's declared type, or ``MIGRIS_DATAPOOL_ERR_READ_ONLY`` if
 *  the parameter is read-only. On success the pool's ``generation``
 *  counter is incremented — a consumer polling it detects the mutation
 *  without re-reading every parameter. Returns ``MIGRIS_DATAPOOL_OK``. */
int migris_datapool_set(migris_datapool_t* dp,
                        migris_dp_param_id_t id,
                        const migris_dp_value_t* value);

/** Mutation counter — strictly monotonic, bumped on every successful
 *  ``migris_datapool_set``. Lets the application save the pool to NVM
 *  when it changes without polling each parameter. Returns ``0`` on a
 *  NULL store. Resets to ``0`` on every ``migris_datapool_init`` (the
 *  field is NOT persisted — a fresh boot starts at 0 even after a
 *  restored image). */
uint32_t migris_datapool_generation(const migris_datapool_t* dp);

/** Serialise every parameter into ``out`` as a contiguous byte stream
 *  for the ``lib/nvstore/`` persistence layer: ``count(2 BE) +
 *  {id(2 BE), type(1), value(width-per-type, BE)}*``. The access policy
 *  is NOT serialised — it is a code-defined attribute, not operator
 *  state. Returns the positive byte count written on success, or a
 *  negative ``migris_datapool_status_t`` (``_ERR_BUF_TOO_SMALL`` /
 *  ``_ERR_BAD_ARG``). */
int migris_datapool_serialize(const migris_datapool_t* dp, uint8_t* out, size_t out_cap);

/** Restore parameter values from a previously serialised image: for
 *  each ``(id, type, value)`` record, find the matching parameter in
 *  the pool and, if the on-flash type matches the parameter's declared
 *  type, restore the value. Unknown ids and type-mismatches are
 *  silently skipped — robust to a parameter set that changed across a
 *  firmware update. The pool's ``generation`` counter is NOT bumped
 *  (this is a restore, not an operator set). Returns
 *  ``MIGRIS_DATAPOOL_OK`` on a complete decode, ``_ERR_BAD_ARG`` on a
 *  NULL argument, ``_ERR_TYPE`` if a record carries an out-of-range
 *  type, or ``_ERR_BUF_TOO_SMALL`` if the image is truncated. */
int migris_datapool_deserialize(migris_datapool_t* dp, const uint8_t* in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_DATAPOOL_DATAPOOL_H_
