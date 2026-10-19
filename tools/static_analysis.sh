#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 - "$script_dir/.." "$@" <<'PY'
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

root=Path(sys.argv[1]).resolve()
parser=argparse.ArgumentParser(description='Run Clang static analyzer on every own C++ translation unit in a configured build database')
parser.add_argument('--build-dir',type=Path,default=root/'build/host')
args=parser.parse_args(sys.argv[2:])
build=args.build_dir.resolve()
database=build/'compile_commands.json'
if not database.is_file():
    raise SystemExit('Configure the host build first; compile_commands.json is required')
tidy=os.environ.get('CLANG_TIDY') or shutil.which('clang-tidy-19') or shutil.which('clang-tidy')
if not tidy:
    raise SystemExit('clang-tidy is required; install it or set CLANG_TIDY')
entries=json.loads(database.read_text())
files=set()
for entry in entries:
    path=Path(entry['file'])
    if not path.is_absolute():
        path=Path(entry['directory'])/path
    path=path.resolve()
    if path.is_relative_to(root) and 'third_party' not in path.parts and path.suffix=='.cpp':
        files.add(path)
if not files:
    raise SystemExit('No own C++ translation units in compile database')
extra=[]
if sys.platform=='darwin':
    sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
    extra=['--extra-arg=-isysroot','--extra-arg='+sdk,'--extra-arg=-isystem','--extra-arg='+sdk+'/usr/include/c++/v1']
    # CMake omits compiler-implicit include paths from its compile database.
    # clang-tidy may use a different Clang installation, so restore those paths.
    for config in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
        found=re.search(r'set\(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES \"([^\"]*)\"\)',config.read_text())
        if found:
            for include in found.group(1).split(';'):
                if Path(include).is_dir():
                    extra+=['--extra-arg=-isystem','--extra-arg='+include]
for path in sorted(files):
    result=subprocess.run([tidy,*extra,'-p',str(build),'-checks=-*,clang-analyzer-*','-warnings-as-errors=clang-analyzer-*',str(path)],cwd=root)
    if result.returncode:
        raise SystemExit(result.returncode)
print(f'Clang static analysis passed: {len(files)} translation units from {database}')
PY
