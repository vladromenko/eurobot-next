#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
preset="${1:-host}"
case "$preset" in host|host-sanitize) ;; *) echo "Test preset must be host or host-sanitize" >&2; exit 2;; esac
"$root_dir/tools/build.sh" "$preset"
cd "$root_dir"
ctest --preset "$preset"
