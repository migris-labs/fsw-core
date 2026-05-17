find_package(GTest REQUIRED)

add_executable(
  fsw-core-tests
  version_test.cpp
  pus_ccsds_test.cpp
  pus_secondary_test.cpp
  pus1_test.cpp
  pus5_test.cpp
  pus17_test.cpp
  pus3_test.cpp
  tc_router_test.cpp
  event_fifo_test.cpp
  fdir_test.cpp)

target_link_libraries(fsw-core-tests PRIVATE migris::fsw-core GTest::gtest GTest::gtest_main)

target_compile_features(fsw-core-tests PRIVATE cxx_std_20)

migris_target_compiler_warnings(fsw-core-tests)
migris_target_sanitizers(fsw-core-tests)
migris_target_clang_tidy(fsw-core-tests)

include(GoogleTest)
gtest_discover_tests(fsw-core-tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS "unit")
