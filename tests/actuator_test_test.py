#!/usr/bin/env python3
"""Actuator exercise safety using fake clients and a fake clock; no hardware access."""
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
try:
    spec = importlib.util.spec_from_file_location('bounded_actuator_subject', TOOLS / 'actuator_test.py')
    tool = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = tool
    spec.loader.exec_module(tool)
finally:
    sys.path.pop(0)
board = tool.board


class Clock:
    def __init__(self):
        self.now = 0.0
    def monotonic(self):
        return self.now
    def sleep(self, duration):
        if duration <= 0:
            raise AssertionError('busy sleep')
        self.now += duration


class Client:
    def __init__(self, mode='operational', commissioned=True, valid=True, busy=False, failure=None):
        self.owner = 0
        self.calls = []
        self.deployment = dict(mode=mode, commissioned=commissioned, valid=valid)
        self.busy, self.failure = busy, failure
        self.armed, self.leased = False, False

    def maintenance_command(self, operation, channel, argument, payload):
        self.calls.append((0x24, payload, False))
        assert (operation, channel, argument) == (37, 0, 0)
        assert payload == struct.pack('<BBII3f', 37, 0, 0, 0, 0, 0, 0)
        return self.deployment

    def status_command(self, opcode, payload=b'', *, cleanup=False):
        self.calls.append((opcode, payload, cleanup))
        if self.failure is not None:
            self.failure(opcode, payload, cleanup)
        if opcode == 8:
            return dict(armed=self.busy, lease_active=self.busy, owner=99 if self.busy else 0)
        if opcode == 1:
            assert struct.unpack('<I', payload) == (2000,)
            self.leased = True
        elif opcode == 2:
            self.armed = True
        elif opcode in (6, 7):
            self.armed = False
            if opcode == 7:
                self.leased = False
        return dict(armed=self.armed, lease_active=self.leased, owner=self.owner)


class ActuatorTests(unittest.TestCase):
    def args(self, kind='dc_duty', value='0.1', rate=None, duration='1000', device='0'):
        values = ['--port', '/mock/no-device', '--allow-motion', '--kind', kind,
                  '--id', device, '--value', value, '--duration-ms', duration]
        if rate is not None:
            values += ['--rate', rate]
        return tool.arguments(values)

    def run_exercise(self, client, args):
        clock = Clock()
        with mock.patch.object(tool.time, 'monotonic', clock.monotonic), \
                mock.patch.object(tool.time, 'sleep', clock.sleep), \
                mock.patch.object(tool.secrets, 'randbits', return_value=123), \
                mock.patch.object(board, 'emit'):
            tool.exercise(client, args)
        return clock

    def test_explicit_authorization_and_all_required_values_before_port_open(self):
        base = ['--port', '/mock/no-device', '--kind', 'dc_duty', '--id', '0', '--value', '0.1', '--duration-ms', '100']
        for argv in (base, base + ['--allow-motion', '--value', 'nan'],
                     base + ['--allow-motion', '--value', 'inf'],
                     base + ['--allow-motion', '--value', '1.1'],
                     base + ['--allow-motion', '--id', '4'],
                     base + ['--allow-motion', '--duration-ms', '0']):
            with self.subTest(argv=argv), mock.patch.object(board, 'SerialPort') as port, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    tool.main(argv)
                port.assert_not_called()
        for omitted in ('--id', '--value', '--duration-ms'):
            argv = base + ['--allow-motion']
            index = argv.index(omitted)
            del argv[index:index + 2]
            with self.subTest(omitted=omitted), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    tool.arguments(argv)

        args = self.args()
        args.allow_motion = False
        client = Client()
        with self.assertRaises(board.BoardError):
            self.run_exercise(client, args)
        self.assertEqual(client.calls, [])

    def test_actuator_domains_reject_broadcast_fractional_steps_and_zero_home_rate(self):
        cases = [('ax_position', '1', None, '254'), ('pwm_servo', '90', None, '8'),
                 ('stepper_relative', '0.5', '10', '0'), ('stepper_relative', '16777217', '10', '0'),
                 ('stepper_home', '0', None, '0'), ('stepper_home', '2', '10', '0'),
                 ('stepper_profile_relative', '10', '10', '0')]
        for kind, value, rate, device in cases:
            with self.subTest(kind=kind, value=value, rate=rate), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.args(kind, value, rate, device=device)

    def test_diagnostic_unknown_uncommissioned_or_invalid_deployment_never_claims(self):
        for mode, commissioned, valid in (('diagnostic', True, True), ('unknown', True, True),
                                          ('operational', False, True), ('operational', True, False)):
            with self.subTest(mode=mode, commissioned=commissioned, valid=valid):
                client = Client(mode, commissioned, valid)
                with self.assertRaises(board.BoardError):
                    self.run_exercise(client, self.args())
                self.assertEqual([opcode for opcode, _, _ in client.calls], [8, 0x24])
                self.assertEqual(client.owner, 0)

    def test_busy_owner_rejected_without_probe_claim_or_foreign_estop(self):
        client = Client(busy=True)
        with self.assertRaises(board.BoardError):
            self.run_exercise(client, self.args())
        self.assertEqual([opcode for opcode, _, _ in client.calls], [8])

    def test_ax_torque_once_position_refresh_heartbeat_fresh_owner_and_final_stop(self):
        client = Client()
        clock = self.run_exercise(client, self.args('ax_position', '0.5', device='42'))
        commands = [struct.unpack('<BBff', payload) for opcode, payload, _ in client.calls if opcode == 0x20]
        self.assertEqual(commands[0], (7, 42, 1, 0))
        self.assertEqual(sum(command[0] == 7 for command in commands), 1)
        self.assertGreater(sum(command[0] == 1 for command in commands), 1)
        self.assertTrue(all(command[1] == 42 for command in commands))
        self.assertEqual(client.owner, 123)
        self.assertIn(3, [opcode for opcode, _, _ in client.calls])
        self.assertEqual(client.calls[-2:], [(6, b'', True), (7, b'', True)])
        self.assertFalse(client.armed or client.leased)
        self.assertAlmostEqual(clock.now, 1.0)

    def test_home_and_relative_are_sent_once_only_with_heartbeat_during_window(self):
        for kind, value in (('stepper_home', '0'), ('stepper_relative', '-10')):
            with self.subTest(kind=kind):
                client = Client()
                self.run_exercise(client, self.args(kind, value, '20'))
                commands = [payload for opcode, payload, _ in client.calls if opcode == 0x20]
                self.assertEqual(len(commands), 1)
                self.assertEqual(struct.unpack('<BBff', commands[0]), (tool.KINDS[kind], 0, float(value), 20))
                self.assertIn(3, [opcode for opcode, _, _ in client.calls])
                self.assertEqual(client.calls[-2:], [(6, b'', True), (7, b'', True)])

    def test_profiled_steps_use_configured_rate_without_duplicate_impulses(self):
        client = Client()
        self.run_exercise(client, self.args('stepper_profile_relative', '10'))
        self.assertEqual(len([opcode for opcode, _, _ in client.calls if opcode == 0x20]), 1)

    def test_nack_and_serial_loss_abort_without_retry_and_still_attempt_cleanup(self):
        for error in (board.ControllerError(5), board.BoardError('mock serial timeout')):
            def fail(opcode, payload, cleanup):
                if opcode == 0x20:
                    raise error
            client = Client(failure=fail)
            with self.subTest(error=str(error)), self.assertRaises(board.BoardError):
                self.run_exercise(client, self.args())
            self.assertEqual(len([opcode for opcode, _, _ in client.calls if opcode == 0x20]), 1)
            self.assertEqual(client.calls[-2:], [(6, b'', True), (7, b'', True)])

    def test_arm_rejection_stops_owned_lease_without_actuator_command(self):
        def fail(opcode, payload, cleanup):
            if opcode == 2:
                raise board.ControllerError(3)
        client = Client(failure=fail)
        with self.assertRaises(board.ControllerError):
            self.run_exercise(client, self.args())
        self.assertNotIn(0x20, [opcode for opcode, _, _ in client.calls])
        self.assertEqual(client.calls[-2:], [(6, b'', True), (7, b'', True)])
        # Another owner may claim after the initial idle read: never ESTOP them.
        def conflict(opcode, payload, cleanup):
            if opcode == 1:
                raise board.ControllerError(5)
        client = Client(failure=conflict)
        with self.assertRaises(board.ControllerError):
            self.run_exercise(client, self.args())
        self.assertEqual([opcode for opcode, _, _ in client.calls], [8, 0x24, 1])

    def test_failed_cleanup_is_reported_and_main_closes_port(self):
        def fail(opcode, payload, cleanup):
            if opcode == 6:
                raise board.BoardError('mock stop not acknowledged')
        client = Client(failure=fail)
        with self.assertRaisesRegex(board.BoardError, 'Cleanup unconfirmed'):
            self.run_exercise(client, self.args(duration='20'))
        port = mock.Mock()
        with mock.patch.object(board, 'SerialPort', return_value=port), \
                mock.patch.object(board, 'Client', return_value=client), \
                mock.patch.object(tool, 'exercise', side_effect=KeyboardInterrupt), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tool.main(['--port', '/mock', '--allow-motion', '--kind', 'dc_duty', '--id', '0', '--value', '0', '--duration-ms', '20']), 130)
        port.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
