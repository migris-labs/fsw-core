/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * fsw-6 TC reception + verification + event reporting sample.
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

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus3.h"
#include "migris/fsw/pus/pus5.h"
#include "migris/fsw/pus/tc_router.h"

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

/* Largest TC we will buffer. Comfortably above the only TC in the
 * baseline (PUS-17[1] is 13 bytes) with headroom for near-future
 * small commands; a declared length beyond this is treated as
 * desync. */
#define TC_BUF_SIZE 64U

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
 * mechanism — no atomic_t, no irq-lock, no event FIFO (that
 * abstraction stays deferred, see lib/pus/.../pus5.h and the fsw-6
 * CHANGELOG entry). Surfaced in the PUS-3 housekeeping report. */
static volatile uint32_t rx_ring_overflow_drops;

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
         * a flow-controlled link. The drop is now *counted* (slice
         * fsw-7) and reported in the PUS-3 housekeeping structure;
         * raising an asynchronous overflow *event* from ISR context
         * still wants a freestanding bounded event FIFO and stays
         * deferred (see lib/pus/.../pus5.h). */
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

/* Build the framework PUS-3 housekeeping parameter snapshot for the
 * *spontaneous* periodic report. Unlike the router's [27]-poll path,
 * this carries the live PUS-5 counters: the application owns the PUS-5
 * context, so it can report it accurately. */
static void fill_hk_params(const migris_tc_router_ctx_t* router,
                           const migris_pus5_ctx_t* pus5_ctx,
                           migris_pus3_hk_params_t* p) {
    for (size_t i = 0U; i < 4U; ++i) {
        p->pus1_msg_counter[i] = router->pus1.msg_counter[i];
        p->pus5_msg_counter[i] = pus5_ctx->msg_counter[i];
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
     * packet. `pus5_ctx` is sample-local for now; it moves into
     * migris_tc_router_ctx_t (alongside pus1/pus17) when an FDIR
     * consumer raises events from inside the router. */
    migris_pus5_ctx_t pus5_ctx = {0};
    const uint32_t boot_sec = (uint32_t)(k_uptime_get() / 1000);
    const int boot_n = migris_pus5_build_event_report(&pus5_ctx,
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
        uart_tx_blocking(uart_dev, out, (size_t)boot_n);
    }

    /* Slice fsw-7: spontaneous periodic PUS-3[25] housekeeping report.
     * `pus3_ctx` is sample-local (same rationale as `pus5_ctx`). The
     * first report fires one full period *after* boot — not
     * immediately — so a sub-second TC exchange always completes within
     * a period and the per-APID sequence stays a single monotonic
     * space across the boot event, every verification / service burst,
     * and each periodic report (all share `&ctx.tm_seq_count`, all run
     * sequentially in this single thread). */
    migris_pus3_ctx_t pus3_ctx = {0};
    uint32_t last_hk_emit_sec = boot_sec;

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
         * never overlaps a TC-driven burst. */
        if (now_sec - last_hk_emit_sec >= (uint32_t)CONFIG_FSW_PUS3_HK_PERIOD_SEC) {
            ctx.rx_ring_overflow_drops = rx_ring_overflow_drops;
            migris_pus3_hk_params_t hp;
            fill_hk_params(&ctx, &pus5_ctx, &hp);
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
                uart_tx_blocking(uart_dev, out, (size_t)hk_n);
            }
            last_hk_emit_sec = now_sec;
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
                uart_tx_blocking(uart_dev, out, (size_t)rc);
            }
            /* Done with this TC regardless of the verdict — reset for
             * the next packet. */
            have = 0U;
            want = 0U;
        }
    }
}
