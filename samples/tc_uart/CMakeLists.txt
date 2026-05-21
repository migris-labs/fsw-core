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

target_sources(
  app
  PRIVATE src/main.c
          ${MIGRIS_FSW_PUS_DIR}/src/ccsds.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus_tc.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus_tm.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus1.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus5.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus17.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus3.c
          ${MIGRIS_FSW_PUS_DIR}/src/pus20.c
          ${MIGRIS_FSW_PUS_DIR}/src/tc_router.c
          ${MIGRIS_FSW_FDIR_DIR}/src/event_fifo.c
          ${MIGRIS_FSW_FDIR_DIR}/src/fdir.c
          ${MIGRIS_FSW_DATAPOOL_DIR}/src/datapool.c)

target_include_directories(app PRIVATE ${MIGRIS_FSW_PUS_DIR}/include
                                       ${MIGRIS_FSW_FDIR_DIR}/include
                                       ${MIGRIS_FSW_DATAPOOL_DIR}/include)

# Size the freestanding event FIFO, the parameter datapool, and the
# PUS-20 per-TC parameter bound from Kconfig. Applied to the whole
# `app` target (every lib TU above is part of it) so the struct sizes
# are identical across all sample TUs; the host library build has no
# such defines and uses the lib defaults.
target_compile_definitions(
  app
  PRIVATE MIGRIS_FDIR_EVENT_FIFO_CAPACITY=${CONFIG_FSW_FDIR_EVENT_FIFO_CAPACITY}
          MIGRIS_DATAPOOL_CAPACITY=${CONFIG_FSW_DATAPOOL_CAPACITY}
          MIGRIS_PUS20_MAX_PARAMS_PER_TC=${CONFIG_FSW_PUS20_MAX_PARAMS})
