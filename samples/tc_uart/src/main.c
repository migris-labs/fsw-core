/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * TC reception + verification + FDIR + on-board parameter, schedule
 * and storage management sample.
 *
 * Boots Zephyr on nucleo_h753zi, emits one spontaneous PUS-5[1]
 * "FSW boot" informative event (the first TM the FSW produces, the
 * slice fsw-6 demonstration of asynchronous, non-TC-triggered TM),
 * then listens on USART3 for a CCSDS Space Packet, hands it to the
 * TC router, and writes back whatever verification / service TM the
 * router produces. With PUS-1 in place a single inbound TC can yield
 * up to three packets — a PUS-1[1] acceptance report, the PUS-17[2]
 * service response, and a PUS-1[7] completion report — depending on
 * the TC's ack flags. The boot event consumes the first per-APID
 * sequence count, so the first TC response starts at count 1. Wire
 * format is pinned in docs/wire/pus-5.md, docs/wire/pus-1.md and
 * docs/wire/pus-17.md.
 *
 * Architecture (slice-minimum):
 *
 *   USART3 RX IRQ  ──fifo_read──▶  ring_buf  ──ring_buf_get──▶  main loop
 *                                                                  │
 *                                                                  ▼
 *                                                  migris_tc_router_dispatch()
 *                                                                  │
 *                                                                  ▼
 *                                                   uart_poll_out  ◀── out[]
 *
 * Slice fsw-8 adds FDIR: the TC router reports a rejected TC to an
 * FDIR event sink, and the main loop also detects UART RX-ring
 * overflow, both feeding a bounded event FIFO. Once per iteration the
 * loop drains one FIFO record into a spontaneous PUS-5 anomaly report
 * (drained *after* a TC's PUS-1 verification, so the ack precedes the
 * anomaly on the wire). The FIFO is single-context: every producer
 * runs in the main loop; the RX ISR only bumps its own counter.
 *
 * Slice fsw-9 adds an on-board parameter datapool and PUS-20. The
 * datapool holds the PUS-3 housekeeping period as a read-write
 * parameter (framework ID 0x0001), seeded from Kconfig; a routed
 * PUS-20 TC reports it ([20,1]) or sets it ([20,3]). The main loop
 * reads the period from the datapool every iteration, so a PUS-20[3]
 * set reconfigures the housekeeping cadence live, with no rebuild — a
 * period of 0 disables periodic housekeeping. Wire format is pinned
 * in docs/wire/pus-20.md.
 *
 * Slice fsw-10 adds an on-board schedule and PUS-11. A routed PUS-11
 * TC enables / disables the schedule, resets it, inserts time-tagged
 * telecommands, deletes them, or summary-reports them. Each main-loop
 * iteration the loop releases at most one activity whose absolute
 * release time has been reached — re-dispatching its stored TC
 * through the router exactly as if it had just arrived — so a ground
 * station can load a pass's commands and let them run autonomously.
 * Wire format is pinned in docs/wire/pus-11.md.
 *
 * Slice fsw-11 adds an on-board packet store and PUS-15. Every live
 * TM packet the FSW emits is tapped into a RAM-backed circular store
 * (transmit_tm below); a routed PUS-15 TC enables / disables storage,
 * reports the store, deletes a time range, or arms a by-time-window
 * retrieval. Each main-loop iteration drains at most one packet of an
 * armed retrieval, re-emitting it verbatim — a replay of stored
 * history, so replayed packets are not tapped back in. The store is
 * RAM-backed and volatile (empty after a reboot). Wire format is
 * pinned in docs/wire/pus-15.md.
 *
 * Slice fsw-13 adds an operating-mode manager. When
 * CONFIG_FSW_MODE_DEMO is set, the sample defines a small mode set
 * (boot, nominal, safe) and its allowed transitions, then performs one
 * transition at boot — boot to nominal — which the mode manager
 * announces as a spontaneous PUS-5 MODE_CHANGED event through the FDIR
 * sink. The mode manager has no inbound telecommand (a ground
 * mode-commanding service is downstream), so the demo is the
 * observable surface for the closed-loop test; like the fsw-12
 * large-data demo it is off in the verification-stream build. Wire
 * format of the event is pinned in docs/wire/pus-5.md.
 *
 * Slice fsw-12 adds a PUS-13 large-data downlink. When
 * CONFIG_FSW_LARGEDATA_DEMO is set, the sample starts one transfer of
 * a synthetic data unit at boot and, each main-loop iteration, emits
 * one [13,1] / [13,2] / [13,3] part — a unit too large for a single
 * Space Packet, dripped out one packet per iteration and reassembled
 * from the part header on the ground. PUS-13 has no inbound subtype,
 * so the demo is the observable surface for the closed-loop test; it
 * is left off in the verification-stream build, whose tests read a
 * fixed byte count per stimulus. Parts are live TM, tapped into the
 * packet store like any other emitted telemetry. Wire format is
 * pinned in docs/wire/pus-13.md.
 *
 * Slice fsw-14 completes the FDIR story with isolation and recovery.
 * When CONFIG_FSW_FDIR_RECOVERY_DEMO is set, FDIR counts rejected
 * telecommands; once they cross a confirmation threshold it declares
 * the fault confirmed, emits a high-severity PUS-5 FDIR_RECOVERY
 * event, and autonomously commands the mode manager to SAFE — a
 * genuine closed loop a ground test drives by sending malformed TCs.
 * Recovery is gated on the same build as the mode demo and is off in
 * the verification-stream build. Wire format is pinned in
 * docs/wire/pus-5.md.
 *
 * Slice fsw-15 adds dynamic PUS-3 housekeeping structures. A routed
 * PUS-3 [3,1]/[3,2]/[3,5]/[3,6] TC creates, deletes, enables and
 * disables ground-defined housekeeping structures, each selecting a
 * list of datapool parameters; the main loop's emission tick turns an
 * enabled, due structure into a datapool-backed PUS-3[25] report. The
 * sample registers two read-only datapool parameters (a firmware
 * version and a build identifier) so a ground-created structure has
 * parameters to sample. No demo gate is needed — structure management
 * is TC-driven and a created structure starts disabled, so the
 * verification-stream build emits no unsolicited structure telemetry.
 * The frozen FRAMEWORK_DIAG report is unchanged. Wire format is pinned
 * in docs/wire/pus-3.md.
 *
 * Slice fsw-16 adds non-volatile parameter storage. The board's
 * 256 KB storage_partition (= two STM32H7 flash sectors) becomes an
 * A/B-redundant NVM image managed by lib/nvstore/, with the Zephyr
 * flash_area_* backend in nv_flash_backend.c. At boot the datapool's
 * persisted values are restored over the Kconfig defaults; in the main
 * loop, a successful PUS-20[3] set bumps the datapool's generation
 * counter and the next iteration's save tick writes the new image to
 * the older flash sector — one coalesced write per batch of sets. A
 * power loss mid-write leaves the previous (intact) copy in the other
 * sector. No new PUS service, no wire change; persistence rides on the
 * existing PUS-20 set/report. On-flash image format is pinned in
 * docs/nv-image-format.md.
 *
 * Slice fsw-17 extends that persistence to three more subsystems: the
 * on-board schedule (PUS-11 activities + the enabled flag), the
 * housekeeping-structure store (PUS-3 dynamic structure definitions),
 * and the operating-mode manager (the current mode). Each owns a new
 * nvstore record type (SCHEDULE = 2, HKSTORE = 3, MODE = 4) and a
 * generation counter the loop's save tick polls through the shared
 * `nv_autosave_tick` helper. A single `migris_nvstore_save` at the
 * end of the tick coalesces any combination of advances into one
 * flash write. The boot-time `BOOT → NOMINAL` demo transition is now
 * gated on the post-restore current still being BOOT, so a
 * spacecraft persisted in SAFE (by an FDIR recovery, say) outlives a
 * reboot rather than being silently undone.
 *
 * Single producer (the RX IRQ) and single consumer (main thread)
 * make ring_buf safe without explicit locking. We send TM with
 * blocking ``uart_poll_out`` — this slice has no concurrent TX
 * pressure, so interrupt-driven TX would be premature.
 *
 * Framing is CCSDS-only: the receiver reads the 6-byte primary
 * header, decodes Packet Data Length, then reads exactly that many
 * more bytes. A declared length larger than our receive buffer means
 * we are out of sync (or being sent something we cannot buffer) —
 * reset and re-listen from byte 0. There is no resynchronisation
 * magic byte (CCSDS provides none); over a lossless emulated UART
 * this is sufficient. Real RF relies on the AOS/TC transfer-frame
 * ASM one layer down, not on the Space Packet layer.
 */

#include "migris/fsw/datapool/datapool.h"
#include "migris/fsw/fdir/fdir.h"
#include "migris/fsw/hkstore/hkstore.h"
#include "migris/fsw/largedata/largedata.h"
#include "migris/fsw/mode/mode.h"
#include "migris/fsw/nvstore/nvstore.h"
#include "migris/fsw/pktstore/pktstore.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus11.h"
#include "migris/fsw/pus/pus15.h"
#include "migris/fsw/pus/pus20.h"
#include "migris/fsw/pus/pus3.h"
#include "migris/fsw/pus/pus5.h"
#include "migris/fsw/pus/tc_router.h"
#include "migris/fsw/schedule/schedule.h"

#include "nv_flash_backend.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

/* USART3 is the platform-pinned UART (same as the fsw-2 hello
 * sample). The board's ``zephyr,console`` choice points there by
 * default; we leave that alone — boot banner / printk / log are all
 * disabled in prj.conf so nothing else writes to it. */
#define UART_NODE DT_NODELABEL(usart3)
static const struct device* const uart_dev = DEVICE_DT_GET(UART_NODE);

/* APID 0x100 — see docs/wire/pus-17.md. The sample app is its own
 * application process; downstream missions allocate their own. */
#define MIGRIS_FSW_APID 0x100U

/* Framework datapool parameter IDs (reserved range 0x0001..0x00FF —
 * see docs/wire/pus-20.md). The PUS-3 housekeeping period is
 * operator-tunable via PUS-20; the firmware-identity parameters are
 * read-only observability values a ground-created PUS-3 housekeeping
 * structure (slice fsw-15) can sample. */
#define MIGRIS_FSW_PARAM_HK_PERIOD_SEC 0x0001U
#define MIGRIS_FSW_PARAM_FW_VERSION 0x0002U  /* read-only u16, major<<8 | minor */
#define MIGRIS_FSW_PARAM_FW_BUILD_ID 0x0003U /* read-only u32, build identifier */

/* Firmware-identity values surfaced through the read-only datapool
 * parameters above. The version mirrors the migris-fsw-core project
 * version; the build identifier is the slice date (YYYYMMDD, hex). */
#define MIGRIS_FSW_FW_VERSION 0x0001U
#define MIGRIS_FSW_FW_BUILD_ID 0x20260522U

/* Largest TC we will buffer. The biggest in the baseline is a
 * PUS-11[4] insert carrying scheduled telecommands; 192 admits an
 * insert of a couple of maximum-size activities. A declared length
 * beyond this is treated as desync. */
#define TC_BUF_SIZE 192U

/* Ring-buffer between the UART RX ISR and the main thread. 128 bytes
 * is ~10× a PUS-17[1] TC; comfortably absorbs a burst even with main
 * pre-empted by other Zephyr work. */
#define RX_RING_SIZE 128
RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);

/* UART RX-ring overflow drop count. Written only by the RX ISR,
 * read (snapshotted) only by the main loop. A naturally-aligned
 * 32-bit access is atomic on Cortex-M7 and telemetry only needs a
 * recent value, so single-writer / single-reader + `volatile` (to
 * defeat caching across loop iterations) is the minimal correct
 * mechanism — no atomic_t, no irq-lock. The ISR deliberately does
 * *not* touch the FDIR event FIFO: the main loop observes the delta
 * of this counter and is the sole FIFO producer, keeping the FIFO
 * single-context and non-atomic (slice fsw-8). Surfaced both as the
 * cumulative PUS-3 housekeeping counter and as a PUS-5 RX_OVERFLOW
 * anomaly. */
static volatile uint32_t rx_ring_overflow_drops;

/* On-board schedule (slice fsw-10). A routed PUS-11 TC inserts
 * time-tagged telecommands here; the main loop's release tick
 * dispatches due ones back through the router. File-scope because the
 * store holds up to MIGRIS_SCHEDULE_CAPACITY telecommands — too large
 * for the main() stack. */
static migris_schedule_t schedule;

/* On-board packet store (slice fsw-11). Every live TM packet the FSW
 * emits is tapped into this RAM-backed circular buffer by transmit_tm
 * below; a routed PUS-15 TC arms a by-time retrieval the main loop
 * drains. File-scope because the store holds up to
 * MIGRIS_PKTSTORE_CAPACITY packets — far too large for the main()
 * stack. RAM-only and volatile: empty after every reboot. */
static migris_pktstore_t store;

/* On-board housekeeping-structure store (slice fsw-15). A routed PUS-3
 * [3,1]/[3,2]/[3,5]/[3,6] TC creates, deletes, enables and disables
 * ground-defined housekeeping structures here; the emission tick in
 * the main loop turns an enabled, due structure into a datapool-backed
 * PUS-3[25] report. File-scope because the store holds up to
 * MIGRIS_HKSTORE_CAPACITY structures — too large for the main() stack.
 * RAM-only and volatile: empty after every reboot. */
static migris_hkstore_t hkstore;

/* Non-volatile parameter store (slice fsw-16, extended in fsw-17). On
 * boot, persisted records are restored from flash for the datapool,
 * the schedule, the housekeeping-structure store and the operating-
 * mode manager. In the main loop, each subsystem's generation counter
 * is polled and any record whose generation advanced is re-serialised
 * into the nvstore image; one coalesced flash save lands the new
 * image at the end of the tick. File-scope because the in-RAM payload
 * buffer alone is ~1.5 KB (too large for the main() stack), and the
 * backend keeps file-scope state of its own. The per-subsystem
 * scratch buffers are sized to each codec's worst case so the helper
 * never has to truncate. */
static migris_nvstore_t nvstore;
static migris_nv_backend_t nv_backend;
static uint8_t nv_buf_dp[64];      /* datapool record is ~20 B today */
static uint8_t nv_buf_sched[1200]; /* schedule worst case ≈ 3 + 16 * (4+2+TC_MAX) */
static uint8_t nv_buf_hk[256];     /* hkstore worst case ≈ 2 + CAPACITY * (8 + 2*MAX_PARAMS) */
static uint8_t nv_buf_mode[8];     /* mode record is exactly 1 byte */

/* Serialiser signature shared by the four persisted subsystems —
 * every subsystem's serialize matches it. Used by nv_autosave_tick to
 * dispatch a per-record save without four near-identical copies of the
 * "if generation advanced, serialise + put" block (cognitive
 * complexity in main() would otherwise blow out — see CHANGELOG note
 * on the fsw-17 helper). */
typedef int (*nv_serialize_fn)(const void* obj, uint8_t* out, size_t out_cap);

/* If `cur_gen` has advanced since `*last_saved_gen`, serialise `obj`
 * into the nvstore image as `record_type` and update `*last_saved_gen`
 * to `cur_gen`. Does NOT call migris_nvstore_save — the caller
 * coalesces multiple put()s into one save per loop iteration. Returns
 * non-zero iff a record was put (i.e. the image is dirty and needs a
 * subsequent save). */
static int nv_autosave_tick(uint8_t record_type,
                            nv_serialize_fn serialize,
                            const void* obj,
                            uint32_t cur_gen,
                            uint32_t* last_saved_gen,
                            uint8_t* buf,
                            size_t buf_cap) {
    if (cur_gen == *last_saved_gen) {
        return 0;
    }
    const int n = serialize(obj, buf, buf_cap);
    if (n <= 0) {
        return 0;
    }
    if (migris_nvstore_put(&nvstore, record_type, buf, (uint16_t)n) != MIGRIS_NVSTORE_OK) {
        return 0;
    }
    *last_saved_gen = cur_gen;
    return 1;
}

/* Adapter shims so the four C-typed serialisers match nv_serialize_fn
 * exactly — pointer-to-typed-struct -> pointer-to-void is not
 * implicit in C, and the strict-aliasing-safe casts live here in one
 * place rather than at every call site. */
static int nv_ser_datapool(const void* obj, uint8_t* out, size_t out_cap) {
    return migris_datapool_serialize((const migris_datapool_t*)obj, out, out_cap);
}

static int nv_ser_schedule(const void* obj, uint8_t* out, size_t out_cap) {
    return migris_schedule_serialize((const migris_schedule_t*)obj, out, out_cap);
}

static int nv_ser_hkstore(const void* obj, uint8_t* out, size_t out_cap) {
    return migris_hkstore_serialize((const migris_hkstore_t*)obj, out, out_cap);
}
#ifdef CONFIG_FSW_MODE_DEMO
static int nv_ser_mode(const void* obj, uint8_t* out, size_t out_cap) {
    return migris_mode_serialize((const migris_mode_manager_t*)obj, out, out_cap);
}
#endif

#ifdef CONFIG_FSW_LARGEDATA_DEMO
/* On-board large-data downlink session (slice fsw-12). PUS-13 has no
 * inbound telecommand, so — unlike PUS-11 or PUS-15 — a closed-loop
 * test cannot trigger a transfer; the sample instead starts one at
 * boot over a synthetic data unit, dripped out one part per main-loop
 * iteration. The unit is a byte ramp so the ground side can verify
 * the reassembled bytes. File-scope: the session borrows the unit, so
 * both must outlive the main loop. */
#    define LARGEDATA_DEMO_UNIT_LEN 200U
#    define LARGEDATA_DEMO_TRANSACTION_ID 0x0001U
static migris_largedata_session_t largedata;
static uint8_t largedata_demo_unit[LARGEDATA_DEMO_UNIT_LEN];
#endif

#ifdef CONFIG_FSW_MODE_DEMO
/* On-board operating-mode manager (slice fsw-13). The mode manager has
 * no inbound telecommand — a ground mode-commanding service is
 * downstream (cry4-fsw) — so the sample drives it directly: it defines
 * a small mode set and performs one demo transition at boot, which the
 * manager announces as a spontaneous PUS-5 MODE_CHANGED event.
 * File-scope for consistency with the other on-board stores. */
#    define FSW_MODE_BOOT 0U
#    define FSW_MODE_NOMINAL 1U
#    define FSW_MODE_SAFE 2U
static migris_mode_manager_t mode_mgr;
#endif

static void uart_isr(const struct device* dev, void* user_data) {
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        uint8_t byte = 0;
        const int n = uart_fifo_read(dev, &byte, 1);
        if (n != 1) {
            break;
        }
        /* Drop bytes when the ring fills — this is a smoke test, not
         * a flow-controlled link. The drop is *counted* here only; the
         * main loop turns an increase of this counter into a PUS-5
         * RX_OVERFLOW anomaly (slice fsw-8). The ISR stays free of the
         * FIFO so it remains single-context and non-atomic. */
        if (ring_buf_put(&rx_ring, &byte, 1) != 1U) {
            rx_ring_overflow_drops++;
        }
    }
}

static void uart_tx_blocking(const struct device* dev, const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uart_poll_out(dev, buf[i]);
    }
}

/* Transmit a live TM burst and tap every packet in it into the
 * packet store (slice fsw-11). A single inbound TC can yield a burst
 * of several back-to-back CCSDS packets (acceptance + service +
 * completion); this walks the burst by each packet's primary-header
 * Packet Data Length and stores each one verbatim, tagged with
 * ``store_time``. A packet the store rejects (storage disabled, a
 * retrieval in progress, or over MIGRIS_PKTSTORE_PACKET_MAX) is
 * silently skipped — best-effort capture is the contract. Replayed
 * packets drained from a retrieval go out via plain uart_tx_blocking
 * instead: a replay of stored history must not be re-stored. */
static void
transmit_tm(const struct device* dev, uint32_t store_time, const uint8_t* burst, size_t len) {
    uart_tx_blocking(dev, burst, len);
    size_t pos = 0U;
    while ((pos + MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) <= len) {
        const uint16_t data_length =
            (uint16_t)(((uint16_t)burst[pos + 4U] << 8) | (uint16_t)burst[pos + 5U]);
        const size_t pkt_size = migris_ccsds_packet_total_size(data_length);
        if ((pos + pkt_size) > len) {
            break;
        }
        (void)migris_pktstore_store(&store, &burst[pos], pkt_size, store_time);
        pos += pkt_size;
    }
}

/* Build the framework PUS-3 housekeeping parameter snapshot for the
 * *spontaneous* periodic report. Both this and the router's [27]-poll
 * path now read the same router-owned PUS-5 counters (slice fsw-8). */
static void fill_hk_params(const migris_tc_router_ctx_t* router, migris_pus3_hk_params_t* p) {
    for (size_t i = 0U; i < 4U; ++i) {
        p->pus1_msg_counter[i] = router->pus1.msg_counter[i];
        p->pus5_msg_counter[i] = router->pus5.msg_counter[i];
    }
    p->pus17_tm_msg_counter = router->pus17.tm_msg_counter;
    p->tc_accepted_count = router->tc_accepted_count;
    p->tc_rejected_count = router->tc_rejected_count;
    p->rx_ring_overflow_drops = router->rx_ring_overflow_drops;
}

int main(void) {
    if (!device_is_ready(uart_dev)) {
        /* Nothing to do without a UART — go idle. */
        for (;;) {
            k_sleep(K_FOREVER);
        }
    }

    uart_irq_rx_disable(uart_dev);
    uart_irq_tx_disable(uart_dev);
    uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
    uart_irq_rx_enable(uart_dev);

    migris_tc_router_ctx_t ctx = {
        .apid = MIGRIS_FSW_APID,
    };

    /* FDIR: detection + event reporting. The router reports a rejected
     * TC through this sink; the loop also feeds it RX-overflow events.
     * Both land in the bounded FIFO the loop drains into PUS-5. */
    migris_fdir_ctx_t fdir;
    migris_fdir_init(&fdir);
    const migris_event_sink_t fdir_sink = migris_fdir_event_sink(&fdir);
    ctx.sink = &fdir_sink;

    /* Slice fsw-9: the on-board parameter datapool. fsw-core hard-codes
     * no parameters — the application supplies the set. This sample
     * registers one: the PUS-3 housekeeping period, made operator-
     * tunable through PUS-20, seeded from Kconfig. The descriptor set
     * is a fixed, compile-time-correct constant (one read-write u32
     * parameter), so the init cannot fail. */
    const migris_dp_param_t dp_params[] = {
        {MIGRIS_FSW_PARAM_HK_PERIOD_SEC,
         MIGRIS_DP_ACCESS_READ_WRITE,
         migris_dp_u32((uint32_t)CONFIG_FSW_PUS3_HK_PERIOD_SEC)},
        {MIGRIS_FSW_PARAM_FW_VERSION,
         MIGRIS_DP_ACCESS_READ_ONLY,
         migris_dp_u16(MIGRIS_FSW_FW_VERSION)},
        {MIGRIS_FSW_PARAM_FW_BUILD_ID,
         MIGRIS_DP_ACCESS_READ_ONLY,
         migris_dp_u32(MIGRIS_FSW_FW_BUILD_ID)},
    };
    migris_datapool_t datapool;
    (void)migris_datapool_init(&datapool, dp_params, sizeof(dp_params) / sizeof(dp_params[0]));
    ctx.datapool = &datapool;

    /* Slice fsw-16: open the board's storage_partition through the
     * Zephyr flash backend and attach it to the nvstore. The actual
     * restore for each persisted subsystem happens further down,
     * after that subsystem is initialised — order matters and we want
     * the load to land on a clean post-init state. */
    nv_backend = migris_fsw_nv_flash_backend();
    migris_nvstore_init(&nvstore, &nv_backend);
    const int nv_load_rc = migris_nvstore_load(&nvstore);

    /* Slice fsw-16 (extended in fsw-17): restore the datapool over
     * the Kconfig defaults seeded above. Best-effort: a missing or
     * corrupted image leaves the datapool at its defaults, exactly
     * the first-boot behaviour. */
    if (nv_load_rc == MIGRIS_NVSTORE_OK) {
        const uint8_t* dp_bytes = NULL;
        uint16_t dp_len = 0U;
        if (migris_nvstore_get(&nvstore, MIGRIS_NVSTORE_RECORD_DATAPOOL, &dp_bytes, &dp_len) ==
            MIGRIS_NVSTORE_OK) {
            (void)migris_datapool_deserialize(&datapool, dp_bytes, dp_len);
        }
    }

    /* Slice fsw-10: the on-board schedule. A routed PUS-11 TC inserts
     * time-tagged telecommands; the release tick in the loop below
     * dispatches due ones. The schedule starts disabled — ground
     * enables it with a PUS-11[1]. fsw-17: the activities array and
     * enabled flag are restored from the nvstore SCHEDULE record (if
     * one was loaded), so a pass's worth of uploaded commands survives
     * a reboot. */
    migris_schedule_init(&schedule);
    if (nv_load_rc == MIGRIS_NVSTORE_OK) {
        const uint8_t* sched_bytes = NULL;
        uint16_t sched_len = 0U;
        if (migris_nvstore_get(
                &nvstore, MIGRIS_NVSTORE_RECORD_SCHEDULE, &sched_bytes, &sched_len) ==
            MIGRIS_NVSTORE_OK) {
            (void)migris_schedule_deserialize(&schedule, sched_bytes, sched_len);
        }
    }
    ctx.schedule = &schedule;

    /* Slice fsw-11: the on-board packet store. transmit_tm taps every
     * live TM packet into it; a routed PUS-15 TC enables / disables
     * storage, reports it, deletes a time range, or arms a retrieval
     * the drain tick in the loop below empties. migris_pktstore_init
     * leaves storage ENABLED, so telemetry is captured from boot. */
    migris_pktstore_init(&store);
    ctx.store = &store;

    /* Slice fsw-15: the on-board housekeeping-structure store. A routed
     * PUS-3 structure-management TC creates / enables structures here;
     * the emission tick in the loop below turns an enabled, due
     * structure into a datapool-backed dynamic PUS-3[25] report. The
     * store starts empty — ground defines structures with [3,1].
     * fsw-17: defined structures and their enabled flags are restored
     * from the nvstore HKSTORE record (if one was loaded); each
     * restored structure re-arms from now so a long boot does not fire
     * an immediate downlink burst. */
    migris_hkstore_init(&hkstore);
    if (nv_load_rc == MIGRIS_NVSTORE_OK) {
        const uint8_t* hk_bytes = NULL;
        uint16_t hk_len = 0U;
        if (migris_nvstore_get(&nvstore, MIGRIS_NVSTORE_RECORD_HKSTORE, &hk_bytes, &hk_len) ==
            MIGRIS_NVSTORE_OK) {
            (void)migris_hkstore_deserialize(&hkstore, hk_bytes, hk_len);
        }
    }
    ctx.hkstore = &hkstore;

#ifdef CONFIG_FSW_LARGEDATA_DEMO
    /* Slice fsw-12: arm the PUS-13 large-data demo. The unit is a byte
     * ramp; the main loop's part-drain tick below downlinks it as a
     * sequence of [13,1] / [13,2] / [13,3] parts, one per iteration. */
    for (size_t i = 0U; i < LARGEDATA_DEMO_UNIT_LEN; ++i) {
        largedata_demo_unit[i] = (uint8_t)i;
    }
    migris_largedata_init(&largedata);
    (void)migris_largedata_start(
        &largedata, LARGEDATA_DEMO_TRANSACTION_ID, largedata_demo_unit, LARGEDATA_DEMO_UNIT_LEN);
#endif

#ifdef CONFIG_FSW_MODE_DEMO
    /* Slice fsw-13: wire the operating-mode manager. The mode set and
     * its allowed transitions are sample-defined — fsw-core hard-codes
     * no modes. BOOT may go to NOMINAL or SAFE; NOMINAL and SAFE may
     * swap. The FDIR sink carries the MODE_CHANGED event. The mode set
     * is a fixed, compile-time-correct constant, so the init cannot
     * fail. fsw-17: the current mode is restored from the nvstore MODE
     * record (if one was loaded); the boot transition further down is
     * guarded on the post-restore current being BOOT, so a persisted
     * SAFE outlives a reboot rather than being silently undone. */
    const migris_mode_def_t mode_defs[] = {
        {FSW_MODE_BOOT, (1U << FSW_MODE_NOMINAL) | (1U << FSW_MODE_SAFE)},
        {FSW_MODE_NOMINAL, 1U << FSW_MODE_SAFE},
        {FSW_MODE_SAFE, 1U << FSW_MODE_NOMINAL},
    };
    (void)migris_mode_init(
        &mode_mgr, mode_defs, sizeof(mode_defs) / sizeof(mode_defs[0]), FSW_MODE_BOOT, &fdir_sink);
    if (nv_load_rc == MIGRIS_NVSTORE_OK) {
        const uint8_t* mode_bytes = NULL;
        uint16_t mode_len = 0U;
        if (migris_nvstore_get(&nvstore, MIGRIS_NVSTORE_RECORD_MODE, &mode_bytes, &mode_len) ==
            MIGRIS_NVSTORE_OK) {
            (void)migris_mode_deserialize_current(&mode_mgr, mode_bytes, mode_len);
        }
    }
#endif

#ifdef CONFIG_FSW_FDIR_RECOVERY_DEMO
    /* Slice fsw-14: arm FDIR isolation/recovery. After
     * CONFIG_FSW_FDIR_TC_REJECTED_THRESHOLD rejected telecommands FDIR
     * confirms the fault, emits a high-severity PUS-5 FDIR_RECOVERY
     * event, and autonomously commands the mode manager to SAFE. The
     * MODE_CHANGED that the transition raises drains through the same
     * FDIR FIFO, so the main loop needs no new tick. */
    const migris_fdir_confirm_def_t fdir_confirms[] = {
        {MIGRIS_FDIR_ANOM_TC_REJECTED, CONFIG_FSW_FDIR_TC_REJECTED_THRESHOLD},
    };
    (void)migris_fdir_arm_recovery(&fdir,
                                   &mode_mgr,
                                   FSW_MODE_SAFE,
                                   fdir_confirms,
                                   sizeof(fdir_confirms) / sizeof(fdir_confirms[0]));
#endif

    uint8_t tc[TC_BUF_SIZE];
    uint8_t out[MIGRIS_TC_ROUTER_MAX_TM];
    size_t have = 0U;
    size_t want = 0U; /* total TC size once the primary header is in */

    /* Slice fsw-6: one spontaneous PUS-5[1] "FSW boot" event before
     * the TC loop — the framework's first asynchronous, non-TC-
     * triggered TM. It threads the router's shared per-APID
     * `tm_seq_count` (a plain uint16_t field) so the boot event
     * consumes count 0 and the per-APID sequence stays strictly
     * monotonic across it and every subsequent verification / service
     * packet. The PUS-5 context now lives in the router context
     * (slice fsw-8), shared by the boot event, the [27]-polled
     * housekeeping report and the FDIR anomaly drain. */
    const uint32_t boot_sec = (uint32_t)(k_uptime_get() / 1000);
    const int boot_n = migris_pus5_build_event_report(&ctx.pus5,
                                                      ctx.apid,
                                                      &ctx.tm_seq_count,
                                                      boot_sec,
                                                      MIGRIS_PUS5_SEV_INFO,
                                                      MIGRIS_PUS5_EVT_FSW_BOOT,
                                                      NULL,
                                                      0U,
                                                      0U,
                                                      out,
                                                      sizeof(out));
    if (boot_n > 0) {
        transmit_tm(uart_dev, boot_sec, out, (size_t)boot_n);
    }

#ifdef CONFIG_FSW_MODE_DEMO
    /* Slice fsw-13: the boot-time mode transition. Once the FSW is up,
     * leave BOOT for NOMINAL; the mode manager enqueues a PUS-5
     * MODE_CHANGED event through the FDIR sink, drained onto the wire
     * by the loop's FDIR tick like any other event.
     *
     * fsw-17: gated on the post-restore current still being BOOT — if
     * the nvstore restored a previously-persisted operational mode
     * (NOMINAL, or SAFE after an FDIR recovery), reboot must leave the
     * spacecraft in that mode rather than silently undoing it. */
    if (migris_mode_current(&mode_mgr) == FSW_MODE_BOOT) {
        (void)migris_mode_request(&mode_mgr, FSW_MODE_NOMINAL, boot_sec);
    }
#endif

    /* Slice fsw-7: spontaneous periodic PUS-3[25] housekeeping report.
     * `pus3_ctx` is sample-local. The first report fires one full
     * period *after* boot — not immediately — so a sub-second TC
     * exchange always completes within a period and the per-APID
     * sequence stays a single monotonic space across the boot event,
     * every verification / service burst, each periodic report, and
     * each drained FDIR anomaly (all share `&ctx.tm_seq_count`, all run
     * sequentially in this single thread). */
    migris_pus3_ctx_t pus3_ctx = {0};
    uint32_t last_hk_emit_sec = boot_sec;

    /* Last RX-overflow drop count already turned into a PUS-5 event.
     * The ISR only bumps `rx_ring_overflow_drops`; the loop (the sole
     * FIFO producer) reports the *delta* as an RX_OVERFLOW anomaly. */
    uint32_t last_reported_rx_drops = 0U;

    /* Per-subsystem snapshots of the generation already persisted to
     * flash (slice fsw-16 for the datapool, fsw-17 for the others).
     * Each subsystem bumps its generation on every persisted mutation;
     * the loop's save tick below visits the four subsystems through
     * `nv_autosave_tick`, puts the new bytes only for records that
     * advanced, and issues at most one `migris_nvstore_save` per
     * iteration — back-to-back mutations coalesce into one flash
     * write. Initialised to the post-restore generation (0 in both
     * cases — deserialise is a restore, not a mutation, so it does
     * not bump). */
    uint32_t last_saved_gen_dp = migris_datapool_generation(&datapool);
    uint32_t last_saved_gen_sched = migris_schedule_generation(&schedule);
    uint32_t last_saved_gen_hk = migris_hkstore_generation(&hkstore);
#ifdef CONFIG_FSW_MODE_DEMO
    uint32_t last_saved_gen_mode = migris_mode_generation(&mode_mgr);
#endif

    for (;;) {
        /* One FSW-clock read per iteration, reused by the periodic
         * report and the TC dispatch (the full TC completes in the
         * same iteration its final byte arrives, microseconds after
         * this read — coarse seconds are unaffected). */
        const uint32_t now_sec = (uint32_t)(k_uptime_get() / 1000);

        /* Non-volatile save tick (slice fsw-16, extended in fsw-17).
         * Each persisted subsystem bumps its generation counter on
         * every mutation; the helper puts the new record into the
         * in-RAM nvstore image only for records whose generation has
         * advanced since the last save. A single flash write at the
         * end of the tick coalesces multiple subsystem changes from
         * the same iteration. Ahead of every other tick so the
         * on-flash image always reflects state ground has already
         * seen a PUS-1 completion for. The save is best-effort: a
         * backend failure leaves the `last_saved_gen_*` snapshots
         * unchanged so the next iteration retries. */
        int nv_dirty = 0;
        nv_dirty |= nv_autosave_tick(MIGRIS_NVSTORE_RECORD_DATAPOOL,
                                     nv_ser_datapool,
                                     &datapool,
                                     migris_datapool_generation(&datapool),
                                     &last_saved_gen_dp,
                                     nv_buf_dp,
                                     sizeof(nv_buf_dp));
        nv_dirty |= nv_autosave_tick(MIGRIS_NVSTORE_RECORD_SCHEDULE,
                                     nv_ser_schedule,
                                     &schedule,
                                     migris_schedule_generation(&schedule),
                                     &last_saved_gen_sched,
                                     nv_buf_sched,
                                     sizeof(nv_buf_sched));
        nv_dirty |= nv_autosave_tick(MIGRIS_NVSTORE_RECORD_HKSTORE,
                                     nv_ser_hkstore,
                                     &hkstore,
                                     migris_hkstore_generation(&hkstore),
                                     &last_saved_gen_hk,
                                     nv_buf_hk,
                                     sizeof(nv_buf_hk));
#ifdef CONFIG_FSW_MODE_DEMO
        nv_dirty |= nv_autosave_tick(MIGRIS_NVSTORE_RECORD_MODE,
                                     nv_ser_mode,
                                     &mode_mgr,
                                     migris_mode_generation(&mode_mgr),
                                     &last_saved_gen_mode,
                                     nv_buf_mode,
                                     sizeof(nv_buf_mode));
#endif
        if (nv_dirty != 0) {
            (void)migris_nvstore_save(&nvstore);
        }

        /* Spontaneous periodic housekeeping (slice fsw-7). Checked
         * every iteration — including the idle path below, where most
         * time is spent. The shared `out[]` is fully transmitted
         * (blocking) before the loop proceeds, so a periodic report
         * never overlaps a TC-driven burst.
         *
         * The period is a live-tunable datapool parameter (fsw-9):
         * re-read every iteration so a PUS-20[3] set reconfigures the
         * cadence with no rebuild. A period of 0 disables it. */
        uint32_t hk_period_sec = 0U;
        migris_dp_value_t hk_period = {0};
        if (migris_datapool_get(&datapool, MIGRIS_FSW_PARAM_HK_PERIOD_SEC, &hk_period) ==
                MIGRIS_DATAPOOL_OK &&
            hk_period.type == MIGRIS_DP_TYPE_U32) {
            hk_period_sec = migris_dp_as_u32(&hk_period);
        }
        if (hk_period_sec != 0U && (now_sec - last_hk_emit_sec) >= hk_period_sec) {
            ctx.rx_ring_overflow_drops = rx_ring_overflow_drops;
            migris_pus3_hk_params_t hp;
            fill_hk_params(&ctx, &hp);
            const int hk_n = migris_pus3_build_hk_report(&pus3_ctx,
                                                         ctx.apid,
                                                         &ctx.tm_seq_count,
                                                         now_sec,
                                                         MIGRIS_PUS3_SID_FRAMEWORK_DIAG,
                                                         &hp,
                                                         0U,
                                                         out,
                                                         sizeof(out));
            if (hk_n > 0) {
                transmit_tm(uart_dev, now_sec, out, (size_t)hk_n);
            }
            last_hk_emit_sec = now_sec;
        }

        /* Dynamic housekeeping structures (slice fsw-15). A
         * ground-created structure that is enabled and whose interval
         * has elapsed is emitted as a datapool-backed PUS-3[25]
         * report — one structure per iteration, reusing the shared
         * out[] (fully transmitted before the loop proceeds). A
         * structure naming a parameter the datapool lacks produces no
         * packet — the encoder fails the whole report — and is retried
         * at its next interval. The store is empty until ground
         * defines a structure, so this is a no-op on the
         * verification-stream build. */
        const migris_hk_structure_t* due_struct = migris_hkstore_due(&hkstore, now_sec);
        if (due_struct != NULL) {
            const int dyn_n = migris_pus3_build_dynamic_hk_report(&pus3_ctx,
                                                                  &datapool,
                                                                  due_struct,
                                                                  ctx.apid,
                                                                  &ctx.tm_seq_count,
                                                                  now_sec,
                                                                  0U,
                                                                  out,
                                                                  sizeof(out));
            if (dyn_n > 0) {
                transmit_tm(uart_dev, now_sec, out, (size_t)dyn_n);
            }
        }

        /* RX-overflow detector (slice fsw-8). The loop, not the ISR,
         * is the FIFO producer: on an increase of the ISR's drop
         * counter, report the delta as a PUS-5 RX_OVERFLOW anomaly. */
        const uint32_t rx_drops = rx_ring_overflow_drops;
        if (rx_drops != last_reported_rx_drops) {
            migris_fdir_report_anomaly(
                &fdir, MIGRIS_FDIR_ANOM_RX_OVERFLOW, now_sec, rx_drops - last_reported_rx_drops);
            last_reported_rx_drops = rx_drops;
        }

        /* Drain one FDIR event per iteration into a spontaneous PUS-5
         * report. Checked every iteration (including the idle path),
         * after the periodic report and *before* the TC handling — a
         * rejected TC enqueues its anomaly during dispatch below, so
         * it surfaces on the next iteration, strictly after that TC's
         * PUS-1 verification went out. The shared `out[]` is fully
         * transmitted before the loop proceeds. */
        const int fdir_n =
            migris_fdir_drain(&fdir, ctx.apid, &ctx.tm_seq_count, &ctx.pus5, out, sizeof(out));
        if (fdir_n > 0) {
            transmit_tm(uart_dev, now_sec, out, (size_t)fdir_n);
        }

        /* Release tick (slice fsw-10): if the schedule is enabled and
         * an activity is due, dispatch its telecommand through the
         * router — one per iteration, after the FDIR drain and before
         * the inbound-TC handling, so each use of `out[]` is fully
         * transmitted before the next. */
        uint8_t released[MIGRIS_SCHEDULE_TC_MAX];
        size_t released_len = 0U;
        if (migris_schedule_pop_due(
                &schedule, now_sec, released, sizeof(released), &released_len) == 1) {
            const int rel_n =
                migris_tc_router_dispatch(&ctx, now_sec, released, released_len, out, sizeof(out));
            if (rel_n > 0) {
                transmit_tm(uart_dev, now_sec, out, (size_t)rel_n);
            }
        }

        /* Retrieval drain (slice fsw-11): a routed PUS-15[9] downlink
         * arms a by-time-window retrieval; here, one stored packet in
         * the window is re-emitted verbatim per iteration — a replay
         * of stored history, so it goes out via plain uart_tx_blocking
         * and is NOT tapped back into the store. It reuses `out[]`
         * (fully transmitted before the next use, like every other
         * burst) and is drained after the schedule release and before
         * the inbound-TC handling. While a retrieval is active the
         * store is frozen, so transmit_tm's taps above are no-ops
         * until the window is exhausted. */
        size_t replay_len = 0U;
        if (migris_pktstore_retrieve_next(&store, out, sizeof(out), &replay_len) == 1) {
            uart_tx_blocking(uart_dev, out, replay_len);
        }

#ifdef CONFIG_FSW_LARGEDATA_DEMO
        /* Large-data part drain (slice fsw-12): emit one [13,1] /
         * [13,2] / [13,3] part of the PUS-13 demo transfer per
         * iteration, after the retrieval drain and before the
         * inbound-TC handling so each use of `out[]` is fully
         * transmitted first. Parts are live TM — they go out through
         * transmit_tm and are tapped into the packet store like any
         * other emitted telemetry. */
        const int part_n = migris_largedata_next_part(
            &largedata, ctx.apid, &ctx.tm_seq_count, now_sec, 0U, out, sizeof(out));
        if (part_n > 0) {
            transmit_tm(uart_dev, now_sec, out, (size_t)part_n);
        }
#endif

        uint8_t b = 0U;
        const uint32_t got = ring_buf_get(&rx_ring, &b, 1);
        if (got == 0U) {
            /* Yield cheaply — the ISR refills the ring while we
             * sleep. K_MSEC(1) keeps wake-up latency low enough for a
             * 115200-baud single-packet exchange. */
            k_sleep(K_MSEC(1));
            continue;
        }

        tc[have++] = b;

        /* Once the 6-byte primary header is in, the CCSDS Packet Data
         * Length tells us the exact total. A length we cannot buffer
         * means we are out of sync — reset and re-listen. */
        if (have == MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) {
            const uint16_t data_length = (uint16_t)(((uint16_t)tc[4] << 8) | (uint16_t)tc[5]);
            want = migris_ccsds_packet_total_size(data_length);
            if (want > sizeof(tc)) {
                have = 0U;
                want = 0U;
                continue;
            }
        }

        if (want != 0U && have == want) {
            /* Snapshot the ISR drop counter so a [27]-poll-triggered
             * report and the accepted/rejected counters compose
             * coherently within this dispatch. */
            ctx.rx_ring_overflow_drops = rx_ring_overflow_drops;
            const int rc = migris_tc_router_dispatch(&ctx, now_sec, tc, have, out, sizeof(out));
            if (rc > 0) {
                transmit_tm(uart_dev, now_sec, out, (size_t)rc);
            }
            /* Done with this TC regardless of the verdict — reset for
             * the next packet. */
            have = 0U;
            want = 0U;
        }
    }
}
