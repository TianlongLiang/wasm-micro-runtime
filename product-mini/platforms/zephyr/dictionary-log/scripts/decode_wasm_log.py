#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
decode_wasm_log.py - Offline decoder for WASM dictionary log packets.

Reads a Zephyr UART hex log stream and decodes both Zephyr native dictionary
packets and WASM dictionary packets (msg_type=0x80) in a single pass.

Usage:
    python3 decode_wasm_log.py \
        --wasm-db build/wasm_log_dict.json \
        [--zephyr-db build/zephyr/log_dictionary.json] \
        /tmp/serial.log --hex
"""

import argparse
import io
import json
import logging
import os
import re
import struct
import sys

try:
    import colorama
    from colorama import Fore, Style
    colorama.init()
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False

logger = logging.getLogger("decode_wasm_log")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Zephyr message types (from log_output_dict.h)
MSG_TYPE_NORMAL = 0x00
MSG_TYPE_DROPPED = 0x01

# WASM dictionary packet type (from lib_wasm_dict_log.c)
MSG_WASM_LOG = 0x80

# WASM log levels (from wasm_log.h) — colors match Zephyr's log_parser.py
if HAS_COLOR:
    WASM_LOG_LEVELS = {
        1: ("err", Fore.RED),
        2: ("wrn", Fore.YELLOW),
        3: ("inf", Fore.GREEN),
        4: ("dbg", Fore.BLUE),
        5: ("verbose", Fore.BLUE),
    }
else:
    WASM_LOG_LEVELS = {
        1: ("err", ""),
        2: ("wrn", ""),
        3: ("inf", ""),
        4: ("dbg", ""),
        5: ("verbose", ""),
    }

# WASM argument type codes (from lib_wasm_dict_log.c)
WASM_LOG_ARG_INT32 = 0x01
WASM_LOG_ARG_INT64 = 0x02
WASM_LOG_ARG_FLOAT64 = 0x03
WASM_LOG_ARG_STRING = 0x04

# ---------------------------------------------------------------------------
# Zephyr native packet parsing (V3 format, 32-bit little-endian target)
# ---------------------------------------------------------------------------

# V3 header after msg_type byte: domain_lvl(1B) + pkg_len(2B) + data_len(2B)
#   + source_id(4B) + timestamp(4B for 32-bit, 8B for 64-bit)
# Total for 32-bit without CONFIG_LOG_TIMESTAMP_64BIT: 1+1+2+2+4+4 = 14 bytes


def calc_zephyr_native_packet_size(data, offset):
    """Calculate the total size of a Zephyr native MSG_NORMAL packet.

    Returns the total packet size (header + payload), or None if the data
    is truncated.
    """
    # Minimum header: type(1) + domain_lvl(1) + pkg_len(2) + data_len(2)
    #                 + source_id(4) + timestamp(4) = 14 bytes
    if offset + 14 > len(data):
        return None

    pkg_len = struct.unpack_from("<H", data, offset + 2)[0]
    data_len = struct.unpack_from("<H", data, offset + 4)[0]

    total = 14 + pkg_len + data_len
    if offset + total > len(data):
        return None

    return total


# ---------------------------------------------------------------------------
# WASM dictionary packet decoding
# ---------------------------------------------------------------------------


def decode_wasm_packet(data, offset, wasm_dbs):
    """Decode a single WASM dictionary packet starting at offset.

    Returns (decoded_text, bytes_consumed) or (None, bytes_consumed) on error.
    The decoded_text is a fully formatted log line.

    wasm_dbs is a dict mapping app_id -> (dictionary, app_name).
    """
    if offset + 14 > len(data):
        logger.debug("Truncated WASM packet header at offset %d", offset)
        return None, len(data) - offset

    msg_type = data[offset]
    app_id = data[offset + 1]
    log_level = data[offset + 2]
    string_id = struct.unpack_from("<H", data, offset + 3)[0]
    timestamp = struct.unpack_from("<Q", data, offset + 5)[0]
    arg_count = data[offset + 13]

    pos = offset + 14

    # Decode arguments
    args = []
    for i in range(arg_count):
        if pos >= len(data):
            logger.warning(
                "Truncated WASM packet at arg %d/%d (offset %d)",
                i, arg_count, offset,
            )
            return None, pos - offset

        atype = data[pos]
        pos += 1

        if atype == WASM_LOG_ARG_INT32:
            if pos + 4 > len(data):
                logger.warning("Truncated int32 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<i", data, pos)[0]
            args.append(val)
            pos += 4

        elif atype == WASM_LOG_ARG_INT64:
            if pos + 8 > len(data):
                logger.warning("Truncated int64 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<q", data, pos)[0]
            args.append(val)
            pos += 8

        elif atype == WASM_LOG_ARG_FLOAT64:
            if pos + 8 > len(data):
                logger.warning("Truncated float64 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<d", data, pos)[0]
            args.append(val)
            pos += 8

        elif atype == WASM_LOG_ARG_STRING:
            if pos + 2 > len(data):
                logger.warning("Truncated string length at offset %d", pos)
                return None, pos - offset
            slen = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            if pos + slen > len(data):
                logger.warning("Truncated string data at offset %d", pos)
                return None, pos - offset
            val = data[pos:pos + slen].decode("utf-8", errors="replace")
            args.append(val)
            pos += slen

        else:
            logger.warning(
                "Unknown arg type 0x%02x at offset %d in WASM packet",
                atype, pos - 1,
            )
            return None, pos - offset

    bytes_consumed = pos - offset

    # Look up the format string
    if app_id not in wasm_dbs:
        logger.warning(
            "Unknown app_id %d in WASM packet at offset %d", app_id, offset
        )
        return None, bytes_consumed

    db, app_name = wasm_dbs[app_id]
    str_key = str(string_id)
    if str_key not in db:
        logger.warning(
            "Unknown string_id %d for app_id %d at offset %d",
            string_id, app_id, offset,
        )
        return None, bytes_consumed

    entry = db[str_key]
    fmt = entry["fmt"]

    # Format the message
    level_info = WASM_LOG_LEVELS.get(log_level, ("lvl%d" % log_level, ""))
    level_str, color = level_info
    reset = Fore.RESET if HAS_COLOR else ""

    try:
        message = fmt % tuple(args)
    except (TypeError, ValueError) as exc:
        logger.warning(
            "Format error for string_id=%d fmt=%r args=%r: %s",
            string_id, fmt, args, exc,
        )
        message = f"[RAW] id={string_id} fmt={fmt!r} args={args!r}"

    line = f"{color}[{timestamp:>10}] <{level_str}> {app_name}: {message}{reset}"
    return line, bytes_consumed


# ---------------------------------------------------------------------------
# Zephyr parser integration (best-effort)
# ---------------------------------------------------------------------------


def try_import_zephyr_parser():
    """Try to import Zephyr's dictionary parser.

    Searches for the parser scripts in:
    1. $ZEPHYR_BASE/scripts/logging/dictionary/
    2. Common install paths: ~/zephyrproject/zephyr/scripts/logging/dictionary/

    Returns a module/class that can parse native packets, or None.
    """
    candidates = []

    zephyr_base = os.environ.get("ZEPHYR_BASE")
    if zephyr_base:
        candidates.append(
            os.path.join(zephyr_base, "scripts", "logging", "dictionary")
        )

    home = os.path.expanduser("~")
    candidates.append(
        os.path.join(home, "zephyrproject", "zephyr", "scripts", "logging", "dictionary")
    )

    parser_dir = None
    for candidate in candidates:
        if os.path.isdir(candidate):
            parser_dir = candidate
            break

    if parser_dir is None:
        logger.debug(
            "Zephyr parser directory not found in any of: %s", candidates
        )
        return None

    logger.debug("Using Zephyr parser from: %s", parser_dir)

    try:
        if parser_dir not in sys.path:
            sys.path.insert(0, parser_dir)
        from dictionary_parser import log_database
        return log_database
    except ImportError as exc:
        logger.debug("Failed to import Zephyr dictionary parser: %s", exc)
        return None


def create_zephyr_parser(zephyr_db_path):
    """Create a Zephyr log parser instance if possible.

    Returns a parser object with a parse_one_msg method, or None.
    """
    log_database_mod = try_import_zephyr_parser()
    if log_database_mod is None:
        return None

    if not os.path.isfile(zephyr_db_path):
        logger.warning("Zephyr database file not found: %s", zephyr_db_path)
        return None

    try:
        database = log_database_mod.LogDatabase.read_json_database(zephyr_db_path)
        if database is None:
            logger.warning("Failed to read Zephyr database: %s", zephyr_db_path)
            return None

        # Import the appropriate version parser
        ver = database.get_version()
        if ver == 3:
            from dictionary_parser.log_parser_v3 import LogParserV3
            return LogParserV3(database)
        elif ver == 1:
            from dictionary_parser.log_parser_v1 import LogParserV1
            return LogParserV1(database)
        else:
            logger.warning("Unsupported Zephyr database version: %d", ver)
            return None
    except Exception as exc:
        logger.debug("Failed to create Zephyr parser: %s", exc)
        return None


# ---------------------------------------------------------------------------
# Main decoding loop
# ---------------------------------------------------------------------------


def extract_wasm_from_text_hexdump(content):
    """Extract WASM dict packets from text-mode hexdump output (dict OFF).

    When Zephyr dictionary logging is OFF, LOG_HEXDUMP produces text like:
        [00:00:00.100,000] <inf> wasm_dict:
          80 00 03 00 00 64 00 00  00 00 00 00 00 00 01 01 |.....d..........|
          2a 00 00 00                                       |*...            |

    This function finds those hexdump blocks and reconstructs binary packets.

    Limitation: if another module logs text containing '> wasm_dict:' at the
    standard module name position, it could produce false positives. This is
    unlikely in practice.
    """
    packets = []
    lines = content.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        # Match Zephyr text log line with wasm_dict as module name
        # Format: [timestamp] <level> wasm_dict:
        if '> wasm_dict:' in line:
            hex_bytes = []
            i += 1
            while i < len(lines):
                hex_line = lines[i].strip()
                if not hex_line:
                    i += 1
                    continue
                # Hex dump lines: "80 00 03 00 ..." possibly with "|...|" ASCII part
                if '|' in hex_line:
                    hex_line = hex_line[:hex_line.index('|')].strip()
                parts = hex_line.split()
                valid_hex = True
                for part in parts:
                    if len(part) == 2 and all(c in '0123456789abcdefABCDEF' for c in part):
                        hex_bytes.append(int(part, 16))
                    else:
                        valid_hex = False
                        break
                if not valid_hex:
                    break
                i += 1
            if hex_bytes and hex_bytes[0] == MSG_WASM_LOG:
                packets.append(bytes(hex_bytes))
        else:
            i += 1
    return packets


def extract_hex_data(content):
    """Extract hex-encoded log data from UART output.

    Two modes:
    1. ##ZLOGV1## found (Zephyr dict ON): all packets (native + WASM) are after
       the separator in a unified stream.
    2. No separator (Zephyr dict OFF): parse text hexdump from wasm_dict module.

    Returns the binary data, or None if no hex data found.
    """
    marker_idx = content.find("##ZLOGV1##")

    if marker_idx >= 0:
        # All packets (native + WASM) are after the separator in unified stream
        after = content[marker_idx + len("##ZLOGV1##"):]
        hexdata = ""
        for c in after:
            if c in "0123456789abcdefABCDEF":
                hexdata += c
            elif c in "\n\r \t":
                continue
            else:
                break
    else:
        # Mode 2: no separator — Zephyr dict is OFF (text output)
        # WASM packets appear as LOG_HEXDUMP text from wasm_dict module
        logger.debug("No ##ZLOGV1## marker — parsing text hexdump from wasm_dict")
        packets = extract_wasm_from_text_hexdump(content)
        if packets:
            hexdata = ''.join(pkt.hex() for pkt in packets)
        else:
            hexdata = ""

    if not hexdata:
        logger.error("No hex data found in input")
        return None

    if len(hexdata) % 2 != 0:
        hexdata = hexdata[:-1]

    try:
        return bytes.fromhex(hexdata)
    except ValueError as exc:
        logger.error("Invalid hex data: %s", exc)
        return None


TIMESTAMP_RE = re.compile(r"\[\s*(\d+)\]")
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")


def extract_timestamp(line):
    """Extract the integer timestamp from a decoded log line for sorting.
    Strips ANSI color codes before matching."""
    stripped = ANSI_ESCAPE_RE.sub("", line)
    m = TIMESTAMP_RE.search(stripped)
    if m:
        return int(m.group(1))
    return 0


def decode_log_stream(data, wasm_dbs, zephyr_parser=None, sort_output=False):
    """Decode the binary log stream.

    Iterates through packets, dispatching based on msg_type:
    - 0x00 (MSG_NORMAL): Zephyr native packet
    - 0x01 (MSG_DROPPED): Zephyr dropped message notification
    - 0x80 (MSG_WASM_LOG): WASM dictionary packet
    - Others: skip with warning

    If sort_output is True, collects all lines and sorts by timestamp
    before printing. Otherwise prints in packet order.

    wasm_dbs is a dict mapping app_id -> (dictionary, app_name).

    Returns counts: (zephyr_count, wasm_count, skipped_count, error_count)
    """
    offset = 0
    zephyr_count = 0
    wasm_count = 0
    skipped_count = 0
    error_count = 0
    lines = [] if sort_output else None

    def emit(text):
        if sort_output:
            lines.append(text)
        else:
            print(text, end="" if text.endswith("\n") else "\n")

    while offset < len(data):
        msg_type = data[offset]

        if msg_type == MSG_TYPE_NORMAL:
            pkt_size = calc_zephyr_native_packet_size(data, offset)
            if pkt_size is None:
                logger.debug(
                    "Truncated Zephyr packet at offset %d, stopping", offset
                )
                break

            # Check if this native packet contains an embedded WASM dict packet
            # (LOG_HEXDUMP data field starting with 0x80)
            wasm_extracted = False
            if offset + 14 <= len(data):
                pkg_len = struct.unpack_from("<H", data, offset + 2)[0]
                data_len = struct.unpack_from("<H", data, offset + 4)[0]
                data_start = offset + 14 + pkg_len

                if (data_len > 0
                        and data_start + data_len <= len(data)
                        and data_start < len(data)
                        and data[data_start] == MSG_WASM_LOG):
                    # Extract and decode the embedded WASM packet
                    wasm_payload = data[data_start:data_start + data_len]
                    line, _ = decode_wasm_packet(wasm_payload, 0, wasm_dbs)
                    if line is not None:
                        emit(line)
                        wasm_count += 1
                    else:
                        error_count += 1
                    wasm_extracted = True

            if not wasm_extracted and zephyr_parser is not None:
                try:
                    buf = io.StringIO()
                    old_stdout = sys.stdout
                    sys.stdout = buf
                    try:
                        _ok, new_offset = zephyr_parser.parse_one_msg(
                            data, offset
                        )
                    finally:
                        sys.stdout = old_stdout
                    captured = buf.getvalue()
                    if captured:
                        for cline in captured.splitlines():
                            if cline:
                                emit(cline)
                    if not _ok:
                        logger.debug(
                            "Zephyr parser failed at offset %d, skipping",
                            offset,
                        )
                except Exception as exc:
                    logger.debug(
                        "Zephyr parser exception at offset %d: %s", offset, exc
                    )

            offset += pkt_size
            zephyr_count += 1

        elif msg_type == MSG_TYPE_DROPPED:
            if offset + 3 > len(data):
                logger.debug("Truncated dropped message at offset %d", offset)
                break
            cnt = struct.unpack_from("<H", data, offset + 1)[0]
            emit(f"--- {cnt} messages dropped ---")
            offset += 3
            zephyr_count += 1

        elif msg_type == MSG_WASM_LOG:
            # Legacy: standalone WASM packet (old direct uart_poll_out approach)
            line, consumed = decode_wasm_packet(data, offset, wasm_dbs)
            if line is not None:
                emit(line)
                wasm_count += 1
            else:
                error_count += 1
            offset += consumed

        elif 0x02 <= msg_type <= 0x7F:
            logger.warning(
                "Unknown Zephyr message type 0x%02x at offset %d, skipping 1 byte",
                msg_type, offset,
            )
            offset += 1
            skipped_count += 1

        else:
            logger.debug(
                "Unknown byte 0x%02x at offset %d, skipping", msg_type, offset
            )
            offset += 1
            skipped_count += 1

    if sort_output and lines:
        lines.sort(key=extract_timestamp)
        for line in lines:
            print(line)

    return zephyr_count, wasm_count, skipped_count, error_count


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def parse_wasm_dbs(wasm_db_args):
    """Parse --wasm-db arguments into {app_id: (dict, app_name)} mapping."""
    dbs = {}
    for arg in wasm_db_args:
        if ':' in arg and arg.split(':')[0].isdigit():
            app_id_str, path = arg.split(':', 1)
            app_id = int(app_id_str)
        else:
            app_id = 0
            path = arg

        try:
            with open(path, 'r') as f:
                db = json.load(f)
        except (IOError, json.JSONDecodeError) as exc:
            logger.error("Failed to load WASM dictionary %s: %s", path, exc)
            sys.exit(1)

        # Derive app name from filename
        basename = os.path.splitext(os.path.basename(path))[0]
        if basename.startswith("wasm_log_dict_"):
            app_name = basename[len("wasm_log_dict_"):] + "_app"
        elif basename == "wasm_log_dict":
            app_name = "wasm_app"
        else:
            app_name = basename

        dbs[app_id] = (db, app_name)
        logger.debug("Loaded app_id=%d (%s): %d entries from %s",
                     app_id, app_name, len(db), path)

    return dbs


def main():
    parser = argparse.ArgumentParser(
        description="Decode WASM dictionary log packets from Zephyr UART hex output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  # Decode WASM packets only
  python3 %(prog)s --wasm-db build/wasm_log_dict.json /tmp/serial.log --hex

  # Also decode Zephyr native packets (requires ZEPHYR_BASE)
  python3 %(prog)s --wasm-db build/wasm_log_dict.json \\
      --zephyr-db build/zephyr/log_dictionary.json /tmp/serial.log --hex
""",
    )
    parser.add_argument(
        "--wasm-db",
        action="append",
        required=True,
        help="WASM dictionary in 'app_id:path' format (e.g., '0:dict.json'). "
             "Without app_id prefix, defaults to 0. Can be repeated.",
    )
    parser.add_argument(
        "--zephyr-db",
        default=None,
        help="Path to the Zephyr log dictionary JSON (log_dictionary.json)",
    )
    parser.add_argument(
        "logfile",
        help="Path to the captured serial/UART log file",
    )
    parser.add_argument(
        "--hex",
        action="store_true",
        help="Input contains ##ZLOGV1## hex-encoded data (default mode for QEMU captures)",
    )
    parser.add_argument(
        "--sort",
        action="store_true",
        help="Sort output by timestamp (needed when WASM and Zephyr packets are in separate hex blocks)",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose/debug output",
    )
    args = parser.parse_args()

    # Configure logging
    log_level = logging.DEBUG if args.verbose else logging.WARNING
    logging.basicConfig(
        level=log_level,
        format="%(levelname)s: %(message)s",
    )

    # Load WASM dictionaries
    wasm_dbs = parse_wasm_dbs(args.wasm_db)

    # Try to set up Zephyr parser
    zephyr_parser = None
    if args.zephyr_db:
        zephyr_parser = create_zephyr_parser(args.zephyr_db)
        if zephyr_parser is not None:
            logger.debug("Zephyr parser initialized successfully")
        else:
            logger.debug(
                "Zephyr parser not available, native packets will be skipped"
            )

    # Read input file
    try:
        with open(args.logfile, "r") as f:
            content = f.read()
    except IOError as exc:
        logger.error("Failed to read log file %s: %s", args.logfile, exc)
        sys.exit(1)

    # Extract binary data
    if args.hex:
        data = extract_hex_data(content)
    else:
        # Raw binary mode (future extension)
        logger.error("Raw binary mode not yet supported, use --hex")
        sys.exit(1)

    if data is None:
        sys.exit(1)

    logger.debug("Decoded %d bytes of binary log data", len(data))

    # Decode
    zephyr_n, wasm_n, skip_n, err_n = decode_log_stream(
        data, wasm_dbs, zephyr_parser, sort_output=args.sort
    )

    # Summary to stderr
    print(
        f"\n--- Decode summary ---\n"
        f"  Zephyr native packets: {zephyr_n}\n"
        f"  WASM dict packets:     {wasm_n}\n"
        f"  Skipped bytes:         {skip_n}\n"
        f"  Decode errors:         {err_n}\n"
        f"  Total binary bytes:    {len(data)}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
