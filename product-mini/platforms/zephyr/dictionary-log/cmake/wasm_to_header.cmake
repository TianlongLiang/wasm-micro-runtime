# Convert a .wasm binary to a C byte array header file.
# Usage: cmake -DWASM_IN=<input.wasm> -DHEADER_OUT=<output.h>
#              [-DARRAY_NAME=<name>] -P wasm_to_header.cmake
#
# If ARRAY_NAME is not set, defaults to "wasm_test_file".

if(NOT DEFINED ARRAY_NAME)
  set(ARRAY_NAME "wasm_test_file")
endif()

file(READ "${WASM_IN}" WASM_HEX HEX)
string(LENGTH "${WASM_HEX}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

set(OUTPUT "unsigned char __aligned(4) ${ARRAY_NAME}[] = {\n")
set(LINE "")
set(COL 0)
math(EXPR LAST_BYTE "${BYTE_COUNT} - 1")

foreach(I RANGE 0 ${LAST_BYTE})
  math(EXPR OFFSET "${I} * 2")
  string(SUBSTRING "${WASM_HEX}" ${OFFSET} 2 BYTE)
  if(I LESS LAST_BYTE)
    string(APPEND LINE "  0x${BYTE},")
  else()
    string(APPEND LINE "  0x${BYTE}")
  endif()
  math(EXPR COL "${COL} + 1")
  if(COL EQUAL 12 OR I EQUAL LAST_BYTE)
    string(APPEND OUTPUT "${LINE}\n")
    set(LINE "")
    set(COL 0)
  endif()
endforeach()

string(APPEND OUTPUT "};\n")
file(WRITE "${HEADER_OUT}" "${OUTPUT}")
message(STATUS "Generated ${HEADER_OUT} (${BYTE_COUNT} bytes, array=${ARRAY_NAME})")
