# Sanitizer wiring. Apply to both compile and link flags; ASan+UBSan may be combined, TSan may not
# be combined with ASan.
include_guard(GLOBAL)

function(migris_target_sanitizers target)
  if(MSVC)
    if(MIGRIS_FSW_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    endif()
    return()
  endif()

  set(_flags "")

  if(MIGRIS_FSW_ENABLE_ASAN)
    list(APPEND _flags -fsanitize=address -fno-omit-frame-pointer)
  endif()

  if(MIGRIS_FSW_ENABLE_UBSAN)
    list(APPEND _flags -fsanitize=undefined -fno-sanitize-recover=undefined)
  endif()

  if(MIGRIS_FSW_ENABLE_TSAN)
    if(MIGRIS_FSW_ENABLE_ASAN)
      message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer; "
                          "enable one or the other, not both.")
    endif()
    list(APPEND _flags -fsanitize=thread -fno-omit-frame-pointer)
  endif()

  if(_flags)
    target_compile_options(${target} PRIVATE ${_flags})
    target_link_options(${target} PRIVATE ${_flags})
  endif()
endfunction()
