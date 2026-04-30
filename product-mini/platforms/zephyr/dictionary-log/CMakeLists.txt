# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

option(WAMR_ZEPHYR_DICT_LOG
       "Enable Zephyr dictionary logging for native logs (WASM dict always on)" ON)

if(WAMR_ZEPHYR_DICT_LOG)
  set(OVERLAY_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/overlay-dict.conf")
  message(STATUS "Zephyr dictionary logging: ON (native logs as binary)")
else()
  message(STATUS "Zephyr dictionary logging: OFF (native logs as text, WASM dict still active)")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(wamr_dictionary_log LANGUAGES C)

# --- Find wasi-sdk ---
if(NOT DEFINED WASI_SDK_DIR)
  foreach(_dir /opt/wasi-sdk /opt/wasi-sdk-25.0-x86_64-linux
               /opt/wasi-sdk-29.0-x86_64-linux $ENV{WASI_SDK_DIR})
    if(EXISTS "${_dir}/bin/clang")
      set(WASI_SDK_DIR "${_dir}")
      break()
    endif()
  endforeach()
endif()

# --- Baseline WASM build ---
set(WASM_APPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wasm-apps)
set(WASM_BASELINE_SRC ${WASM_APPS_DIR}/log_heavy_app.c)
set(WASM_BASELINE_OUT ${CMAKE_CURRENT_BINARY_DIR}/log_heavy_app_baseline.wasm)
set(WASM_BASELINE_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/lib-wamr-zephyr/test_wasm_baseline.h)

if(WASI_SDK_DIR)
  message(STATUS "WASI SDK: ${WASI_SDK_DIR}")
  set(WASI_CLANG ${WASI_SDK_DIR}/bin/clang)

  add_custom_command(
    OUTPUT ${WASM_BASELINE_OUT}
    COMMAND ${WASI_CLANG}
      --target=wasm32 -O2
      -DCUR_LOG_LEVEL=4
      -I ${WASM_APPS_DIR}
      -z stack-size=8192
      -Wl,--initial-memory=131072
      -Wl,--export=main
      -Wl,--export=__heap_base,--export=__data_end
      -Wl,--no-entry
      -Wl,--allow-undefined
      -nostdlib
      -o ${WASM_BASELINE_OUT} ${WASM_BASELINE_SRC}
    DEPENDS ${WASM_BASELINE_SRC} ${WASM_APPS_DIR}/wasm_log.h
    COMMENT "Compiling WASM baseline: log_heavy_app.c -> log_heavy_app_baseline.wasm"
  )

  add_custom_command(
    OUTPUT ${WASM_BASELINE_HEADER}
    COMMAND ${CMAKE_COMMAND}
      -DWASM_IN=${WASM_BASELINE_OUT}
      -DHEADER_OUT=${WASM_BASELINE_HEADER}
      -DARRAY_NAME=wasm_test_file_baseline
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm_to_header.cmake
    DEPENDS ${WASM_BASELINE_OUT}
    COMMENT "Generating WASM header: log_heavy_app_baseline.wasm -> test_wasm_baseline.h"
  )

  add_custom_target(wasm_baseline DEPENDS ${WASM_BASELINE_HEADER})

  # --- Dictionary mode: two-pass build ---
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  set(EXTRACT_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_log_strings.py)
  set(WASM_DICT_SRC ${CMAKE_CURRENT_BINARY_DIR}/log_heavy_app_dict.c)
  set(WASM_DICT_JSON ${CMAKE_CURRENT_BINARY_DIR}/wasm_log_dict.json)
  set(WASM_DICT_OUT ${CMAKE_CURRENT_BINARY_DIR}/log_heavy_app_dict.wasm)
  set(WASM_DICT_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/lib-wamr-zephyr/test_wasm_dict.h)

  # Pass 1: extract strings and generate transformed source
  add_custom_command(
    OUTPUT ${WASM_DICT_SRC} ${WASM_DICT_JSON}
    COMMAND ${Python3_EXECUTABLE} ${EXTRACT_SCRIPT}
      ${WASM_BASELINE_SRC}
      -o ${WASM_DICT_SRC}
      -j ${WASM_DICT_JSON}
    DEPENDS ${WASM_BASELINE_SRC} ${EXTRACT_SCRIPT}
    COMMENT "Pass 1: Extracting log strings from log_heavy_app.c"
  )

  # Pass 2: compile dictionary WASM (no format strings)
  add_custom_command(
    OUTPUT ${WASM_DICT_OUT}
    COMMAND ${WASI_CLANG}
      --target=wasm32 -O2
      -DWASM_LOG_DICT
      -DCUR_LOG_LEVEL=4
      -I ${WASM_APPS_DIR}
      -z stack-size=8192
      -Wl,--initial-memory=131072
      -Wl,--export=main
      -Wl,--export=__heap_base,--export=__data_end
      -Wl,--no-entry
      -Wl,--allow-undefined
      -nostdlib
      -o ${WASM_DICT_OUT} ${WASM_DICT_SRC}
    DEPENDS ${WASM_DICT_SRC}
    COMMENT "Pass 2: Compiling dict WASM: log_heavy_app_dict.c -> log_heavy_app_dict.wasm"
  )

  # Convert to C header
  add_custom_command(
    OUTPUT ${WASM_DICT_HEADER}
    COMMAND ${CMAKE_COMMAND}
      -DWASM_IN=${WASM_DICT_OUT}
      -DHEADER_OUT=${WASM_DICT_HEADER}
      -DARRAY_NAME=wasm_test_file_dict
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm_to_header.cmake
    DEPENDS ${WASM_DICT_OUT}
    COMMENT "Generating header: log_heavy_app_dict.wasm -> test_wasm_dict.h"
  )

  add_custom_target(wasm_dict DEPENDS ${WASM_DICT_HEADER} ${WASM_DICT_JSON})

  # --- Size comparison ---
  add_custom_command(TARGET wasm_dict POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "===== WASM Size Comparison ====="
    COMMAND ls -la ${WASM_BASELINE_OUT}
    COMMAND ls -la ${WASM_DICT_OUT}
    COMMAND ${CMAKE_COMMAND} -E echo "================================"
  )

  # --- Network app: dictionary mode build ---
  set(WASM_NET_SRC ${WASM_APPS_DIR}/log_app_network.c)
  set(WASM_NET_DICT_SRC ${CMAKE_CURRENT_BINARY_DIR}/log_app_network_dict.c)
  set(WASM_NET_DICT_JSON ${CMAKE_CURRENT_BINARY_DIR}/wasm_log_dict_network.json)
  set(WASM_NET_DICT_OUT ${CMAKE_CURRENT_BINARY_DIR}/log_app_network_dict.wasm)
  set(WASM_NET_DICT_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/lib-wamr-zephyr/test_wasm_network.h)

  # Pass 1: extract strings and generate transformed source
  add_custom_command(
    OUTPUT ${WASM_NET_DICT_SRC} ${WASM_NET_DICT_JSON}
    COMMAND ${Python3_EXECUTABLE} ${EXTRACT_SCRIPT}
      ${WASM_NET_SRC}
      -o ${WASM_NET_DICT_SRC}
      -j ${WASM_NET_DICT_JSON}
    DEPENDS ${WASM_NET_SRC} ${EXTRACT_SCRIPT}
    COMMENT "Pass 1: Extracting log strings from log_app_network.c"
  )

  # Pass 2: compile dictionary WASM (no format strings)
  add_custom_command(
    OUTPUT ${WASM_NET_DICT_OUT}
    COMMAND ${WASI_CLANG}
      --target=wasm32 -O2
      -DWASM_LOG_DICT
      -DCUR_LOG_LEVEL=4
      -I ${WASM_APPS_DIR}
      -z stack-size=8192
      -Wl,--initial-memory=131072
      -Wl,--export=main
      -Wl,--export=__heap_base,--export=__data_end
      -Wl,--no-entry
      -Wl,--allow-undefined
      -nostdlib
      -o ${WASM_NET_DICT_OUT} ${WASM_NET_DICT_SRC}
    DEPENDS ${WASM_NET_DICT_SRC}
    COMMENT "Pass 2: Compiling dict WASM: log_app_network_dict.c -> log_app_network_dict.wasm"
  )

  # Convert to C header
  add_custom_command(
    OUTPUT ${WASM_NET_DICT_HEADER}
    COMMAND ${CMAKE_COMMAND}
      -DWASM_IN=${WASM_NET_DICT_OUT}
      -DHEADER_OUT=${WASM_NET_DICT_HEADER}
      -DARRAY_NAME=wasm_test_file_network
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm_to_header.cmake
    DEPENDS ${WASM_NET_DICT_OUT}
    COMMENT "Generating header: log_app_network_dict.wasm -> test_wasm_network.h"
  )

  add_custom_target(wasm_network DEPENDS ${WASM_NET_DICT_HEADER} ${WASM_NET_DICT_JSON})

else()
  message(STATUS "WASI SDK not found - using pre-built test_wasm_baseline.h")
  add_custom_target(wasm_baseline)
  add_custom_target(wasm_dict)
  add_custom_target(wasm_network)
endif()

# --- WAMR library ---
add_subdirectory(lib-wamr-zephyr)

# Ensure the WASM header is generated before compiling the WAMR library
add_dependencies(wamr_lib wasm_baseline wasm_dict wasm_network)

set(WAMR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

target_link_libraries(app PRIVATE wamr_lib)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/lib-wamr-zephyr
  ${WAMR_ROOT}/core/iwasm/include
  ${WAMR_ROOT}/core/shared/utils
  ${WAMR_ROOT}/core/shared/platform/include
  ${WAMR_ROOT}/core/shared/platform/zephyr
)

target_sources(app PRIVATE
  src/main.c
)

# Force kernel symbols needed by WAMR to be linked
zephyr_link_libraries(
  -Wl,--undefined=z_impl_k_msgq_put
  -Wl,--undefined=z_impl_k_msgq_get
  -Wl,--undefined=k_msgq_init
  -Wl,--undefined=z_impl_k_msgq_purge
  -Wl,--undefined=z_impl_k_sem_init
  -Wl,--undefined=z_impl_k_sem_take
  -Wl,--undefined=z_impl_k_sem_give
)
if(CONFIG_USERSPACE AND CONFIG_DYNAMIC_OBJECTS)
  zephyr_link_libraries(
    -Wl,--undefined=z_impl_k_condvar_init
    -Wl,--undefined=z_impl_k_condvar_signal
    -Wl,--undefined=z_impl_k_condvar_wait
    -Wl,--undefined=z_impl_k_condvar_broadcast
  )
endif()
