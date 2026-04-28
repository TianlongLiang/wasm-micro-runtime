# CMake function to check WASM-host struct layout consistency at configure time.
#
# Usage:
#   include(cmake/check_struct_layout.cmake)
#   check_wasm_struct_layout(
#     SOURCE       src/native_impl.c    # C file with NativeSymbol arrays
#     NATIVE_CC    gcc                   # native compiler (default: CMAKE_C_COMPILER)
#     NATIVE_FLAGS "-m32"               # extra native flags (optional)
#     WASI_SDK     /opt/wasi-sdk         # wasi-sdk path (required)
#     INCLUDE_DIRS shared/               # extra include dirs (optional, auto-discovered)
#     VERBOSE                            # show full field-by-field table (optional)
#     FATAL_ON_MISMATCH                   # treat mismatch as FATAL_ERROR (optional)
#   )
#
# On mismatch: emits CMake WARNING (or FATAL_ERROR if FATAL_ON_MISMATCH).
# On success or missing tools: STATUS message only, build continues.

function(check_wasm_struct_layout)
  # cmake_parse_arguments splits arguments into three categories:
  #   Line 1: boolean flags      — present = ON, absent = OFF (no value)
  #   Line 2: single-value args  — each takes exactly one value
  #   Line 3: multi-value args   — each takes a list of values
  cmake_parse_arguments(CHK
    "VERBOSE;FATAL_ON_MISMATCH"                     # boolean flags
    "SOURCE;NATIVE_CC;NATIVE_FLAGS;WASI_SDK"        # single-value args
    "INCLUDE_DIRS;SOURCES"                          # multi-value args
    ${ARGN}
  )

  # --- Validate required arguments and tools ---
  # Support both SOURCE (single file) and SOURCES (multiple files)
  if(CHK_SOURCES)
    set(_sources ${CHK_SOURCES})
  elseif(CHK_SOURCE)
    set(_sources ${CHK_SOURCE})
  else()
    message(WARNING "check_wasm_struct_layout: no source files specified")
    return()
  endif()

  if(NOT CHK_WASI_SDK OR NOT EXISTS "${CHK_WASI_SDK}/bin/clang")
    message(STATUS "check_wasm_struct_layout: wasi-sdk not found, skipping")
    return()
  endif()

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(NOT Python3_FOUND)
    message(STATUS "check_wasm_struct_layout: Python3 not found, skipping")
    return()
  endif()

  set(CHECKER "${CMAKE_CURRENT_LIST_DIR}/check_struct_layout.py")
  if(NOT EXISTS "${CHECKER}")
    message(WARNING "check_wasm_struct_layout: ${CHECKER} not found")
    return()
  endif()

  # --- Build command ---

  # Default native compiler to the project's C compiler
  if(NOT CHK_NATIVE_CC)
    set(CHK_NATIVE_CC "${CMAKE_C_COMPILER}")
  endif()

  set(CMD ${Python3_EXECUTABLE} "${CHECKER}" "--source")
  foreach(src IN LISTS _sources)
    list(APPEND CMD "${src}")
  endforeach()
  list(APPEND CMD "--native-cc" "${CHK_NATIVE_CC}" "--wasi-sdk" "${CHK_WASI_SDK}")

  if(CHK_NATIVE_FLAGS)
    list(APPEND CMD "--native-flags=${CHK_NATIVE_FLAGS}")
  endif()

  foreach(dir IN LISTS CHK_INCLUDE_DIRS)
    list(APPEND CMD "--include-dir" "${dir}")
  endforeach()

  # Verbose: show full table. Otherwise: quiet (only mismatches + summary).
  if(CHK_VERBOSE)
    list(APPEND CMD "--verbose")
  else()
    list(APPEND CMD "--quiet")
  endif()

  # --- Run checker ---

  message(STATUS "Checking WASM-host struct layout consistency...")

  execute_process(
    COMMAND ${CMD}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err
    TIMEOUT 30
  )

  # Show full report when verbose
  if(CHK_VERBOSE AND out)
    message(STATUS "${out}")
  endif()

  # --- Report results ---

  if(rc EQUAL 0)
    message(STATUS "Struct layout check: PASS")

  elseif(rc EQUAL 1)
    # Extract fix suggestions between FIX_SUGGESTIONS_BEGIN / END markers
    set(fixes "")
    string(FIND "${out}" "FIX_SUGGESTIONS_BEGIN" _begin)
    string(FIND "${out}" "FIX_SUGGESTIONS_END" _end)
    if(NOT _begin EQUAL -1 AND NOT _end EQUAL -1)
      string(LENGTH "FIX_SUGGESTIONS_BEGIN" _len)
      math(EXPR _start "${_begin} + ${_len} + 1")
      math(EXPR _count "${_end} - ${_start}")
      if(_count GREATER 0)
        string(SUBSTRING "${out}" ${_start} ${_count} fixes)
      endif()
    endif()

    if(CHK_FATAL_ON_MISMATCH)
      message(FATAL_ERROR
        "WASM-host struct layout MISMATCH detected!\n"
        "\n"
        "Suggested fixes:\n"
        "${fixes}"
      )
    else()
      message(WARNING
        "WASM-host struct layout MISMATCH detected!\n"
        "\n"
        "Suggested fixes:\n"
        "${fixes}"
      )
    endif()

  else()
    message(STATUS "Struct layout check: tool error (exit ${rc})")
    if(err)
      message(STATUS "${err}")
    endif()
  endif()

  # Report void*/buffer pointer warnings (informational, not an error)
  string(FIND "${out}" "VOID_PTR_WARNINGS_BEGIN" _vp_begin)
  if(NOT _vp_begin EQUAL -1)
    string(FIND "${out}" "VOID_PTR_WARNINGS_END" _vp_end)
    if(NOT _vp_end EQUAL -1)
      string(LENGTH "VOID_PTR_WARNINGS_BEGIN" _vp_len)
      math(EXPR _vp_start "${_vp_begin} + ${_vp_len} + 1")
      math(EXPR _vp_count "${_vp_end} - ${_vp_start}")
      if(_vp_count GREATER 0)
        string(SUBSTRING "${out}" ${_vp_start} ${_vp_count} _vp_lines)
        message(STATUS
          "Note: some native APIs use void*/buffer pointers whose "
          "struct layout cannot be verified automatically:\n${_vp_lines}")
      endif()
    endif()
  endif()
endfunction()
