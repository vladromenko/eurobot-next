#!/usr/bin/env python3
import json, subprocess, sys, tempfile
from pathlib import Path
compiler=sys.argv[1]
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/"probe.cpp"; obj=Path(d)/"probe.o"
 src.write_text("#include <array>\n#include <span>\n#if __cplusplus < 202400L\n#error C++26 mode missing\n#endif\nint f(){std::array<int,1>a{};return std::span<int>{a}[0];}\n")
 flags=["-mcpu=cortex-m4","-mthumb","-mfpu=fpv4-sp-d16","-mfloat-abi=hard"] if "arm-none-eabi" in compiler else []
 r=subprocess.run([compiler,"-std=c++2c",*flags,"-c",str(src),"-o",str(obj)],capture_output=True,text=True)
 print(json.dumps({"compiler":compiler,"mode":"c++2c","supported_probe":r.returncode==0,"diagnostic":r.stderr,"decision":"Default remains C++23; experimental mode does not prove complete C++26 support"},indent=2))
