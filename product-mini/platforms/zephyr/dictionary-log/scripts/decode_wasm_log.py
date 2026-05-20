#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
decode_wasm_log.py - Offline decoder for WASM dictionary log packets.

Usage:
    python3 decode_wasm_log.py \
        --wasm-db build/unified_wasm_dict.json \
        [--zephyr-db build/zephyr/log_dictionary.json] \
        /tmp/serial.log --hex
"""

import argparse
import logging
import sys

from wasm_log_common import (
    parse_wasm_dbs,
    detect_timestamp_format,
    detect_native_color,
    detect_native_colors_per_level,
    TS_FORMAT_RAW,
)
from decode_dict_on import decode as decode_dict_on
from decode_dict_off import decode as decode_dict_off

logger = logging.getLogger("decode_wasm_log")


class DecodeState:
    """Holds decode configuration state passed to decoder modules."""
    def __init__(self, ts_format, native_has_color, binary_mode,
                 auto_color, detected_colors):
        self.ts_format = ts_format
        self.native_has_color = native_has_color
        self.binary_mode = binary_mode
        self.auto_color = auto_color
        self.detected_colors = detected_colors


def main():
    parser = argparse.ArgumentParser(
        description="Decode WASM dictionary log packets from Zephyr UART hex output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python3 %(prog)s --wasm-db build/unified_wasm_dict.json /tmp/serial.log --hex

  python3 %(prog)s --wasm-db build/unified_wasm_dict.json \\
      --zephyr-db build/zephyr/log_dictionary.json /tmp/serial.log --hex
""",
    )
    parser.add_argument("--wasm-db", required=True,
                        help="Path to unified WASM dictionary JSON")
    parser.add_argument("--zephyr-db", default=None,
                        help="Path to the Zephyr log dictionary JSON")
    parser.add_argument("logfile",
                        help="Path to the captured serial/UART log file")
    parser.add_argument("--hex", action="store_true",
                        help="Input contains ##ZLOGV1## hex-encoded data")
    parser.add_argument("--sort", action="store_true",
                        help="Sort output by timestamp")
    parser.add_argument("--auto-color", action="store_true",
                        help="Auto-detect color scheme from native log lines")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable verbose/debug output")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.WARNING,
        format="%(levelname)s: %(message)s",
    )

    wasm_dbs = parse_wasm_dbs(args.wasm_db)

    try:
        with open(args.logfile, "r") as f:
            content = f.read()
    except IOError as exc:
        logger.error("Failed to read log file %s: %s", args.logfile, exc)
        sys.exit(1)

    binary_mode = "##ZLOGV1##" in content
    auto_color = args.auto_color and not binary_mode

    ts_format = detect_timestamp_format(content)
    native_has_color = detect_native_color(content)
    detected_colors = {}

    if binary_mode:
        ts_format = TS_FORMAT_RAW
        native_has_color = True
    elif native_has_color and auto_color:
        detected_colors = detect_native_colors_per_level(content)

    decode_state = DecodeState(
        ts_format=ts_format,
        native_has_color=native_has_color,
        binary_mode=binary_mode,
        auto_color=auto_color,
        detected_colors=detected_colors,
    )

    if binary_mode:
        if not args.zephyr_db:
            print("Warning: binary dict mode detected (##ZLOGV1## present) but "
                  "--zephyr-db not provided.\n"
                  "  Native Zephyr log packets will not be decoded.\n"
                  "  Add --zephyr-db build/zephyr/log_dictionary.json to see native logs.\n",
                  file=sys.stderr)

        zephyr_n, wasm_n, skip_n, err_n = decode_dict_on(
            content, wasm_dbs, args.zephyr_db, decode_state,
            sort_output=args.sort
        )
        print(f"\n--- Decode summary ---\n"
              f"  Zephyr native packets: {zephyr_n}\n"
              f"  WASM dict packets:     {wasm_n}\n"
              f"  Skipped bytes:         {skip_n}\n"
              f"  Decode errors:         {err_n}",
              file=sys.stderr)
    else:
        logger.debug("No ##ZLOGV1## -- using text mode (dict OFF)")
        wasm_n, err_n = decode_dict_off(content, wasm_dbs, decode_state)
        print(f"\n--- Decode summary (text mode) ---\n"
              f"  WASM dict packets decoded: {wasm_n}\n"
              f"  Decode errors:             {err_n}\n"
              f"  (Native logs passed through as-is)",
              file=sys.stderr)


if __name__ == "__main__":
    main()
