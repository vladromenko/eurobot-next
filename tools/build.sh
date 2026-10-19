#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root_dir"
preset="${1:-stm32-debug}"
if [ "$#" -gt 0 ]; then shift; fi
case "$preset" in host|host-sanitize|stm32-debug|stm32-release|cxx26-probe|stm32-cxx26-probe) ;; *) echo "Unknown preset" >&2; exit 2;; esac
cmake --preset "$preset" "$@"
cmake --build --preset "$preset" --parallel
