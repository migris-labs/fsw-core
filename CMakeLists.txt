cmake_minimum_required(VERSION 3.25)

project(
  migris-fsw-core
  VERSION 0.1.0
  DESCRIPTION "Migris platform — flight-software framework (HAL, PUS, FDIR, mode mgr, scheduler)"
  HOMEPAGE_URL "https://github.com/migris-labs/fsw-core"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Basic standards
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C99 for the freestanding lib/pus codec. Zephyr supports C99/C11 too, so the same translation units
# compile on both toolchains.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds are not allowed. Configure into a build/ directory.")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(MIGRIS_FSW_BUILD_TESTS "Build unit tests" ON)
option(MIGRIS_FSW_BUILD_DOCS "Build Doxygen documentation" OFF)
option(MIGRIS_FSW_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MIGRIS_FSW_ENABLE_UBSAN "Enable UndefinedBehaviourSanitizer" OFF)
option(MIGRIS_FSW_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MIGRIS_FSW_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(MIGRIS_FSW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)

# ---------------------------------------------------------------------------
# Library target: migris::fsw-core
#
# Slice fsw-1 ships a host-side library carrying only the version surface. Real on-board (Zephyr /
# Cortex-M7) code arrives in slice fsw-2 alongside the west workspace; until then this target lives
# purely on the host toolchain so the engineering pipeline (CI, sanitizers, clang-tidy) is durable
# from day one.
# ---------------------------------------------------------------------------
add_library(fsw-core)
add_library(migris::fsw-core ALIAS fsw-core)

# Host-side surface: version (C++) + freestanding PUS codec (C). The PUS sources under lib/pus/ also
# compile against the Cortex-M7 / Zephyr toolchain — the same translation units run on-board, so
# every wire-format byte is covered by host CI (ASan/UBSan/clang-tidy) on every PR.
target_sources(
  fsw-core
  PRIVATE src/version.cpp
          lib/pus/src/ccsds.c
          lib/pus/src/pus_tc.c
          lib/pus/src/pus_tm.c
          lib/pus/src/pus1.c
          lib/pus/src/pus17.c
          lib/pus/src/tc_router.c)

target_include_directories(
  fsw-core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/pus/include>
         $<INSTALL_INTERFACE:include>)

target_compile_features(fsw-core PUBLIC cxx_std_20 c_std_99)

set_target_properties(
  fsw-core
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)

migris_target_compiler_warnings(fsw-core)
migris_target_sanitizers(fsw-core)
migris_target_clang_tidy(fsw-core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MIGRIS_FSW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Docs
# ---------------------------------------------------------------------------
if(MIGRIS_FSW_BUILD_DOCS)
  add_subdirectory(docs)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
  TARGETS fsw-core
  EXPORT migris-fsw-core-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.hpp")

install(
  DIRECTORY lib/pus/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h")

install(
  EXPORT migris-fsw-core-targets
  FILE migris-fsw-core-targets.cmake
  NAMESPACE migris::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/migris-fsw-core)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/migris-fsw-core-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/migris-fsw-core-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/migris-fsw-core)
