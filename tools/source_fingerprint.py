"""Fingerprint application inputs, excluding generated builds and upstream dependencies."""
from __future__ import annotations
import hashlib
from pathlib import Path


def fingerprint(root: Path) -> str:
    files = [root / "CMakeLists.txt", root / "CMakePresets.json",
             root / "tools/dependencies.sh", root / "tools/image_manifest.py",
             root / "tools/source_fingerprint.py"]
    suffixes = {".cpp", ".hpp", ".h", ".ld", ".cmake", ".s", ".S"}
    files.extend(path for path in (root / "firmware").rglob("*")
                 if path.is_file() and "third_party" not in path.relative_to(root).parts
                 and (path.suffix in suffixes or path.name == "CMakeLists.txt"))
    digest = hashlib.sha256()
    for path in sorted(set(files)):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def validation_fingerprint(root: Path) -> str:
    digest = hashlib.sha256(fingerprint(root).encode("ascii"))
    for name in ("Dockerfile.ros2", ".dockerignore", ".clang-format"):
        path=root/name
        digest.update(name.encode("utf-8")+b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    for area in ("ros2_ws/src", "ros2_ws/tools", "tools", "tests", "examples", ".github/workflows"):
        for path in sorted((root / area).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
                digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
                digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()
