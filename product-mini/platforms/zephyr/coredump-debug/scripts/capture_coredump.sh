#!/bin/bash

# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Runs QEMU, captures console output, extracts WAMR call stack and
# Zephyr coredump hex for offline analysis, and symbolicates the
# WASM call stack using addr2line.py.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"
RAW_LOG="${BUILD_DIR}/qemu_output_raw.log"
LOG_FILE="${BUILD_DIR}/qemu_output.log"
COREDUMP_BIN="${BUILD_DIR}/coredump.bin"
WASM_CALL_STACK="${BUILD_DIR}/wasm_call_stack.txt"

WAMR_ROOT=$(cd "${SCRIPT_DIR}/../../../../.." && pwd)
ADDR2LINE="${WAMR_ROOT}/test-tools/addr2line/addr2line.py"

# Tool paths: env var -> default
WASI_SDK_PATH="${WASI_SDK_PATH:-/opt/wasi-sdk}"
WABT_PATH="${WABT_PATH:-/opt/wabt}"

if [ ! -f "${BUILD_DIR}/zephyr/zephyr.elf" ]; then
    echo "Error: build/zephyr/zephyr.elf not found. Run 'west build' first."
    exit 1
fi

# Clean up stale files from previous runs
rm -f "${BUILD_DIR}/qemu.pid" "${RAW_LOG}" "${LOG_FILE}" "${COREDUMP_BIN}" "${WASM_CALL_STACK}"

echo "=== Running QEMU ==="

# Run west build -t run in background. After the crash, Zephyr halts and
# QEMU spins, so we wait a few seconds for all output then kill QEMU.
cd "${PROJECT_DIR}"
west build -t run > "${RAW_LOG}" 2>&1 &
WEST_PID=$!

# Wait for QEMU to boot, crash, and dump coredump
sleep 5

# Kill the process tree (west -> ninja -> QEMU)
kill ${WEST_PID} 2>/dev/null
wait ${WEST_PID} 2>/dev/null

# Also kill QEMU directly if pidfile exists
if [ -f "${BUILD_DIR}/qemu.pid" ]; then
    kill $(cat "${BUILD_DIR}/qemu.pid") 2>/dev/null
    rm -f "${BUILD_DIR}/qemu.pid"
fi

# Strip ANSI escape codes from the raw log. Zephyr's logging backend wraps
# coredump hex lines in color codes (e.g. \e[1;31m ... \e[0m) which breaks
# the coredump_serial_log_parser.py hex parser.
sed 's/\x1b\[[0-9;]*m//g' "${RAW_LOG}" > "${LOG_FILE}"

echo ""
echo "=== WAMR Call Stack (WASM-level) ==="
grep -E "#[0-9]+:.*0x[0-9a-f]+" "${LOG_FILE}" || echo "(no WAMR call stack found)"

echo ""
echo "=== WASM Exception ==="
grep "WASM exception:" "${LOG_FILE}" || echo "(no exception found)"

echo ""
echo "=== WASM Symbolicated Call Stack ==="

# Extract call stack lines for addr2line
grep -E "#[0-9]+:.*0x[0-9a-f]+" "${LOG_FILE}" > "${WASM_CALL_STACK}" 2>/dev/null

if [ ! -s "${WASM_CALL_STACK}" ]; then
    echo "(no WAMR call stack lines to symbolicate)"
else
    # Determine which crash app was built from CMakeCache
    CRASH_APP=$(grep "^CRASH_APP:" "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    if [ -z "${CRASH_APP}" ]; then
        CRASH_APP="oob"
    fi

    WASM_FILE="${BUILD_DIR}/wasm-apps/wasm/${CRASH_APP}.wasm"

    CAN_SYMBOLICATE=true
    if [ ! -f "${ADDR2LINE}" ]; then
        echo "addr2line.py not found at ${ADDR2LINE}"
        CAN_SYMBOLICATE=false
    fi
    if [ ! -f "${WASM_FILE}" ]; then
        echo "Unstripped WASM not found at ${WASM_FILE}"
        echo "(Build with wasi-sdk to generate debug WASM binaries)"
        CAN_SYMBOLICATE=false
    fi
    if [ ! -d "${WASI_SDK_PATH}" ]; then
        echo "wasi-sdk not found at ${WASI_SDK_PATH}"
        echo "Set WASI_SDK_PATH env var or install to /opt/wasi-sdk"
        CAN_SYMBOLICATE=false
    fi
    if [ ! -d "${WABT_PATH}" ]; then
        echo "wabt not found at ${WABT_PATH}"
        echo "Set WABT_PATH env var or install to /opt/wabt"
        CAN_SYMBOLICATE=false
    fi

    if [ "${CAN_SYMBOLICATE}" = true ]; then
        python3 "${ADDR2LINE}" \
            --wasi-sdk "${WASI_SDK_PATH}" \
            --wabt "${WABT_PATH}" \
            --wasm-file "${WASM_FILE}" \
            "${WASM_CALL_STACK}"
    else
        echo ""
        echo "To symbolicate manually:"
        echo "  python3 ${ADDR2LINE} \\"
        echo "      --wasi-sdk \${WASI_SDK_PATH} \\"
        echo "      --wabt \${WABT_PATH} \\"
        echo "      --wasm-file <path-to-unstripped.wasm> \\"
        echo "      ${WASM_CALL_STACK}"
    fi
fi

echo ""
echo "=== Zephyr Coredump ==="
if grep -q "#CD:BEGIN#" "${LOG_FILE}"; then
    # Parse hex log to binary using Zephyr's tool
    if [ -n "${ZEPHYR_BASE}" ] && [ -f "${ZEPHYR_BASE}/scripts/coredump/coredump_serial_log_parser.py" ]; then
        python3 "${ZEPHYR_BASE}/scripts/coredump/coredump_serial_log_parser.py" \
            "${LOG_FILE}" "${COREDUMP_BIN}" 2>&1
        echo ""
        echo "To analyze with GDB:"
        echo "  python3 \${ZEPHYR_BASE}/scripts/coredump/coredump_gdbserver.py ${BUILD_DIR}/zephyr/zephyr.elf ${COREDUMP_BIN}"
        echo "  # Then in another terminal:"
        echo "  # \${ZEPHYR_SDK_INSTALL_DIR}/gnu/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-gdb ${BUILD_DIR}/zephyr/zephyr.elf"
        echo "  # (gdb) target remote localhost:1234"
    else
        echo "Coredump found but ZEPHYR_BASE not set — cannot parse."
        echo "Set ZEPHYR_BASE and re-run, or manually run:"
        echo "  python3 \${ZEPHYR_BASE}/scripts/coredump/coredump_serial_log_parser.py ${LOG_FILE} ${COREDUMP_BIN}"
    fi
else
    echo "(no Zephyr coredump found)"
fi

echo ""
echo "Full QEMU log (ANSI-stripped): ${LOG_FILE}"
