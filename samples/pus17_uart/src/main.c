/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * fsw-4 PUS-17 connection-test sample.
 *
 * Boots Zephyr on nucleo_h753zi, listens on USART3 for a CCSDS Space
 * Packet, validates it as a PUS-17[1] TC, and replies with a
 * PUS-17[2] TM on the same UART. Wire format is pinned in
 * docs/wire/pus-17.md.
 *
 * Architecture (slice-minimum):
 *
 *   USART3 RX IRQ  ──fifo_read──▶  ring_buf  ──ring_buf_get──▶  main loop
 *                                                                  │
 *                                                                  ▼
 *                                                          migris_pus17_*()
 *                                                                  │
 *                                                                  ▼
 *                                                   uart_poll_out  ◀── tm[]
 *
 * Single producer (the RX IRQ) and single consumer (main thread)
 * make ring_buf safe without explicit locking. We send TM with
 * blocking ``uart_poll_out`` — slice fsw-4 has no concurrent TX
 * pressure, so interrupt-driven TX would be premature.
 *
 * The parser is intentionally strict and stateless across packets:
 * any malformed header resets the receive cursor. There is no
 * resynchronisation magic byte (CCSDS provides none) — over a
 * lossless emulated UART this is sufficient. Real RF will rely on
 * the AOS/TC transfer-frame ASM (``0x1ACFFC1D``) one layer down,
 * not on the Space Packet layer.
 */

#include "migris/fsw/pus/ccsds.h"
#include "migris/fsw/pus/pus17.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

/* USART3 is the platform-pinned UART for fsw-4 (same as fsw-3's
 * hello sample). The board's ``zephyr,console`` choice points there
 * by default; we leave that alone — boot banner / printk / log are
 * all disabled in prj.conf so nothing else writes to it. */
#define UART_NODE DT_NODELABEL(usart3)
static const struct device* const uart_dev = DEVICE_DT_GET(UART_NODE);

/* APID 0x100 — see docs/wire/pus-17.md. The sample app is its own
 * application process; downstream missions will allocate their own. */
#define MIGRIS_FSW4_APID 0x100U

/* Ring-buffer between the UART RX ISR and the main thread.
 * 128 bytes is ~10× the size of a PUS-17[1] TC; comfortably absorbs
 * a burst even with main pre-empted by other Zephyr work. */
#define RX_RING_SIZE 128
RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);

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
         * a flow-controlled link. Real downstream code will tie this
         * to PUS-5 event reporting once that service lands. */
        (void)ring_buf_put(&rx_ring, &byte, 1);
    }
}

static void uart_tx_blocking(const struct device* dev, const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uart_poll_out(dev, buf[i]);
    }
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

    migris_pus17_ctx_t ctx = {
        .apid = MIGRIS_FSW4_APID,
        .tm_seq_count = 0U,
        .tm_msg_counter = 0U,
    };

    uint8_t tc[MIGRIS_PUS17_TC_PACKET_SIZE];
    uint8_t tm[MIGRIS_PUS17_TM_PACKET_SIZE];
    size_t have = 0U;

    for (;;) {
        uint8_t b = 0U;
        const uint32_t got = ring_buf_get(&rx_ring, &b, 1);
        if (got == 0U) {
            /* Yield cheaply — the ISR will refill the ring while
             * we sleep. K_MSEC(1) keeps wake-up latency low enough
             * for a 115200-baud single-packet exchange. */
            k_sleep(K_MSEC(1));
            continue;
        }

        tc[have++] = b;

        /* Once the 6-byte CCSDS primary header is in, sanity-check
         * its declared length. Anything that doesn't match the
         * expected PUS-17[1] envelope means we're either out of
         * sync or talking to something we don't understand —
         * either way, reset and re-listen from byte 0. */
        if (have == MIGRIS_CCSDS_PRIMARY_HEADER_SIZE) {
            const uint16_t data_length = (uint16_t)(((uint16_t)tc[4] << 8) | (uint16_t)tc[5]);
            const size_t total = migris_ccsds_packet_total_size(data_length);
            if (total != MIGRIS_PUS17_TC_PACKET_SIZE) {
                have = 0U;
                continue;
            }
        }

        if (have == MIGRIS_PUS17_TC_PACKET_SIZE) {
            const uint32_t now_sec = (uint32_t)(k_uptime_get() / 1000);
            const int rc =
                migris_pus17_handle_are_you_alive(&ctx, now_sec, tc, have, tm, sizeof(tm));
            if (rc > 0) {
                uart_tx_blocking(uart_dev, tm, (size_t)rc);
            }
            /* Whether the handler accepted or rejected the packet,
             * we're done with this TC and reset for the next. */
            have = 0U;
        }
    }
}
