#!/usr/bin/env python3
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
errors=[]
for tree in ("firmware/src","firmware/include","ros2_ws/src","tests"):
 for path in (root/tree).rglob("*"):
  if path.is_file() and path.suffix in (".cpp",".hpp",".c"):
   if path.suffix==".c":errors.append(str(path)+": own C source forbidden")
   if re.search(r"\bcontinue\s*;",path.read_text()):errors.append(str(path)+": forbidden loop statement")
print("\n".join(errors) if errors else "C++ source policy PASS")
raise SystemExit(bool(errors))
