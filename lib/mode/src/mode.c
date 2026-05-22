/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board operating-mode manager — a generic mode state machine.
 * See migris/fsw/mode/mode.h for the contract and rationale.
 *
 * The manager holds the current mode and a caller-supplied table of
 * allowed transitions (one allowed-target bitmask per mode). A
 * transition request is checked against that table; a successful
 * change is announced as a PUS-5 MODE_CHANGED event through the
 * optional event sink.
 */

#include "migris/fsw/mode/mode.h"

#include "migris/fsw/event_sink.h"
#include "migris/fsw/pus/pus5.h"

#include <stddef.h>
#include <stdint.h>

/* Index of the mode with ID `id` among the first `count` definitions,
 * or -1 if none. Pulled out so the validation and lookup paths stay
 * flat (clang-tidy cognitive complexity). */
static int mode_index_of(const migris_mode_def_t* defs, size_t count, migris_mode_id_t id) {
    for (size_t i = 0U; i < count; ++i) {
        if (defs[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* Non-zero iff every mode named by definition `i`'s allowed-target
 * bitmask is itself a declared mode. */
static int mode_targets_declared(const migris_mode_def_t* defs, size_t n, size_t i) {
    for (migris_mode_id_t t = 0U; t < (migris_mode_id_t)MIGRIS_MODE_ID_MAX; ++t) {
        const int named = (int)((defs[i].allowed_targets >> t) & 1U);
        if (named != 0 && mode_index_of(defs, n, t) < 0) {
            return 0;
        }
    }
    return 1;
}

/* Validate the caller's init set: every mode ID in range, no duplicate
 * IDs, every allowed-target bit naming a declared mode. Returns
 * MIGRIS_MODE_OK or a negative code. */
static int mode_validate(const migris_mode_def_t* defs, size_t n) {
    for (size_t i = 0U; i < n; ++i) {
        if (defs[i].id >= (migris_mode_id_t)MIGRIS_MODE_ID_MAX) {
            return MIGRIS_MODE_ERR_RANGE;
        }
        if (mode_index_of(defs, i, defs[i].id) >= 0) {
            return MIGRIS_MODE_ERR_DUPLICATE;
        }
    }
    /* Target bits are checked once the whole declared set is known. */
    for (size_t i = 0U; i < n; ++i) {
        if (mode_targets_declared(defs, n, i) == 0) {
            return MIGRIS_MODE_ERR_RANGE;
        }
    }
    return MIGRIS_MODE_OK;
}

int migris_mode_init(migris_mode_manager_t* mgr,
                     const migris_mode_def_t* defs,
                     size_t n,
                     migris_mode_id_t initial,
                     const migris_event_sink_t* sink) {
    if (mgr == NULL || (defs == NULL && n > 0U)) {
        return MIGRIS_MODE_ERR_BAD_ARG;
    }
    /* Empty until validation passes — a failed init leaves no
     * half-populated manager behind (stateless failure). */
    mgr->count = 0U;
    mgr->current = 0U;
    mgr->sink = NULL;
    if (n > MIGRIS_MODE_CAPACITY) {
        return MIGRIS_MODE_ERR_CAPACITY;
    }
    const int rc = mode_validate(defs, n);
    if (rc != MIGRIS_MODE_OK) {
        return rc;
    }
    if (mode_index_of(defs, n, initial) < 0) {
        return MIGRIS_MODE_ERR_NOT_FOUND;
    }
    for (size_t i = 0U; i < n; ++i) {
        mgr->defs[i] = defs[i];
    }
    mgr->count = n;
    mgr->current = initial;
    mgr->sink = sink;
    return MIGRIS_MODE_OK;
}

migris_mode_id_t migris_mode_current(const migris_mode_manager_t* mgr) {
    return (mgr == NULL) ? (migris_mode_id_t)0U : mgr->current;
}

int migris_mode_is_allowed(const migris_mode_manager_t* mgr, migris_mode_id_t target) {
    if (mgr == NULL || target >= (migris_mode_id_t)MIGRIS_MODE_ID_MAX) {
        return 0;
    }
    const int idx = mode_index_of(mgr->defs, mgr->count, mgr->current);
    if (idx < 0) {
        return 0; /* current mode not declared — only on an uninitialised manager */
    }
    /* Validation guarantees a set target bit names a declared mode, so
     * a set bit is both "in the rules" and "target is a real mode". */
    return (int)((mgr->defs[idx].allowed_targets >> target) & 1U);
}

int migris_mode_request(migris_mode_manager_t* mgr, migris_mode_id_t target, uint32_t now_seconds) {
    if (mgr == NULL) {
        return MIGRIS_MODE_ERR_BAD_ARG;
    }
    if (target >= (migris_mode_id_t)MIGRIS_MODE_ID_MAX ||
        mode_index_of(mgr->defs, mgr->count, target) < 0) {
        return MIGRIS_MODE_ERR_NOT_FOUND;
    }
    if (migris_mode_is_allowed(mgr, target) == 0) {
        return MIGRIS_MODE_ERR_FORBIDDEN;
    }

    const migris_mode_id_t from = mgr->current;
    mgr->current = target;

    /* Announce the change as a spontaneous PUS-5 informative event:
     * aux is the previous mode ID then the new one. */
    if (mgr->sink != NULL && mgr->sink->report != NULL) {
        const uint8_t aux[2] = {from, target};
        (void)mgr->sink->report(mgr->sink->self,
                                now_seconds,
                                MIGRIS_PUS5_SEV_INFO,
                                MIGRIS_PUS5_EVT_MODE_CHANGED,
                                aux,
                                sizeof aux);
    }
    return MIGRIS_MODE_OK;
}
