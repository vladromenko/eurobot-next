#!/usr/bin/env python3
"""Build or flash the selected profile; first-install default is board diagnostics, independent of prior firmware."""
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
    parser.add_argument('--profile', choices=('diagnostic', 'diagnostic2025', 'robot2026', 'custom'), default='diagnostic')
    parser.add_argument('--config', type=Path, help='Explicit reviewed custom deployment; never inferred from a prior run')
    parser.add_argument('--additional-main-servo-id', '--primary-servo-id', dest='primary_servo_id', type=int,
                        help='2026 only: optional additional main AX ID; primary already uses112')
    parser.add_argument('--stlink-sn')
    parser.add_argument('--programmer')
    args = parser.parse_args(argv)
    if args.primary_servo_id is not None:
        if args.profile != 'robot2026':
            parser.error('Additional AX binding requires explicit --profile robot2026')
        if not 1 <= args.primary_servo_id <= 253 or args.primary_servo_id in KNOWN_IDS:
            parser.error('Additional main ID must be1..253 and distinct from existing2026 roles; broadcast254 forbidden')
    if (args.profile == 'custom') != (args.config is not None):
        parser.error('--profile custom requires --config; other profiles do not accept it')
    if args.config is not None and not args.config.is_file():
        parser.error('Custom deployment header must exist')
    return args


def selected(args):
    if args.profile == 'diagnostic':
        return 'stm32-diagnostic', ROOT / 'examples/diagnostic.hpp'
    if args.profile == 'diagnostic2025':
        return 'stm32-robot2025', ROOT / 'examples/robot_2025.hpp'
    if args.profile == 'custom':
        return 'stm32-robot2025', args.config.resolve()
    local = ROOT / 'config/robot_2026_local.hpp'
    config = local if local.is_file() else ROOT / 'examples/robot_2026.hpp'
    if args.primary_servo_id is not None:
        config = local
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text('#pragma once\n#include "eurobot/robot_2026.hpp"\n'
                          'inline eurobot::board::Deployment make_deployment() {\n'
                          f'  return eurobot::board::robot_2026({args.primary_servo_id});\n'
                          '}\n')
    return 'stm32-robot2026', config


def commands(args):
    preset, config = selected(args)
    image = ROOT / 'build' / preset / 'firmware/eurobot_next.bin'
    result = [['cmake', '--preset', preset, '--fresh', '-DEUROBOT_BOARD_CONFIG=' + str(config)],
              ['cmake', '--build', '--preset', preset, '--parallel'],
              [sys.executable, str(ROOT / 'tools/flash.py'), 'verify', '--image', str(image)]]
    if args.action == 'flash':
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
    print('Selected profile: ' + args.profile)
    if args.action == 'build':
        print('Build/offline verification only. Default image is DIAGNOSTIC_MODE for this STM32F407 board.')
    else:
        print('Backup and independent readback completed. Target left halted; reset manually with actuator power isolated.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
