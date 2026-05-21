/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * On-board packet store — a bounded, RAM-backed buffer of telemetry
 * packets awaiting downlink. Slice fsw-11, the framework's first
 * mass-memory primitive.
 *
 * A spacecraft produces telemetry continuously but has a ground
 * contact only during a pass. The packet store captures every TM
 * packet the FSW emits, each tagged with the FSW-clock time it was
 * stored; on the next pass a ground station retrieves a time window
 * of stored packets and they are downlinked. This store is the
 * mechanism; PUS-15 (lib/pus/pus15.{h,c}) is the ground-facing face
 * (enable / disable storage, retrieve by time window, delete by
 * time), and the application's main loop both feeds the store (one
 * call per emitted packet) and drains an armed retrieval.
 *
 * The store is a circular buffer: when full, storing a new packet
 * overwrites the OLDEST — the most recent telemetry is always kept.
 * Packets are stored in non-decreasing time order (the FSW clock is
 * monotonic), so the buffer is time-ordered, oldest first.
 *
 * A retrieval is armed over a [from, to] time window; the main loop
 * then drains it one packet per iteration. While a retrieval is
 * active the store is FROZEN — migris_pktstore_store and
 * migris_pktstore_delete_up_to are no-ops / rejected — so the buffer
 * cannot shift under the retrieval cursor. A retrieval is
 * non-destructive; deletion is the separate migris_pktstore_delete_up_to.
 *
 * RAM-only and volatile: the store is empty after every reboot.
 * (Non-volatile mass memory across reset is a future capability — it
 * needs a flash storage subsystem and is out of this slice.) Capacity
 * and the per-packet size are compile-time constants — freestanding,
 * no malloc. Freestanding C — no Zephyr, no stdlib.
 *
 * Byte-level specification of the PUS-15 wire: docs/wire/pus-15.md.
 */

#ifndef MIGRIS_FSW_PKTSTORE_PKTSTORE_H_
#define MIGRIS_FSW_PKTSTORE_PKTSTORE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the packet store, in packets. Compile-time constant so
 *  the store is statically sized (freestanding, no malloc). Override
 *  with ``-DMIGRIS_PKTSTORE_CAPACITY=<n>`` (the `tc_uart` sample wires
 *  this to Kconfig ``FSW_PKTSTORE_CAPACITY``). */
#ifndef MIGRIS_PKTSTORE_CAPACITY
#    define MIGRIS_PKTSTORE_CAPACITY 32U
#endif

/** Largest telemetry packet, in bytes, the store holds. A single
 *  CCSDS Space Packet on the Migris TM interface is well under this.
 *  Override with ``-DMIGRIS_PKTSTORE_PACKET_MAX=<n>`` (Kconfig
 *  ``FSW_PKTSTORE_PACKET_MAX`` in the sample). */
#ifndef MIGRIS_PKTSTORE_PACKET_MAX
#    define MIGRIS_PKTSTORE_PACKET_MAX 128U
#endif

/** One stored telemetry packet, held verbatim with the FSW-clock time
 *  it was stored. */
typedef struct {
    uint32_t storage_time;                      /**< CUC coarse seconds at the time of storage. */
    uint8_t packet[MIGRIS_PKTSTORE_PACKET_MAX]; /**< The TM packet, verbatim. */
    size_t packet_len;                          /**< Packet length in bytes. */
} migris_pktstore_entry_t;

/** The packet store. Caller-owned, RAM-only and volatile — empty
 *  after a reboot. Zero-initialise once, then call
 *  ``migris_pktstore_init``. A circular buffer: ``head`` is the oldest
 *  entry, ``count`` the number stored. ``retrieval_*`` describe the
 *  at-most-one in-progress time-window retrieval. */
typedef struct {
    migris_pktstore_entry_t entries[MIGRIS_PKTSTORE_CAPACITY];
    size_t head;
    size_t count;
    int enabled; /**< 0 = storage off, 1 = on (the post-init default). */
    int retrieval_active;
    uint32_t retrieval_end;  /**< Inclusive end of the active retrieval window. */
    size_t retrieval_cursor; /**< Offset from ``head`` of the next entry to deliver. */
} migris_pktstore_t;

/** Packet-store return / error codes. Same convention as the rest of
 *  the framework: 0 (or a positive result) is success, negative is
 *  one of these. */
typedef enum {
    MIGRIS_PKTSTORE_OK = 0,
    MIGRIS_PKTSTORE_ERR_BAD_ARG = -1,          /**< NULL pointer, empty / inverted argument. */
    MIGRIS_PKTSTORE_ERR_PACKET_TOO_LARGE = -2, /**< Packet over MIGRIS_PKTSTORE_PACKET_MAX. */
    MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE =
        -3,                                /**< Operation rejected: a retrieval is in progress. */
    MIGRIS_PKTSTORE_ERR_BUF_TOO_SMALL = -4 /**< Output buffer below the retrieved packet. */
} migris_pktstore_status_t;

/** Reset ``store`` to empty, storage ENABLED, no retrieval. The store
 *  defaults enabled so telemetry is captured from boot — it only
 *  records, so leaving it on is harmless and avoids losing early TM.
 *  A zero-initialised ``migris_pktstore_t`` is empty but
 *  storage-disabled; call this for the enabled default. */
void migris_pktstore_init(migris_pktstore_t* store);

/** Set the storage state: non-zero ``enabled`` lets
 *  ``migris_pktstore_store`` capture packets, zero suspends capture. */
void migris_pktstore_set_enabled(migris_pktstore_t* store, int enabled);

/** Non-zero iff storage is enabled. */
int migris_pktstore_is_enabled(const migris_pktstore_t* store);

/** Number of packets currently stored. */
size_t migris_pktstore_count(const migris_pktstore_t* store);

/** Non-zero iff a time-window retrieval is in progress. */
int migris_pktstore_retrieval_active(const migris_pktstore_t* store);

/** Store a copy of ``packet`` (``packet_len`` bytes) tagged with
 *  ``storage_time``. Returns 1 if stored; 0 if not stored because
 *  storage is disabled or a retrieval is in progress (not an error —
 *  the expected best-effort behaviour); a negative
 *  ``migris_pktstore_status_t`` on a NULL / zero-length packet
 *  (``ERR_BAD_ARG``) or one over ``MIGRIS_PKTSTORE_PACKET_MAX``
 *  (``ERR_PACKET_TOO_LARGE``). When the store is full the oldest
 *  packet is overwritten. ``storage_time`` must be non-decreasing
 *  across calls (the FSW clock is monotonic). */
int migris_pktstore_store(migris_pktstore_t* store,
                          const uint8_t* packet,
                          size_t packet_len,
                          uint32_t storage_time);

/** Delete every stored packet whose ``storage_time`` is at or before
 *  ``time_seconds``. Returns the number of packets deleted (>= 0), or
 *  a negative ``migris_pktstore_status_t`` — ``ERR_BAD_ARG`` on NULL,
 *  ``ERR_RETRIEVAL_ACTIVE`` if a retrieval is in progress. */
int migris_pktstore_delete_up_to(migris_pktstore_t* store, uint32_t time_seconds);

/** Arm a retrieval over the inclusive time window [``from_time``,
 *  ``to_time``]: subsequent ``migris_pktstore_retrieve_next`` calls
 *  deliver, in storage order, each stored packet whose
 *  ``storage_time`` falls in the window. Returns ``MIGRIS_PKTSTORE_OK``,
 *  ``MIGRIS_PKTSTORE_ERR_BAD_ARG`` (NULL, or ``from_time`` after
 *  ``to_time``), or ``MIGRIS_PKTSTORE_ERR_RETRIEVAL_ACTIVE`` if a
 *  retrieval is already in progress. */
int migris_pktstore_arm_retrieval(migris_pktstore_t* store, uint32_t from_time, uint32_t to_time);

/** Deliver the next packet of the active retrieval into ``out``
 *  (capacity ``out_cap``), writing its length to ``*out_len``.
 *  Returns 1 if a packet was delivered; 0 if no retrieval is active or
 *  the window is exhausted (the retrieval is then cleared); a negative
 *  ``migris_pktstore_status_t`` on a NULL argument or if ``out_cap``
 *  is below the next packet's length (the retrieval is left in place).
 *  Call once per main-loop iteration. */
int migris_pktstore_retrieve_next(migris_pktstore_t* store,
                                  uint8_t* out,
                                  size_t out_cap,
                                  size_t* out_len);

/** Report the stored time span: writes the oldest and newest
 *  ``storage_time`` to ``*oldest`` / ``*newest`` and returns 1 if the
 *  store holds at least one packet, or returns 0 (leaving the outputs
 *  untouched) if it is empty or an argument is NULL. */
int migris_pktstore_span(const migris_pktstore_t* store, uint32_t* oldest, uint32_t* newest);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MIGRIS_FSW_PKTSTORE_PKTSTORE_H_
