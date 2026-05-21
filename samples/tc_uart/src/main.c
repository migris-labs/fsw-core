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
#include "migris/fsw/pktstore/pktstore.h"
#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus11.h"
#include "migris/fsw/pus/pus15.h"
#include "migris/fsw/pus/pus20.h"
#include "migris/fsw/pus/pus3.h"
#include "migris/fsw/pus/pus5.h"
#include "migris/fsw/pus/tc_router.h"
#include "migris/fsw/schedule/schedule.h"

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
 * see docs/wire/pus-20.md). This sample registers exactly one: the
 * PUS-3 housekeeping period, made operator-tunable via PUS-20. */
#define MIGRIS_FSW_PARAM_HK_PERIOD_SEC 0x0001U

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
    };
    migris_datapool_t datapool;
    (void)migris_datapool_init(&datapool, dp_params, sizeof(dp_params) / sizeof(dp_params[0]));
    ctx.datapool = &datapool;

    /* Slice fsw-10: the on-board schedule. A routed PUS-11 TC inserts
     * time-tagged telecommands; the release tick in the loop below
     * dispatches due ones. The schedule starts disabled — ground
     * enables it with a PUS-11[1]. */
    migris_schedule_init(&schedule);
    ctx.schedule = &schedule;

    /* Slice fsw-11: the on-board packet store. transmit_tm taps every
     * live TM packet into it; a routed PUS-15 TC enables / disables
     * storage, reports it, deletes a time range, or arms a retrieval
     * the drain tick in the loop below empties. migris_pktstore_init
     * leaves storage ENABLED, so telemetry is captured from boot. */
    migris_pktstore_init(&store);
    ctx.store = &store;

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

    for (;;) {
        /* One FSW-clock read per iteration, reused by the periodic
         * report and the TC dispatch (the full TC completes in the
         * same iteration its final byte arrives, microseconds after
         * this read — coarse seconds are unaffected). */
        const uint32_t now_sec = (uint32_t)(k_uptime_get() / 1000);

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
