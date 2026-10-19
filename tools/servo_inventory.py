#!/usr/bin/env python3
"""Read-only Dynamixel inventory through Eurobot USART1. Never arms or writes servo registers."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct
import sys

import board_test as board

KNOWN_ROLES = {1: 'reserve_main1', 2: 'reserve_main2', 7: 'primary_side1',
               8: 'primary_side2', 13: 'pinion', 16: 'reserve_side1',
               25: 'reserve_side2', 98: 'cursor', 112: 'primary_main2'}


def collect(client, ids):
    status = board.parse_status(client.request(8))
    if status['armed'] or status['lease_active'] or status['owner']:
        raise board.BoardError('Inventory requires an idle, disarmed controller; no takeover')
    devices, missing = [], []
    for device_id in ids:
        request = struct.pack('<BBII3f', 36, device_id, 0, 0, 0, 0, 0)
        # A well-formed per-device NACK is a completed query, not a lost MCU ACK.
        # Request() latches any serial/protocol timeout; those abort this inventory.
        raw = client.request(0x24, request)
        try:
            result = board.parse_maintenance(36, device_id, 0, raw)
        except board.ControllerError as error:
            if error.error != board.ERRORS.index('timeout'):  # Only a device timeout is an absent ID; all other faults stop.
                raise
            missing.append(device_id)
        else:
            result['source_role'] = KNOWN_ROLES.get(device_id)
            devices.append(result)
    return {'read_only': True, 'devices': devices, 'no_response_ids': missing,
            'primary_main_id': None,
            'note': 'Unassigned devices are candidates, not proof of their mechanical role. ID254 is never queried.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--ids', nargs='+', type=int, default=list(range(1, 254)))
    parser.add_argument('--output', type=Path, default=Path('artifacts/servo-inventory.json'))
    args = parser.parse_args(argv)
    if not args.ids or len(set(args.ids)) != len(args.ids) or any(not 1 <= value <= 253 for value in args.ids):
        parser.error('Distinct individual IDs1..253 required; no broadcast')
    port = None
    try:
        port = board.SerialPort(args.port)
        report = collect(board.Client(port), args.ids)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return 0
    except (OSError, board.BoardError) as error:
        print('Inventory stopped: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if port is not None:
            port.close()


if __name__ == '__main__':
    raise SystemExit(main())
