#!/usr/bin/env python3
"""CTest entry point for the hardware-free programmer integration suite."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).resolve().parents[1]/'tools/tests/test_flash_tool.py'),run_name='__main__')
