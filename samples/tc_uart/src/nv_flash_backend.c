/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Migris Labs
 *
 * Zephyr `flash_area_*` backend for `lib/nvstore/`. Slice fsw-16.
 *
 * The board's `storage_partition` (defined in
 * `boards/st/nucleo_h753zi/nucleo_h753zi.dts`: 256 KB at offset
 * 0xC0000 = exactly two of the STM32H7's 128 KB flash sectors)
 * becomes the A/B ping-pong NVM. `flash_area_get_sectors` is the
 * authoritative source for the actual sector size at boot;
 * `flash_area_align` returns the per-write alignment the STM32H7
 * driver enforces (32 bytes on the H7).
 *
 * One static partition handle is enough for the sample (single
 * application process); the `self` arg of the backend vtable is left
 * unused.
 */

#include "nv_flash_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <zephyr/storage/flash_map.h>

#define NV_FLASH_PARTITION FIXED_PARTITION_ID(storage_partition)

/* Set by ``migris_fsw_nv_flash_backend`` on first use. NULL if
 * `flash_area_open` failed — the stub read/write/erase below then
 * return -1 and the nvstore's load/save fail gracefully. */
static const struct flash_area* nv_flash_area;
/* Resolved at open-time from `flash_area_get_sectors`. The nvstore's
 * sector-indexed API needs to know the actual erase-block size to
 * translate (sector, offset) into a partition offset. */
static uint32_t nv_flash_sector_size;

static int nv_flash_read(void* self, uint32_t sector, uint32_t off, void* dst, size_t len) {
    (void)self;
    if (nv_flash_area == NULL) {
        return -1;
    }
    const off_t partition_off =
        (off_t)((uint64_t)sector * (uint64_t)nv_flash_sector_size) + (off_t)off;
    return flash_area_read(nv_flash_area, partition_off, dst, len);
}

static int nv_flash_write(void* self, uint32_t sector, uint32_t off, const void* src, size_t len) {
    (void)self;
    if (nv_flash_area == NULL) {
        return -1;
    }
    const off_t partition_off =
        (off_t)((uint64_t)sector * (uint64_t)nv_flash_sector_size) + (off_t)off;
    return flash_area_write(nv_flash_area, partition_off, src, len);
}

static int nv_flash_erase(void* self, uint32_t sector) {
    (void)self;
    if (nv_flash_area == NULL) {
        return -1;
    }
    const off_t partition_off = (off_t)((uint64_t)sector * (uint64_t)nv_flash_sector_size);
    return flash_area_erase(nv_flash_area, partition_off, nv_flash_sector_size);
}

migris_nv_backend_t migris_fsw_nv_flash_backend(void) {
    migris_nv_backend_t bk;
    bk.read = nv_flash_read;
    bk.write = nv_flash_write;
    bk.erase = nv_flash_erase;
    bk.sector_size = 0U;
    bk.sector_count = 0U;
    bk.write_block = 0U;
    bk.self = NULL;

    /* Open the partition and discover the actual sector geometry. We
     * use only the first two sectors (A/B ping-pong); any extra sectors
     * the partition might hold are reserved for future slices. */
    if (flash_area_open(NV_FLASH_PARTITION, &nv_flash_area) != 0) {
        nv_flash_area = NULL;
        return bk;
    }
    struct flash_sector sectors[2];
    uint32_t count = 2U;
    if (flash_area_get_sectors(NV_FLASH_PARTITION, &count, sectors) != 0 || count < 2U) {
        flash_area_close(nv_flash_area);
        nv_flash_area = NULL;
        return bk;
    }
    nv_flash_sector_size = (uint32_t)sectors[0].fs_size;

    bk.sector_size = nv_flash_sector_size;
    bk.sector_count = 2U;
    bk.write_block = (uint32_t)flash_area_align(nv_flash_area);
    return bk;
}
