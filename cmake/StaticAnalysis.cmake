# clang-tidy integration. Off by default; enable with MIGRIS_FSW_ENABLE_CLANG_TIDY (see the `tidy`
# CMake preset).
include_guard(GLOBAL)

function(migris_target_clang_tidy target)
  if(NOT MIGRIS_FSW_ENABLE_CLANG_TIDY)
    return()
  endif()

  find_program(CLANG_TIDY_EXE NAMES clang-tidy-19 clang-tidy-18 clang-tidy-17 clang-tidy REQUIRED)

  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY
                                             "${CLANG_TIDY_EXE};--warnings-as-errors=*")
endfunction()
