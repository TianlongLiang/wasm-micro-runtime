#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
stitch_wasm_dicts.py — Merge per-app WASM dictionary JSONs into a unified format.

Usage:
    python3 stitch_wasm_dicts.py \
        --app 0:sensor_app:path/sensor.json \
        --app 1:network_app:path/network.json \
        -o output.json

Output format:
    [
      {"app_id": 0, "app_name": "sensor_app", "dict": { ...original json... }},
      {"app_id": 1, "app_name": "network_app", "dict": { ...original json... }}
    ]
"""

import argparse
import json
import os
import sys


def parse_app_arg(arg_str):
    """Parse an --app argument in 'app_id:app_name:path' format.

    Returns (app_id: int, app_name: str, path: str) or raises ValueError.
    """
    parts = arg_str.split(':', maxsplit=2)
    if len(parts) != 3:
        raise ValueError(
            f"Invalid --app format: '{arg_str}'. "
            f"Expected format: app_id:app_name:path (3 colon-separated parts)"
        )

    app_id_str, app_name, path = parts

    try:
        app_id = int(app_id_str)
    except ValueError:
        raise ValueError(
            f"Invalid app_id '{app_id_str}': must be an integer"
        )

    return app_id, app_name, path


def main():
    parser = argparse.ArgumentParser(
        description='Merge per-app WASM dictionary JSONs into a unified format.'
    )
    parser.add_argument(
        '--app', action='append', dest='apps', metavar='ID:NAME:PATH',
        help='App specification: app_id:app_name:path_to_dict.json (can be repeated)'
    )
    parser.add_argument(
        '-o', '--output', required=True,
        help='Output JSON file path'
    )

    args = parser.parse_args()

    if not args.apps:
        print("Error: at least one --app argument is required", file=sys.stderr)
        sys.exit(1)

    entries = []
    seen_ids = {}

    for app_str in args.apps:
        # Parse the argument
        try:
            app_id, app_name, path = parse_app_arg(app_str)
        except ValueError as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

        # Check for duplicate app_id
        if app_id in seen_ids:
            print(
                f"Error: duplicate app_id {app_id} "
                f"(already used by '{seen_ids[app_id]}')",
                file=sys.stderr
            )
            sys.exit(1)
        seen_ids[app_id] = app_name

        # Check file exists
        if not os.path.isfile(path):
            print(f"Error: file not found: '{path}'", file=sys.stderr)
            sys.exit(1)

        # Load JSON
        try:
            with open(path, 'r') as f:
                dict_content = json.load(f)
        except json.JSONDecodeError as e:
            print(f"Error: invalid JSON in '{path}': {e}", file=sys.stderr)
            sys.exit(1)

        entries.append({
            "app_id": app_id,
            "app_name": app_name,
            "dict": dict_content
        })

    # Sort by app_id
    entries.sort(key=lambda x: x["app_id"])

    # Write output
    with open(args.output, 'w') as f:
        json.dump(entries, f, indent=2)

    # Summary to stderr
    print(
        f"Stitched {len(entries)} app dict(s) -> {args.output}",
        file=sys.stderr
    )


if __name__ == '__main__':
    main()
