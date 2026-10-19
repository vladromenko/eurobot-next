#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 - "$script_dir/.." "$@" <<'PY'
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

root=Path(sys.argv[1]).resolve()
mode=sys.argv[2] if len(sys.argv)>2 else '--check'
if mode not in ('--check','--fix') or len(sys.argv)>3:
    raise SystemExit('Usage: tools/format.sh [--check|--fix] (clang-format 19)')
formatter=os.environ.get('CLANG_FORMAT') or shutil.which('clang-format-19') or shutil.which('clang-format')
if not formatter:
    raise SystemExit('clang-format 19 is required; install it or set CLANG_FORMAT')
version=subprocess.check_output([formatter,'--version'],text=True)
if re.search(r'clang-format version 19\.',version) is None:
    raise SystemExit('Formatting is pinned to clang-format 19: '+version.strip())
files=[]
for area in ('firmware/include','firmware/src','tests','examples','ros2_ws/src'):
    directory=root/area
    if directory.exists():
        files.extend(p for p in directory.rglob('*') if p.suffix in ('.cpp','.hpp','.h') and p.is_file())
if not files:
    raise SystemExit('No application source files found')
arguments=['-i'] if mode=='--fix' else ['--dry-run','--Werror']
raise SystemExit(subprocess.run([formatter,*arguments,*map(str,sorted(set(files)))],cwd=root).returncode)
PY
