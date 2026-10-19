#!/usr/bin/env python3
"""One explicitly authorized, bounded actuator exercise; never flashes or reconnects."""
from __future__ import annotations
import argparse
import math
import secrets
import struct
import sys
import time

import board_test as board

KINDS = {'ax_position': 1, 'mx_position': 2, 'pca_servo': 3, 'pwm_servo': 4,
         'stepper_relative': 5, 'dc_duty': 6, 'stepper_home': 11,
         'stepper_profile_relative': 12}
IMPULSES = frozenset((5, 11, 12))


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--allow-motion', action='store_true')
    parser.add_argument('--kind', required=True, choices=tuple(KINDS))
    parser.add_argument('--id', required=True, type=int)
    parser.add_argument('--value', required=True, type=float,
                        help='Measured allowed target: radians AX/MX, degrees PWM/PCA, signed steps, direction0/1 home, duty DC')
    parser.add_argument('--rate', type=float, default=0,
                        help='Positive integer steps/s for relative/home; otherwise0. Profile-relative uses configured speed.')
    parser.add_argument('--duration-ms', required=True, type=int,
                        help='Explicit energization window1..10000ms; ESTOP/release at its end, not completion proof')
    parser.add_argument('--timeout', type=float, default=0.06, help='ACK timeout0.02..0.1s')
    args = parser.parse_args(argv)
    kind = KINDS[args.kind]
    if not args.allow_motion:
        parser.error('This tool requires explicit--allow-motion; use board_test.py for read-only diagnostics')
    if not 1 <= args.duration_ms <= 10000 or not 0.02 <= args.timeout <= 0.1:
        parser.error('Duration1..10000ms and ACK timeout0.02..0.1s required')
    if not all(math.isfinite(value) and abs(value) <= 3.4028234663852886e38 for value in (args.value, args.rate)):
        parser.error('Value/rate must be finite float32')
    maximum = 253 if kind in (1, 2) else 15 if kind == 3 else 7 if kind == 4 else 3
    minimum = 1 if kind in (1, 2) else 0
    if not minimum <= args.id <= maximum:
        parser.error('Individual device ID/channel is outside the supported domain; no broadcast')
    if kind in (1, 2):
        if not 0 <= args.value <= math.radians(300 if kind == 1 else 360):
            parser.error('AX/MX angle is radians within model domain; real profile can be narrower')
    elif kind in (3, 4):
        if not 0 <= args.value <= 360:
            parser.error('Servo value is degrees0..360; actual calibrated limits come from profile')
    elif kind in (5, 12):
        if not -2147483648 <= args.value < 2147483648 or args.value != math.trunc(args.value):
            parser.error('Relative steps must be an int32')
        if struct.unpack('<f', struct.pack('<f', args.value))[0] != args.value:
            parser.error('Relative steps must be exactly representable as float32; use smaller requests')
    elif kind == 6 and not -1 <= args.value <= 1:
        parser.error('DC duty must be-1..1')
    elif kind == 11 and args.value not in (0, 1):
        parser.error('Home direction is0(backward)/1(forward), as physically verified in the profile')
    if kind in (5, 11):
        if not 0 < args.rate < 2147483648 or args.rate != math.trunc(args.rate):
            parser.error('Relative/home rate requires positive integer steps/s; profile limits still apply')
    elif args.rate != 0:
        parser.error('Only relative/home accepts a nonzero rate')
    return args


def verify_owned_armed(status, owner):
    if not status['armed'] or not status['lease_active'] or status['owner'] != owner:
        raise board.BoardError('Owned armed session lost; no further actuator command')


def exercise(client, args):
    if not args.allow_motion:
        raise board.BoardError('Explicit--allow-motion is required before any exercise')
    initial = client.status_command(8)
    board.require_idle(initial)
    deployment = client.maintenance_command(37, 0, 0, struct.pack('<BBII3f', 37, 0, 0, 0, 0, 0, 0))
    board.emit('deployment', deployment)
    if deployment.get('mode') != 'operational' or deployment.get('commissioned') is not True or deployment.get('valid') is not True:
        raise board.BoardError('Requires a verified commissioned valid OPERATIONAL_MODE image; diagnostic cannot actuate')
    kind = KINDS[args.kind]
    claimed = False
    cleanup_errors = []
    client.owner = secrets.randbits(32) or 1
    try:
        status = client.status_command(1, struct.pack('<I', 2000))
        if status['owner'] != client.owner or not status['lease_active']:
            raise board.ProtocolError('Claim did not confirm this owner/lease')
        claimed = True
        if status['armed']:
            raise board.ProtocolError('Claim must start disarmed')
        verify_owned_armed(client.status_command(2), client.owner)
        deadline = time.monotonic() + args.duration_ms / 1000
        heartbeat_at = time.monotonic()
        payload = struct.pack('<BBff', kind, args.id, args.value, args.rate)

        def actuate(data):
            if time.monotonic() >= deadline:
                raise board.BoardError('Exercise window elapsed before actuator transaction')
            sent_at = time.monotonic()
            verify_owned_armed(client.status_command(0x20, data), client.owner)
            return sent_at

        if kind in (1, 2):
            # Explicit test authorization includes torque for this one identified servo.
            actuate(struct.pack('<BBff', 7 if kind == 1 else 8, args.id, 1, 0))
        sent_at = actuate(payload)
        board.emit('actuator', {'kind': args.kind, 'id': args.id, 'command_accepted': True,
                                'motion_completion_verified': False})
        next_refresh = sent_at + 0.1
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now - heartbeat_at >= 0.4:
                verify_owned_armed(client.status_command(3), client.owner)
                heartbeat_at = time.monotonic()
            if kind not in IMPULSES and time.monotonic() >= next_refresh and time.monotonic() < deadline:
                sent_at = actuate(payload)  # Fresh sequence, not retry after any error.
                next_refresh = sent_at + 0.1
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.02, remaining))
    finally:
        if claimed:
            for opcode, name in ((6, 'ESTOP'), (7, 'release')):
                try:
                    client.status_command(opcode, cleanup=True)
                except (OSError, board.BoardError) as error:
                    cleanup_errors.append(f'{name}: {error}')
            board.emit('cleanup', {'errors': cleanup_errors, 'estop_latched_expected': True,
                                   'physical_stop_verified': False})
            if cleanup_errors and sys.exc_info()[0] is None:
                raise board.BoardError('Cleanup unconfirmed; use independent hardware energy isolation')


def main(argv=None):
    args = arguments(argv)
    port = None
    try:
        port = board.SerialPort(args.port)
        exercise(board.Client(port, args.timeout), args)
        return 0
    except (OSError, board.BoardError, KeyboardInterrupt) as error:
        print(f'FAILED: {error}; no automatic retry/reconnect/flash', file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if port is not None:
            port.close()


if __name__ == '__main__':
    raise SystemExit(main())
