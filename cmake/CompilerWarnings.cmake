# Warnings applied to every target we own. Kept deliberately strict: if a warning is noisy in
# practice, fix the code rather than silencing the whole category.
include_guard(GLOBAL)

function(migris_target_compiler_warnings target)
  set(_clang_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wconversion
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough)

  set(_gcc_warnings ${_clang_warnings} -Wmisleading-indentation -Wduplicated-cond
                    -Wduplicated-branches -Wlogical-op -Wuseless-cast)

  set(_msvc_warnings
      /W4
      /permissive-
      /w14242
      /w14254
      /w14263
      /w14265
      /w14287
      /we4289
      /w14296
      /w14311
      /w14545
      /w14546
      /w14547
      /w14549
      /w14555
      /w14619
      /w14640
      /w14826
      /w14905
      /w14906
      /w14928)

  if(MSVC)
    set(_warnings ${_msvc_warnings})
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_warnings ${_clang_warnings})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_warnings ${_gcc_warnings})
  else()
    set(_warnings ${_clang_warnings})
  endif()

  if(MIGRIS_FSW_WARNINGS_AS_ERRORS)
    if(MSVC)
      list(APPEND _warnings /WX)
    else()
      list(APPEND _warnings -Werror)
    endif()
  endif()

  target_compile_options(${target} PRIVATE ${_warnings})
endfunction()
