#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
deps="$root_dir/firmware/third_party"
mkdir -p "$deps"
fetch() {
 local name="$1" url="$2" tag="$3" expected="$4"
 if [ ! -d "$deps/$name/.git" ]; then git clone --quiet --depth 1 --branch "$tag" "$url" "$deps/$name"; fi
 local actual
 actual="$(git -C "$deps/$name" rev-parse HEAD)"
 if [ "$actual" != "$expected" ]; then echo "Dependency revision mismatch: $name" >&2; exit 1; fi
 if [ -n "$(git -C "$deps/$name" status --porcelain --untracked-files=no)" ]; then echo "Dependency has modified tracked files: $name" >&2; exit 1; fi
}
fetch FreeRTOS-Kernel https://github.com/FreeRTOS/FreeRTOS-Kernel.git V11.1.0 dbf70559b27d39c1fdb68dfb9a32140b6a6777a0
fetch CMSIS_5 https://github.com/ARM-software/CMSIS_5.git 5.9.0 2b7495b8535bdcb306dac29b9ded4cfb679d7e5c
fetch cmsis_device_f4 https://github.com/STMicroelectronics/cmsis_device_f4.git v2.6.10 5f41fb29d22773896c780052bf61e47fc924d524
