#!/usr/bin/env python3
"""Build the source-configured2026 image; explicitly flash with backup/readback."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KNOWN_IDS = frozenset((1, 2, 7, 8, 13, 16, 25, 98, 112))


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', nargs='?', choices=('build', 'flash'), default='build')
    parser.add_argument('--additional-main-servo-id', '--primary-servo-id', dest='primary_servo_id', type=int,
                        help='Optional additional main AX ID; default primary already works on112 alone')
    parser.add_argument('--stlink-sn')
    parser.add_argument('--programmer')
    args = parser.parse_args(argv)
    if args.primary_servo_id is not None and (
            not 1 <= args.primary_servo_id <= 253 or args.primary_servo_id in KNOWN_IDS):
        parser.error('Additional main ID must be1..253 and distinct from existing2026 roles; broadcast254 forbidden')
    return args


def commands(args):
    local = ROOT / 'config/robot_2026_local.hpp'
    config = local if local.is_file() else ROOT / 'examples/robot_2026.hpp'
    if args.primary_servo_id is not None:
        config = ROOT / 'config/robot_2026_local.hpp'
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text('#pragma once\n#include "eurobot/robot_2026.hpp"\n'
                          'inline eurobot::board::Deployment make_deployment() {\n'
                          f'  return eurobot::board::robot_2026({args.primary_servo_id});\n'
                          '}\n')
    image = ROOT / 'build/stm32-robot2026/firmware/eurobot_next.bin'
    result = [['cmake', '--preset', 'stm32-robot2026', '--fresh',
               '-DEUROBOT_BOARD_CONFIG=' + str(config)],
              ['cmake', '--build', '--preset', 'stm32-robot2026', '--parallel'],
              [sys.executable, str(ROOT / 'tools/flash.py'), 'verify', '--image', str(image)]]
    if args.action == 'flash':
        # This project's2026 standalone linker starts at0x08000000. The existing
        # flash tool validates MCU/Flash size and backs up ALL Flash before write.
        flash = [sys.executable, str(ROOT / 'tools/flash.py'), 'flash', '--image', str(image),
                 '--allow-flash', '--confirm-no-custom-bootloader']
        for flag, value in (('--stlink-sn', args.stlink_sn), ('--programmer', args.programmer)):
            if value:
                flash.extend((flag, value))
        result.append(flash)
    return result


def main(argv=None):
    args = arguments(argv)
    try:
        for command in commands(args):
            subprocess.run(command, cwd=ROOT, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'Installation stopped: {error}; no automatic retry.', file=sys.stderr)
        return 1
    print('Image: ' + str(ROOT / 'build/stm32-robot2026/firmware/eurobot_next.bin'))
    if args.action == 'build':
        print('Build/offline verification only. To write a connected board: python3 tools/install_robot.py flash')
    else:
        print('Backup and independent readback completed. Target was left halted; no automatic run.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
