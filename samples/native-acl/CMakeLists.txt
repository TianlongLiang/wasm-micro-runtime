cmake_minimum_required(VERSION 3.14)
project(native_acl)

string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} WAMR_BUILD_PLATFORM)

set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_NATIVE_API_ACL 1)

set(WAMR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(vmlib ${WAMR_RUNTIME_LIB_SOURCE})

include_directories(src)
include (${SHARED_DIR}/utils/uncommon/shared_uncommon.cmake)

add_executable(native_acl src/main.c src/native_impl.c ${UNCOMMON_SHARED_SOURCE})

target_link_libraries(native_acl vmlib -lm -ldl -lpthread)
