#!/bin/bash
set -euo pipefail

# Run a wasm app, capture the WAMR call stack, and symbolicate it using
# addr2line.py with the debug companion.
#
# Usage: ./symbolicate.sh [oob|stackoverflow]

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
APP="${1:-oob}"

if [ "$APP" != "oob" ] && [ "$APP" != "stackoverflow" ]; then
    echo "Usage: $0 [oob|stackoverflow]" >&2
    exit 1
fi

WASI_SDK_PATH="${WASI_SDK_PATH:-/opt/wasi-sdk}"
WABT_PATH="${WABT_PATH:-/opt/wabt}"
WAMR_ROOT="${SCRIPT_DIR}/../.."

BUILD_DIR="${SCRIPT_DIR}/build"
PROD_WASM="${BUILD_DIR}/wasm-apps/${APP}.prod.wasm"
DEBUG_WASM="${BUILD_DIR}/wasm-apps/${APP}.debug.wasm"
IWASM="${BUILD_DIR}/iwasm"

if [ ! -x "${IWASM}" ]; then
    echo "iwasm not found at ${IWASM}" >&2
    echo "Run: mkdir -p build && cd build && cmake .. && make" >&2
    exit 1
fi
if [ ! -f "${PROD_WASM}" ]; then
    echo "Production wasm not found at ${PROD_WASM}" >&2
    exit 1
fi
if [ ! -f "${DEBUG_WASM}" ]; then
    echo "Debug companion not found at ${DEBUG_WASM}" >&2
    exit 1
fi

CALL_STACK_FILE=$(mktemp)
LOG_FILE=$(mktemp)
trap 'rm -f "${CALL_STACK_FILE}" "${LOG_FILE}"' EXIT

echo "=== Running iwasm on ${APP}.prod.wasm (expect crash) ==="
# -f app_main calls the exported app_main directly, bypassing wasi _start.
# This preserves the OOB / stack-overflow trap behavior — running _start
# under -Oz -flto would lower the OOB pattern to `unreachable` and produce
# misleading call-stack info.
"${IWASM}" -f app_main "${PROD_WASM}" 2>&1 | tee "${LOG_FILE}" || true

echo ""
echo "=== Captured call stack ==="
grep -E "^#[0-9]+:" "${LOG_FILE}" > "${CALL_STACK_FILE}" || true
cat "${CALL_STACK_FILE}"

if [ ! -s "${CALL_STACK_FILE}" ]; then
    echo "(no call stack captured)"
    exit 1
fi

echo ""
echo "=== Symbolicated call stack (using debug companion) ==="
python3 "${WAMR_ROOT}/test-tools/addr2line/addr2line.py" \
    --wasi-sdk "${WASI_SDK_PATH}" \
    --wabt "${WABT_PATH}" \
    --wasm-file "${DEBUG_WASM}" \
    "${CALL_STACK_FILE}"
