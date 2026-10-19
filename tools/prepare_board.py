#!/usr/bin/env python3
"""Explicit ST-LINK enumeration, read-only target inspection and 2025 Flash backup. Never flashes."""
import argparse
from pathlib import Path
import sys
import tempfile
import flash


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('list', 'inspect', 'backup'))
    parser.add_argument('--allow-connect', action='store_true')
    parser.add_argument('--programmer')
    parser.add_argument('--stlink-sn')
    parser.add_argument('--backup-dir', type=Path, default=flash.ROOT / 'backups')
    args = parser.parse_args(argv)
    if args.action != 'list' and not args.allow_connect:
        parser.error('inspect/backup require explicit --allow-connect and isolated actuator power')
    programmer = flash.Programmer(flash.locate_programmer(args.programmer), 60)
    if args.action == 'list':
        print(programmer.run(['-l', 'st-link-only']))
        return 0
    programmer.select(args.stlink_sn)
    with flash.probe_lock(programmer.serial), tempfile.TemporaryDirectory(prefix='eurobot-inspect-') as directory:
        programmer.inspect(1048576, Path(directory))
        # Display-only option-byte access, verified against installed CubeProgrammer -h.
        # No assignments, unprotect, erase, run/reset, or recovery retry are permitted.
        print(programmer.run([*programmer.connection(), '-ob', 'displ']))
        print('STM32F405/F407 family0x413 and1MiB Flash verified. Check MCU package marking separately.')
        if args.action == 'backup':
            flash.backup(programmer, 1048576, args.backup_dir.expanduser().resolve())
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (flash.Failure, OSError, ValueError) as error:
        print('Inspection stopped: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
