/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board schedule — a bounded, time-tagged store of telecommands
 * awaiting release. Slice fsw-10, the framework's first scheduling
 * primitive.
 *
 * A "scheduled activity" is one telecommand plus an absolute release
 * time: the TC is held verbatim and, once the FSW clock reaches the
 * release time, handed back to the TC router for normal dispatch — so
 * a ground station can load a pass's worth of commands and let them
 * execute autonomously, with no continuous link. This store is the
 * mechanism; PUS-11 (lib/pus/pus11.{h,c}) is the ground-facing face
 * that inserts, deletes and reports activities, and the application's
 * main loop drives the release tick (migris_schedule_pop_due).
 *
 * Each activity is identified by a 4-byte REQUEST IDENTIFIER — the
 * first four bytes of its telecommand (the CCSDS packet identification
 * and packet sequence control), exactly the identifier PUS-1 puts in a
 * verification report. Ground assigns the sequence count, so it owns
 * and knows every request identifier it schedules; the store keeps
 * them unique. Release times are absolute CUC coarse seconds.
 *
 * The store has an enabled / disabled state: while disabled,
 * migris_schedule_pop_due releases nothing (activities are retained).
 * It starts DISABLED — flight-safe: a freshly booted FSW does not
 * autonomously fire a stale schedule until ground enables it.
 *
 * RAM at runtime, but the persisted set (the activities array and the
 * enabled flag) survives a reboot through lib/nvstore/ — slice fsw-17
 * gives the schedule a serialize / deserialize pair and a monotonic
 * generation counter that the application polls to detect a mutation
 * since the last save. Capacity and the per-activity TC size are
 * compile-time constants — freestanding, no malloc. Freestanding C —
 * no Zephyr, no stdlib.
 *
 * Byte-level specification of the PUS-11 wire: docs/wire/pus-11.md.
 */

#ifndef MIGRIS_FSW_SCHEDULE_SCHEDULE_H_
#define MIGRIS_FSW_SCHEDULE_SCHEDULE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the schedule, in activities. Compile-time constant so
 *  the store is statically sized (freestanding, no malloc). Override
 *  with ``-DMIGRIS_SCHEDULE_CAPACITY=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_SCHEDULE_CAPACITY``). */
#ifndef MIGRIS_SCHEDULE_CAPACITY
#    define MIGRIS_SCHEDULE_CAPACITY 16U
#endif

/** Largest telecommand, in bytes, that one activity can hold. A
 *  scheduled TC is a leaf command (PUS-17 / PUS-20-class); 64 covers
 *  the baseline. A larger TC is rejected ``ERR_TC_TOO_LARGE``.
 *  Override with ``-DMIGRIS_SCHEDULE_TC_MAX=<n>`` (Kconfig
 *  ``FSW_SCHEDULE_TC_MAX`` in the sample). */
#ifndef MIGRIS_SCHEDULE_TC_MAX
#    define MIGRIS_SCHEDULE_TC_MAX 64U
#endif

/** Request identifier size — the first 4 bytes of a telecommand (the
 *  CCSDS packet identification + packet sequence control), the same
 *  identifier PUS-1 uses. Uniquely identifies a scheduled activity. */
#define MIGRIS_SCHEDULE_REQUEST_ID_SIZE 4U

/** One scheduled activity: a telecommand held verbatim plus its
 *  absolute release time. ``tc[0..3]`` is the request identifier. */
typedef struct {
    uint32_t release_time;              /**< Absolute CUC coarse seconds. */
    uint8_t tc[MIGRIS_SCHEDULE_TC_MAX]; /**< The telecommand, verbatim. */
    size_t tc_len;                      /**< Telecommand length in bytes. */
} migris_schedule_activity_t;

/** The schedule. Caller-owned. Zero-initialise once, then call
 *  ``migris_schedule_init``. Activities are unordered; release order
 *  is decided per tick by ``migris_schedule_pop_due`` (earliest
 *  release time first).
 *
 *  ``generation`` is monotonically incremented on every successful
 *  call that mutates the persisted set — insert, delete, reset,
 *  set_enabled and pop_due. The tc_uart sample polls it to detect a
 *  mutation since the last save without re-reading the array. The
 *  field is RAM-only: it starts at 0 on every boot, is bumped by
 *  mutations, and is NOT included in the on-flash serialised image. */
typedef struct {
    migris_schedule_activity_t activities[MIGRIS_SCHEDULE_CAPACITY];
    size_t count;
    int enabled;         /**< 0 = disabled (the post-init default), 1 = enabled. */
    uint32_t generation; /**< Mutation counter; slice fsw-17. */
} migris_schedule_t;

/** Schedule return / error codes. Same convention as the rest of the
 *  framework: 0 (or a positive result) is success, negative is one of
 *  these. */
typedef enum {
    MIGRIS_SCHEDULE_OK = 0,
    MIGRIS_SCHEDULE_ERR_BAD_ARG = -1,       /**< NULL pointer, or short request ID. */
    MIGRIS_SCHEDULE_ERR_FULL = -2,          /**< Schedule already at capacity. */
    MIGRIS_SCHEDULE_ERR_TC_TOO_LARGE = -3,  /**< TC longer than MIGRIS_SCHEDULE_TC_MAX. */
    MIGRIS_SCHEDULE_ERR_DUPLICATE = -4,     /**< Request identifier already scheduled. */
    MIGRIS_SCHEDULE_ERR_NOT_FOUND = -5,     /**< Request identifier not in the schedule. */
    MIGRIS_SCHEDULE_ERR_BUF_TOO_SMALL = -6, /**< Output buffer below the due TC's length. */
    MIGRIS_SCHEDULE_ERR_TRUNCATED = -7 /**< Deserialise: input shorter than the declared image. */
} migris_schedule_status_t;

/** Reset ``sched`` to empty and DISABLED. A zero-initialised
 *  ``migris_schedule_t`` is already valid; this is provided for
 *  explicitness at startup. */
void migris_schedule_init(migris_schedule_t* sched);

/** Delete every activity (the PUS-11[3] reset). The enabled / disabled
 *  state is left unchanged — that is owned by PUS-11[1]/[2]. */
void migris_schedule_reset(migris_schedule_t* sched);

/** Set the enabled state: non-zero ``enabled`` permits
 *  ``migris_schedule_pop_due`` to release activities, zero suspends
 *  release (activities are retained). */
void migris_schedule_set_enabled(migris_schedule_t* sched, int enabled);

/** Non-zero iff the schedule is enabled. */
int migris_schedule_is_enabled(const migris_schedule_t* sched);

/** Number of activities currently scheduled. */
size_t migris_schedule_count(const migris_schedule_t* sched);

/** Insert one activity: ``tc`` (``tc_len`` bytes) released at absolute
 *  ``release_time``. Fails — with no state change — with
 *  ``MIGRIS_SCHEDULE_ERR_BAD_ARG`` (NULL, or ``tc_len`` below the
 *  request-identifier size), ``_ERR_TC_TOO_LARGE`` (``tc_len`` over
 *  ``MIGRIS_SCHEDULE_TC_MAX``), ``_ERR_DUPLICATE`` (a scheduled
 *  activity already has this request identifier), or ``_ERR_FULL``.
 *  A release time in the past is accepted — it becomes due on the
 *  next ``migris_schedule_pop_due``. Returns ``MIGRIS_SCHEDULE_OK``. */
int migris_schedule_insert(migris_schedule_t* sched,
                           uint32_t release_time,
                           const uint8_t* tc,
                           size_t tc_len);

/** Delete the activity whose request identifier matches the
 *  ``MIGRIS_SCHEDULE_REQUEST_ID_SIZE`` bytes at ``request_id``.
 *  Returns ``MIGRIS_SCHEDULE_OK``, ``MIGRIS_SCHEDULE_ERR_NOT_FOUND``,
 *  or ``MIGRIS_SCHEDULE_ERR_BAD_ARG``. */
int migris_schedule_delete(migris_schedule_t* sched, const uint8_t* request_id);

/** Find the activity with request identifier ``request_id``. Returns
 *  a borrowed pointer into ``sched`` (valid until the schedule is next
 *  mutated), or NULL if not present / on a NULL argument. */
const migris_schedule_activity_t* migris_schedule_find(const migris_schedule_t* sched,
                                                       const uint8_t* request_id);

/** Release the next due activity. If the schedule is enabled and one
 *  or more activities have ``release_time <= now_seconds``, the one
 *  with the earliest release time is copied into ``out_tc`` (capacity
 *  ``out_cap``), removed from the schedule, its length written to
 *  ``*out_len``, and 1 is returned. Returns 0 if the schedule is
 *  disabled or nothing is due; a negative ``migris_schedule_status_t``
 *  on a NULL argument or if ``out_cap`` is below the due activity's
 *  length (in which case the activity is left scheduled). Call once
 *  per main-loop iteration — one activity is released per call. */
int migris_schedule_pop_due(migris_schedule_t* sched,
                            uint32_t now_seconds,
                            uint8_t* out_tc,
                            size_t out_cap,
                            size_t* out_len);

/** Mutation counter — strictly monotonic, bumped on every successful
 *  ``migris_schedule_insert`` / ``_delete`` / ``_reset`` /
 *  ``_set_enabled`` / ``_pop_due``. Lets the application save the
 *  schedule to NVM when it changes without polling each activity.
 *  Returns 0 on a NULL store. Resets to 0 on every
 *  ``migris_schedule_init`` (the field is NOT persisted — a fresh boot
 *  starts at 0 even after a restored image). */
uint32_t migris_schedule_generation(const migris_schedule_t* sched);

/** Serialise the schedule into ``out`` as a contiguous byte stream for
 *  the ``lib/nvstore/`` persistence layer:
 *
 *      count(2 BE) + enabled(1) + { release_time(4 BE), tc_len(2 BE),
 *                                   tc(tc_len) } * count
 *
 *  Variable-length per-entry so the image stays proportional to actual
 *  scheduled volume. Returns the positive byte count written, or a
 *  negative ``migris_schedule_status_t`` (``_ERR_BUF_TOO_SMALL`` /
 *  ``_ERR_BAD_ARG``). The ``generation`` counter is NOT serialised. */
int migris_schedule_serialize(const migris_schedule_t* sched, uint8_t* out, size_t out_cap);

/** Restore the schedule from a previously serialised image. Replaces
 *  the current ``activities``, ``count`` and ``enabled``. The
 *  ``generation`` counter is NOT bumped (a restore is not a mutation —
 *  the sample's "have I changed since the last save?" loop must not
 *  double-save on every boot). On any error the schedule is reset to
 *  empty + disabled (stateless failure). Returns
 *  ``MIGRIS_SCHEDULE_OK`` on a complete decode, ``_ERR_BAD_ARG`` on a
 *  NULL argument, ``_ERR_TRUNCATED`` if the image is short of the
 *  declared payload, ``_ERR_FULL`` if ``count`` exceeds
 *  ``MIGRIS_SCHEDULE_CAPACITY``, or ``_ERR_TC_TOO_LARGE`` if any
 *  per-entry ``tc_len`` exceeds ``MIGRIS_SCHEDULE_TC_MAX``. */
int migris_schedule_deserialize(migris_schedule_t* sched, const uint8_t* in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_SCHEDULE_SCHEDULE_H_
