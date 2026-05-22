// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Migris Labs
//
// Non-volatile-storage layer — the A/B-redundant flash image format,
// the in-RAM record-typed payload, and the load/get/put/save API.
// Pure (gtest-free) helpers build a RAM-backed migris_nv_backend_t so
// every algorithm (image build, header validation, CRC, A/B ping-pong,
// corruption recovery) is exercised host-side without a real flash
// device. The Zephyr flash_area_* backend that ships in slice fsw-16's
// sample is the closed-loop validator.

#include "migris/fsw/nvstore/nvstore.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace migris::fsw::nvstore::test {
namespace {

// Two 1 KB sectors backed by plain RAM — enough room for the maximum
// image (header 12 + payload 512 + CRC 2 + padding to write_block) and
// still cheap. write_block mirrors the STM32H7's 32-byte flash word so
// host tests respect the same alignment a real backend will enforce.
constexpr std::uint32_t test_sector_size = 1024U;
constexpr std::uint32_t test_sector_count = 2U;
constexpr std::uint32_t test_write_block = 32U;

struct RamBackend {
    std::array<std::array<std::uint8_t, test_sector_size>, test_sector_count> sectors{};
    int read_fail = 0;  // non-zero on the n-th call → fail (1-indexed)
    int read_calls = 0;
    int write_fail = 0;
    int write_calls = 0;
    int erase_fail = 0;
    int erase_calls = 0;

    void reset() {
        for (auto& s : sectors) {
            // Mirror Renode's MappedMemory power-on (0x00). A real
            // erased flash would be 0xFF; the magic/CRC checks make the
            // store erased-pattern-agnostic, so either works.
            s.fill(0x00U);
        }
        read_fail = write_fail = erase_fail = 0;
        read_calls = write_calls = erase_calls = 0;
    }
};

extern "C" {

int ram_read(void* self, std::uint32_t sector, std::uint32_t off, void* dst, std::size_t len) {
    auto* b = static_cast<RamBackend*>(self);
    b->read_calls++;
    if (b->read_fail > 0 && b->read_calls == b->read_fail) {
        return -1;
    }
    std::memcpy(dst, b->sectors.at(sector).data() + off, len);
    return 0;
}

int ram_write(
    void* self, std::uint32_t sector, std::uint32_t off, const void* src, std::size_t len) {
    auto* b = static_cast<RamBackend*>(self);
    b->write_calls++;
    if (b->write_fail > 0 && b->write_calls == b->write_fail) {
        return -1;
    }
    std::memcpy(b->sectors.at(sector).data() + off, src, len);
    return 0;
}

int ram_erase(void* self, std::uint32_t sector) {
    auto* b = static_cast<RamBackend*>(self);
    b->erase_calls++;
    if (b->erase_fail > 0 && b->erase_calls == b->erase_fail) {
        return -1;
    }
    b->sectors.at(sector).fill(0xFFU);
    return 0;
}
}  // extern "C"

migris_nv_backend_t ram_backend(RamBackend& b) {
    migris_nv_backend_t bk{};
    bk.read = ram_read;
    bk.write = ram_write;
    bk.erase = ram_erase;
    bk.sector_size = test_sector_size;
    bk.sector_count = test_sector_count;
    bk.write_block = test_write_block;
    bk.self = &b;
    return bk;
}

// One record of arbitrary bytes — distinct from the test_apid pattern
// so a layout regression is easy to spot.
constexpr std::uint8_t type_datapool = MIGRIS_NVSTORE_RECORD_DATAPOOL;
constexpr std::uint8_t type_other = 0x20U;

std::vector<std::uint8_t> sentinel(std::size_t len) {
    std::vector<std::uint8_t> v;
    v.reserve(len);
    for (std::size_t i = 0U; i < len; ++i) {
        v.push_back(static_cast<std::uint8_t>(0x40U + (i & 0x3FU)));
    }
    return v;
}

TEST(NvStore, InitIsEmptyAndUnloaded) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    EXPECT_EQ(store.payload_len, 0U);
    EXPECT_EQ(store.loaded, 0);
}

TEST(NvStore, LoadFromEmptyFlashReturnsNoValidImage) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    EXPECT_EQ(migris_nvstore_load(&store), MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE);
    EXPECT_EQ(store.loaded, 0);
    EXPECT_EQ(store.payload_len, 0U);
}

TEST(NvStore, PutThenGetRoundTrip) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto bytes = sentinel(16U);
    ASSERT_EQ(migris_nvstore_put(
                  &store, type_datapool, bytes.data(), static_cast<std::uint16_t>(bytes.size())),
              MIGRIS_NVSTORE_OK);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&store, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(out_len, bytes.size());
    EXPECT_EQ(std::memcmp(out, bytes.data(), bytes.size()), 0);
}

TEST(NvStore, PutReplacesAnExistingRecord) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto v1 = sentinel(8U);
    const auto v2 = sentinel(24U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v1.data(), 8U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v2.data(), 24U), MIGRIS_NVSTORE_OK);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&store, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 24U);
    EXPECT_EQ(std::memcmp(out, v2.data(), v2.size()), 0);
}

TEST(NvStore, RecordsOfDifferentTypesCoexist) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto a = sentinel(10U);
    const auto b = sentinel(20U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, a.data(), 10U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_put(&store, type_other, b.data(), 20U), MIGRIS_NVSTORE_OK);

    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&store, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 10U);
    EXPECT_EQ(std::memcmp(out, a.data(), 10U), 0);
    ASSERT_EQ(migris_nvstore_get(&store, type_other, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 20U);
    EXPECT_EQ(std::memcmp(out, b.data(), 20U), 0);
}

TEST(NvStore, GetReturnsNotFoundForUnknownType) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    EXPECT_EQ(migris_nvstore_get(&store, type_datapool, &out, &out_len),
              MIGRIS_NVSTORE_ERR_NOT_FOUND);
}

TEST(NvStore, PutRejectsAPayloadOverTheLimit) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    // One record larger than the whole payload budget is impossible.
    const auto huge = sentinel(MIGRIS_NVSTORE_PAYLOAD_MAX);  // no room for the 3-byte overhead
    EXPECT_EQ(migris_nvstore_put(
                  &store, type_datapool, huge.data(), static_cast<std::uint16_t>(huge.size())),
              MIGRIS_NVSTORE_ERR_FULL);
}

TEST(NvStore, SaveThenLoadRoundTrip) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto bytes = sentinel(32U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, bytes.data(), 32U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);

    // A fresh store on the same backend recovers the record.
    migris_nvstore_t reloaded{};
    migris_nvstore_init(&reloaded, &backend);
    ASSERT_EQ(migris_nvstore_load(&reloaded), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(reloaded.loaded, 1);
    EXPECT_EQ(reloaded.loaded_seq, 1U);

    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&reloaded, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 32U);
    EXPECT_EQ(std::memcmp(out, bytes.data(), 32U), 0);
}

TEST(NvStore, SaveAlternatesBetweenSectors) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto bytes = sentinel(8U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, bytes.data(), 8U), MIGRIS_NVSTORE_OK);

    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(store.loaded_sector, 0U);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(store.loaded_sector, 1U);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(store.loaded_sector, 0U);
    EXPECT_EQ(store.loaded_seq, 3U);
}

TEST(NvStore, LoadPicksTheHigherSequenceCopy) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto v1 = sentinel(8U);
    const auto v2 = sentinel(16U);

    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v1.data(), 8U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);  // seq 1 → sector 0
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v2.data(), 16U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);  // seq 2 → sector 1

    migris_nvstore_t reloaded{};
    migris_nvstore_init(&reloaded, &backend);
    ASSERT_EQ(migris_nvstore_load(&reloaded), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(reloaded.loaded_seq, 2U);
    EXPECT_EQ(reloaded.loaded_sector, 1U);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&reloaded, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 16U);
}

TEST(NvStore, LoadFallsBackWhenTheNewerCopyHasACorruptCrc) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto v1 = sentinel(8U);
    const auto v2 = sentinel(16U);

    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v1.data(), 8U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);  // seq 1 → sector 0
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, v2.data(), 16U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);  // seq 2 → sector 1

    // Corrupt the CRC of the higher-seq copy (sector 1). The store
    // must transparently fall back to the older but intact sector 0.
    const std::size_t crc_off = MIGRIS_NVSTORE_HEADER_SIZE + 16U;
    ram.sectors[1][crc_off] ^= 0xFFU;

    migris_nvstore_t reloaded{};
    migris_nvstore_init(&reloaded, &backend);
    ASSERT_EQ(migris_nvstore_load(&reloaded), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(reloaded.loaded_seq, 1U);
    EXPECT_EQ(reloaded.loaded_sector, 0U);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    ASSERT_EQ(migris_nvstore_get(&reloaded, type_datapool, &out, &out_len), MIGRIS_NVSTORE_OK);
    EXPECT_EQ(out_len, 8U);  // the older copy's record
}

TEST(NvStore, LoadRejectsAWrongFormatVersion) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto bytes = sentinel(8U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, bytes.data(), 8U), MIGRIS_NVSTORE_OK);
    ASSERT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_OK);

    // Bump the format_version byte to a value this firmware does not
    // understand (an upgrade contract: a major change must bump it).
    ram.sectors[0][5] = static_cast<std::uint8_t>(MIGRIS_NVSTORE_FORMAT_VERSION + 1U);

    migris_nvstore_t reloaded{};
    migris_nvstore_init(&reloaded, &backend);
    EXPECT_EQ(migris_nvstore_load(&reloaded), MIGRIS_NVSTORE_ERR_NO_VALID_IMAGE);
    EXPECT_EQ(reloaded.loaded, 0);
}

TEST(NvStore, BackendEraseFailurePropagatesAsErrBackend) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    const auto bytes = sentinel(8U);
    ASSERT_EQ(migris_nvstore_put(&store, type_datapool, bytes.data(), 8U), MIGRIS_NVSTORE_OK);
    ram.erase_fail = 1;  // fail the next erase
    EXPECT_EQ(migris_nvstore_save(&store), MIGRIS_NVSTORE_ERR_BACKEND);
}

TEST(NvStore, NullArgumentsRejected) {
    RamBackend ram{};
    ram.reset();
    const migris_nv_backend_t backend = ram_backend(ram);
    migris_nvstore_t store{};
    migris_nvstore_init(&store, &backend);
    EXPECT_EQ(migris_nvstore_load(nullptr), MIGRIS_NVSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_nvstore_save(nullptr), MIGRIS_NVSTORE_ERR_BAD_ARG);
    EXPECT_EQ(migris_nvstore_put(nullptr, type_datapool, nullptr, 0U), MIGRIS_NVSTORE_ERR_BAD_ARG);
    const std::uint8_t* out = nullptr;
    std::uint16_t out_len = 0U;
    EXPECT_EQ(migris_nvstore_get(nullptr, type_datapool, &out, &out_len),
              MIGRIS_NVSTORE_ERR_BAD_ARG);
}

}  // namespace
}  // namespace migris::fsw::nvstore::test
