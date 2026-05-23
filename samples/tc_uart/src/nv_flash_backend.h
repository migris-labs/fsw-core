/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Zephyr `flash_area_*` backend for the freestanding `lib/nvstore/`
 * layer. Slice fsw-16. Sample-local glue: the board's
 * `storage_partition` (256 KB on `nucleo_h753zi`, two 128 KB sectors)
 * becomes the A/B ping-pong NVM the nvstore manages.
 *
 * This file lives in the sample because it is the Zephyr-specific
 * side of the seam — `lib/nvstore/` itself is freestanding C. A
 * future HAL/BSP slice may promote it to a reusable component.
 */

#ifndef MIGRIS_FSW_TC_UART_NV_FLASH_BACKEND_H_
#define MIGRIS_FSW_TC_UART_NV_FLASH_BACKEND_H_

#include "migris/fsw/nvstore/nvstore.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build a `migris_nv_backend_t` that wraps Zephyr's `flash_area_*`
 *  API over the board's `storage_partition`. Returns a backend ready
 *  to hand to `migris_nvstore_init`. On failure (the flash driver did
 *  not initialise, the devicetree partition is missing, or the
 *  partition does not have at least two sectors) every function
 *  pointer is left wired to a stub that returns ``-1``, so the
 *  nvstore's `load`/`save` will fail cleanly without crashing. */
migris_nv_backend_t migris_fsw_nv_flash_backend(void);

#ifdef __cplusplus
}
#endif

#endif  // MIGRIS_FSW_TC_UART_NV_FLASH_BACKEND_H_
