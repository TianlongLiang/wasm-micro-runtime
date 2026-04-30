#!/usr/bin/env python3
"""Thin wrapper — delegates to test-tools/struct-layout-checker/.

Maintains backward compatibility for check_struct_layout.cmake which
calls this script directly.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "test-tools", "struct-layout-checker"))
from main import main

sys.exit(main())
