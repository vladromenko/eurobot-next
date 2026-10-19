#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
preset="${1:-host}"
case "$preset" in host|host-sanitize|stm32-debug|stm32-release|cxx26-probe|stm32-cxx26-probe) ;; *) echo "Unknown preset" >&2; exit 2;; esac
cmake --build "$root_dir/build/$preset" --target clean
