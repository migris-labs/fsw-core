/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board operating-mode manager — a generic mode / state-machine
 * primitive. Slice fsw-13, the framework's first operating-state
 * abstraction.
 *
 * A spacecraft runs in one of a small set of operating modes — boot,
 * safe, nominal, payload-active — and only certain transitions between
 * them are legal. The mode manager holds the current mode and the
 * table of allowed transitions, accepts or rejects a transition
 * request against that table, and announces a successful change as a
 * PUS-5 event.
 *
 * It is generic and mission-agnostic: fsw-core hard-codes NO modes.
 * The application supplies the mode set and the allowed transitions at
 * init — the same shape lib/datapool/ uses for its parameter set. A
 * mode is NUMBERED, not named (human-readable names live in the ground
 * MIB); a mode ID is a 1-byte value below MIGRIS_MODE_ID_MAX.
 *
 * Each declared mode carries a bitmask of the modes it may transition
 * TO — an O(1) "is this transition allowed" test. The mode set is a
 * caller-owned, statically sized array (freestanding, no malloc),
 * bounded by the compile-time MIGRIS_MODE_CAPACITY.
 *
 * This slice ships only the generic primitive and its C API. A ground
 * mode-commanding telecommand is a vendor PUS service and belongs in
 * mission flight software (cry4-fsw), not here; FDIR-driven autonomous
 * transitions to a safe mode are a future fsw-core slice. The manager
 * is driven directly through migris_mode_request — by the application
 * now, by those consumers later.
 *
 * Freestanding C — no Zephyr, no malloc, no stdlib. A successful
 * transition is announced through the lib/fsw event-sink seam as a
 * PUS-5 MODE_CHANGED event; wire format: docs/wire/pus-5.md.
 *
 * The mode table (its IDs and allowed transitions) is code-defined
 * at every ``migris_mode_init`` and not persisted — a stale on-flash
 * table would outlive a firmware change to the rules. Only the
 * **current mode** survives a reboot, restored by lib/nvstore/ —
 * slice fsw-17 gives the manager a one-byte serialise / current-only
 * deserialise pair and a generation counter bumped on every
 * successful transition. The deserialise does NOT emit MODE_CHANGED
 * (a boot restore is not a runtime transition) and silently keeps
 * the init-time current mode if the restored ID is unknown to the
 * declared set.
 */

#ifndef MIGRIS_FSW_MODE_MODE_H_
#define MIGRIS_FSW_MODE_MODE_H_

#include "migris/fsw/event_sink.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the mode set, in distinct modes. Compile-time constant
 *  so the manager is statically sized (freestanding, no malloc).
 *  Override with ``-DMIGRIS_MODE_CAPACITY=<n>`` (the `tc_uart` sample
 *  wires this to Kconfig ``FSW_MODE_CAPACITY``); the host library uses
 *  the default. */
#ifndef MIGRIS_MODE_CAPACITY
#    define MIGRIS_MODE_CAPACITY 8U
#endif

/** Number of distinct mode IDs an allowed-target bitmask can address.
 *  A mode ID must be below this value. The bitmask is a ``uint32_t``,
 *  so the ceiling is 32 — far beyond any real spacecraft mode count. */
#define MIGRIS_MODE_ID_MAX 32U

/** Operating-mode identifier — a 1-byte application-defined number.
 *  fsw-core hard-codes no modes; names live in the ground MIB. Valid
 *  IDs are ``0 .. MIGRIS_MODE_ID_MAX - 1``. */
typedef uint8_t migris_mode_id_t;

/** One declared mode: its ID and the set of modes it may transition
 *  TO, as a bitmask — bit N set means "a transition to mode N is
 *  allowed". A self-transition is allowed only if the mode's own bit
 *  is set, like any other edge. The caller supplies an array of these
 *  to ``migris_mode_init``. */
typedef struct {
    migris_mode_id_t id;
    uint32_t allowed_targets;
} migris_mode_def_t;

/** Mode-manager return / error codes. Framework convention: 0 is
 *  success, a negative value is one of these. */
typedef enum {
    MIGRIS_MODE_OK = 0,
    MIGRIS_MODE_ERR_BAD_ARG = -1,       /**< NULL pointer argument. */
    MIGRIS_MODE_ERR_CAPACITY = -2,      /**< More modes than MIGRIS_MODE_CAPACITY. */
    MIGRIS_MODE_ERR_DUPLICATE = -3,     /**< Repeated mode ID in the init set. */
    MIGRIS_MODE_ERR_RANGE = -4,         /**< Mode ID >= MIGRIS_MODE_ID_MAX, or a target
                                             bit naming an undeclared mode. */
    MIGRIS_MODE_ERR_NOT_FOUND = -5,     /**< Initial / requested mode not declared. */
    MIGRIS_MODE_ERR_FORBIDDEN = -6,     /**< Requested transition not in the rules. */
    MIGRIS_MODE_ERR_BUF_TOO_SMALL = -7, /**< Serialise: output buffer below the image size. */
    MIGRIS_MODE_ERR_TRUNCATED = -8      /**< Deserialise: input shorter than the image. */
} migris_mode_status_t;

/** The mode manager. Caller-owned. Zero-initialise once, then call
 *  ``migris_mode_init``. ``current`` survives a reboot through
 *  lib/nvstore/ (slice fsw-17, ``migris_mode_serialize`` /
 *  ``_deserialize_current``); the declared mode set and ``sink`` are
 *  code-defined every init and not persisted.
 *
 *  ``generation`` is monotonically incremented on every successful
 *  ``migris_mode_request``. The tc_uart sample polls it to detect a
 *  mode change since the last save without re-reading ``current``.
 *  The field is RAM-only: it starts at 0 on every boot, is bumped by
 *  successful transitions, and is NOT included in the on-flash
 *  serialised image. */
typedef struct {
    migris_mode_def_t defs[MIGRIS_MODE_CAPACITY];
    size_t count;
    migris_mode_id_t current;
    /** Optional event sink. When non-NULL, a successful transition is
     *  announced as a PUS-5 MODE_CHANGED event through it. NULL — the
     *  zero-initialised default — means no announcement. */
    const migris_event_sink_t* sink;
    uint32_t generation; /**< Mutation counter; slice fsw-17. */
} migris_mode_manager_t;

/** Initialise ``mgr`` with ``n`` mode definitions copied from
 *  ``defs``, starting in mode ``initial``. ``sink`` may be NULL (no
 *  PUS-5 emission). Validates: ``n`` within capacity; every mode ID
 *  below ``MIGRIS_MODE_ID_MAX``; no duplicate IDs; every set
 *  allowed-target bit naming a declared mode; ``initial`` one of the
 *  declared modes. On any failure the manager is left empty
 *  (``count == 0``) and a negative ``migris_mode_status_t`` is
 *  returned — stateless failure. Returns ``MIGRIS_MODE_OK`` on
 *  success. */
int migris_mode_init(migris_mode_manager_t* mgr,
                     const migris_mode_def_t* defs,
                     size_t n,
                     migris_mode_id_t initial,
                     const migris_event_sink_t* sink);

/** The current mode ID, or 0 if ``mgr`` is NULL (a caller that may
 *  pass NULL must not read meaning into 0). */
migris_mode_id_t migris_mode_current(const migris_mode_manager_t* mgr);

/** Non-zero iff a transition from the current mode to ``target`` is in
 *  the rules. A pure query — no state change, no event. Returns 0 on a
 *  NULL ``mgr``, an out-of-range ``target``, or a forbidden
 *  transition. */
int migris_mode_is_allowed(const migris_mode_manager_t* mgr, migris_mode_id_t target);

/** Request a transition to ``target`` at FSW-clock time
 *  ``now_seconds``. On success the current mode becomes ``target``, a
 *  PUS-5 MODE_CHANGED event (severity info, aux = previous then new
 *  mode ID) is emitted if a sink is wired, and ``MIGRIS_MODE_OK`` is
 *  returned. On failure the current mode is unchanged and nothing is
 *  emitted: ``MIGRIS_MODE_ERR_BAD_ARG`` on a NULL ``mgr``,
 *  ``MIGRIS_MODE_ERR_NOT_FOUND`` if ``target`` is not a declared mode,
 *  ``MIGRIS_MODE_ERR_FORBIDDEN`` if the transition is not in the
 *  rules. */
int migris_mode_request(migris_mode_manager_t* mgr, migris_mode_id_t target, uint32_t now_seconds);

/** Mutation counter — strictly monotonic, bumped on every successful
 *  ``migris_mode_request``. Returns 0 on a NULL manager. Resets to 0
 *  on every ``migris_mode_init`` (the field is NOT persisted — a
 *  fresh boot starts at 0 even after a restored image). */
uint32_t migris_mode_generation(const migris_mode_manager_t* mgr);

/** Serialise the persistable mode state — one byte, the current mode
 *  ID — into ``out`` for the ``lib/nvstore/`` persistence layer. The
 *  declared mode set is NOT persisted (it is code-defined at every
 *  init), nor is the optional ``sink`` (function pointer). Returns
 *  the positive byte count written (always 1), or a negative
 *  ``migris_mode_status_t`` (``_ERR_BUF_TOO_SMALL`` / ``_ERR_BAD_ARG``). */
int migris_mode_serialize(const migris_mode_manager_t* mgr, uint8_t* out, size_t out_cap);

/** Restore the current mode from a previously serialised image. The
 *  restored ID is validated against the declared mode set; if it is
 *  unknown (firmware shipped a new table and dropped the old mode),
 *  the init-time current mode is silently kept and ``MIGRIS_MODE_OK``
 *  is still returned (rollback is non-fatal — the spacecraft must
 *  boot). The ``generation`` counter is NOT bumped, no MODE_CHANGED
 *  event is emitted: a boot restore is not a runtime transition.
 *  Returns ``MIGRIS_MODE_OK`` on success, ``_ERR_BAD_ARG`` on a NULL
 *  argument, or ``_ERR_TRUNCATED`` if the image is empty. */
int migris_mode_deserialize_current(migris_mode_manager_t* mgr, const uint8_t* in, size_t in_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_MODE_MODE_H_
