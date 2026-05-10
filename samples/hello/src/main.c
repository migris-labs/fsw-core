/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * fsw-core hello-world — slice fsw-2.
 *
 * Minimum Zephyr application that proves the cross-compile toolchain
 * end-to-end on the platform's pinned target (STM32H753ZI / Cortex-M7
 * on the nucleo_h753zi board). The console string emitted here is the
 * contract a future Renode UART smoke test (slice fsw-3) will assert
 * on; see samples/hello/sample.yaml.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void) {
    printk("Hello, fsw-core / nucleo_h753zi\n");
    printk("boot ok; idling\n");

    while (1) {
        k_sleep(K_SECONDS(1));
    }
    return 0;
}
