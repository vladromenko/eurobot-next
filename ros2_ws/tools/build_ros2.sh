#!/usr/bin/env bash
# Explicit ROS build/test runner. Never starts a daemon or opens a device.
set -euo pipefail
runner_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$runner_dir/../.." && pwd)
mode=native
distro=jazzy
while (($#)); do
  case "$1" in
    --native) mode=native; shift ;;
    --docker) mode=docker; shift ;;
    --distro)
      if (($# < 2)); then echo '--distro requires jazzy or lyrical' >&2; exit 2; fi
      distro=$2; shift 2 ;;
    --help)
      echo 'Usage: ros2_ws/tools/build_ros2.sh [--native|--docker] [--distro jazzy|lyrical]'
      echo 'Builds all ROS packages, then runs transport and ROS action tests without hardware.'
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$distro" in jazzy|lyrical) ;; *) echo 'Supported distributions: jazzy, lyrical' >&2; exit 2 ;; esac
if [[ "$mode" == docker ]]; then
  command -v docker >/dev/null || { echo 'Docker CLI is required.' >&2; exit 1; }
  docker info >/dev/null 2>&1 || { echo 'No accessible Docker daemon. Start it yourself before invoking this runner.' >&2; exit 1; }
  exec docker build --progress=plain --file "$repo_dir/Dockerfile.ros2" \
    --build-arg "ROS_DISTRO=$distro" --tag "eurobot-next:$distro" "$repo_dir"
fi
if [[ "$(uname -s)" != Linux ]]; then
  echo 'Native ROS builds require the supported Ubuntu Linux environment; use --docker for source checks on macOS.' >&2
  exit 1
fi
setup_file="/opt/ros/$distro/setup.bash"
[[ -f "$setup_file" ]] || { echo "Missing ROS installation: $setup_file" >&2; exit 1; }
# ROS environment setup files may reference unset variables.
set +u
source "$setup_file"
set -u
command -v colcon >/dev/null || { echo 'colcon is required.' >&2; exit 1; }
cd -- "$repo_dir/ros2_ws"
colcon --log-base "log/$distro" build --build-base "build/$distro" \
  --install-base "install/$distro" --cmake-args -DBUILD_TESTING=ON
set +u
source "install/$distro/setup.bash"
set -u
colcon --log-base "log/$distro" test --build-base "build/$distro" \
  --install-base "install/$distro" --return-code-on-test-failure
colcon test-result --test-result-base "build/$distro" --verbose
