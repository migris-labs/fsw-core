# SPDX-License-Identifier: Apache-2.0 Copyright 2026 Migris Labs

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fsw_core_tc_uart LANGUAGES C)

# Pull in the freestanding PUS codec from lib/pus/. Same translation units the host C++ build links
# into migris::fsw-core — keeping the wire-format bytes identical between flight and host tests is
# the whole point of the layout.
set(MIGRIS_FSW_PUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/pus)
set(MIGRIS_FSW_FDIR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/fdir)
set(MIGRIS_FSW_DATAPOOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/datapool)
set(MIGRIS_FSW_SCHEDULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/schedule)
set(MIGRIS_FSW_HKSTORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/hkstore)
set(MIGRIS_FSW_PKTSTORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/pktstore)
set(MIGRIS_FSW_LARGEDATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/largedata)
set(MIGRIS_FSW_MODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mode)
set(MIGRIS_FSW_NVSTORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/nvstore)

target_sources(
  app
  PRIVATE src/main.c
          src/nv_flash_backend.c
          ${MIGRIS_FSW_PUS_DIR}/src/ccsds.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus_tc.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus_tm.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus1.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus5.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus17.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus3.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus11.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus15.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus20.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus13.c
          ${MIGRIS_FSW_PUS_DIR}/src/tc_router.c
          ${MIGRIS_FSW_FDIR_DIR}/src/event_fifo.c
          ${MIGRIS_FSW_FDIR_DIR}/src/fdir.c
          ${MIGRIS_FSW_DATAPOOL_DIR}/src/datapool.c
          ${MIGRIS_FSW_SCHEDULE_DIR}/src/schedule.c
          ${MIGRIS_FSW_HKSTORE_DIR}/src/hkstore.c
          ${MIGRIS_FSW_PKTSTORE_DIR}/src/pktstore.c
          ${MIGRIS_FSW_LARGEDATA_DIR}/src/largedata.c
          ${MIGRIS_FSW_MODE_DIR}/src/mode.c
          ${MIGRIS_FSW_NVSTORE_DIR}/src/nvstore.c)

target_include_directories(app PRIVATE ${MIGRIS_FSW_PUS_DIR}/include
                                       ${MIGRIS_FSW_FDIR_DIR}/include
                                       ${MIGRIS_FSW_DATAPOOL_DIR}/include
                                       ${MIGRIS_FSW_SCHEDULE_DIR}/include
                                       ${MIGRIS_FSW_HKSTORE_DIR}/include
                                       ${MIGRIS_FSW_PKTSTORE_DIR}/include
                                       ${MIGRIS_FSW_LARGEDATA_DIR}/include
                                       ${MIGRIS_FSW_MODE_DIR}/include
                                       ${MIGRIS_FSW_NVSTORE_DIR}/include)

# Size the freestanding event FIFO, the parameter datapool, the
# schedule store, the packet store, and the PUS-20 / PUS-11 per-TC
# bounds from Kconfig. Applied to the whole `app` target (every lib TU
# above is part of it) so the struct sizes are identical across all
# sample TUs; the host library build has no such defines and uses the
# lib defaults.
target_compile_definitions(
  app
  PRIVATE MIGRIS_FDIR_EVENT_FIFO_CAPACITY=${CONFIG_FSW_FDIR_EVENT_FIFO_CAPACITY}
          MIGRIS_DATAPOOL_CAPACITY=${CONFIG_FSW_DATAPOOL_CAPACITY}
          MIGRIS_PUS20_MAX_PARAMS_PER_TC=${CONFIG_FSW_PUS20_MAX_PARAMS}
          MIGRIS_SCHEDULE_CAPACITY=${CONFIG_FSW_SCHEDULE_CAPACITY}
          MIGRIS_SCHEDULE_TC_MAX=${CONFIG_FSW_SCHEDULE_TC_MAX}
          MIGRIS_HKSTORE_CAPACITY=${CONFIG_FSW_HKSTORE_CAPACITY}
          MIGRIS_HKSTORE_MAX_PARAMS=${CONFIG_FSW_HKSTORE_MAX_PARAMS}
          MIGRIS_PUS11_MAX_PER_TC=${CONFIG_FSW_PUS11_MAX_PER_TC}
          MIGRIS_PKTSTORE_CAPACITY=${CONFIG_FSW_PKTSTORE_CAPACITY}
          MIGRIS_PKTSTORE_PACKET_MAX=${CONFIG_FSW_PKTSTORE_PACKET_MAX}
          MIGRIS_MODE_CAPACITY=${CONFIG_FSW_MODE_CAPACITY})
