#!/usr/bin/env python3
"""Installation and inventory mocks only: no USB, serial device or real subprocess."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'tools'


def load_tool(name, filename):
    spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sys.path.insert(0, str(TOOLS))
try:
    installer = load_tool('installation_subject', 'install_robot.py')
    inventory = load_tool('inventory_subject', 'servo_inventory.py')
finally:
    sys.path.pop(0)
board = inventory.board


def status(flags=0, owner=0):
    result = bytearray(84)
    result[1] = flags
    result[2] = 8
    struct.pack_into('<I', result, 4, owner)
    return bytes(result)


def probe(device_id, model=12):
    return b'\x00' + struct.pack('<HBBHHHBB', model, 42, device_id, 0, 1023, 511, 0, 2)


class FakeClient:
    """Rejects every wire opcode except read-only status and typed servo probe."""
    def __init__(self, responses=None, initial=None):
        self.responses = {} if responses is None else responses
        self.initial = status() if initial is None else initial
        self.calls = []

    def request(self, opcode, payload=b''):
        self.calls.append((opcode, payload))
        if opcode == 8:
            if payload:
                raise AssertionError('status query must have no payload')
            return self.initial
        if opcode != 0x24 or len(payload) != 22:
            raise AssertionError('inventory attempted a mutation or unknown opcode')
        operation, device_id, argument, secondary, x, y, z = struct.unpack('<BBII3f', payload)
        if operation != 36 or not 1 <= device_id <= 253 or (argument, secondary, x, y, z) != (0, 0, 0, 0, 0):
            raise AssertionError('inventory attempted a non-probe maintenance request')
        response = self.responses[device_id]
        if isinstance(response, BaseException):
            raise response
        return response


class InstallationTests(unittest.TestCase):
    def test_default_build_only_configures_builds_and_verifies_offline(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(installer, 'ROOT', Path(directory)), \
                mock.patch.object(installer.subprocess, 'run') as run, redirect_stdout(io.StringIO()):
            self.assertEqual(installer.main([]), 0)
            commands = [call.args[0] for call in run.call_args_list]
            self.assertEqual(len(commands), 3)
            self.assertEqual(commands[0][:4], ['cmake', '--preset', 'stm32-robot2026', '--fresh'])
            self.assertEqual(commands[1][:4], ['cmake', '--build', '--preset', 'stm32-robot2026'])
            self.assertEqual(commands[2][2], 'verify')
            self.assertTrue(commands[2][1].endswith('/tools/flash.py'))
            self.assertTrue(commands[2][-1].endswith('/build/stm32-robot2026/firmware/eurobot_next.bin'))
            self.assertNotIn('--allow-flash', [item for command in commands for item in command])
            self.assertTrue(all(call.kwargs == {'cwd': Path(directory), 'check': True} for call in run.call_args_list))
            self.assertFalse((Path(directory) / 'config').exists())

    def test_explicit_flash_delegates_backup_and_readback_to_existing_safety_tool(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(installer, 'ROOT', Path(directory)), \
                mock.patch.object(installer.subprocess, 'run') as run, redirect_stdout(io.StringIO()):
            self.assertEqual(installer.main(['flash', '--stlink-sn', 'TEST_SERIAL', '--programmer', '/mock/programmer']), 0)
            commands = [call.args[0] for call in run.call_args_list]
            self.assertEqual(len(commands), 4)
            self.assertEqual(commands[2][2], 'verify')
            flash = commands[3]
            self.assertEqual(flash[2], 'flash')
            self.assertEqual(flash[1], str(Path(directory) / 'tools/flash.py'))
            self.assertIn('--allow-flash', flash)
            self.assertIn('--confirm-no-custom-bootloader', flash)
            self.assertEqual(flash[flash.index('--stlink-sn') + 1], 'TEST_SERIAL')
            self.assertEqual(flash[flash.index('--programmer') + 1], '/mock/programmer')
            self.assertFalse(any('erase' in argument or 'option' in argument or 'rdp' in argument.lower() for argument in flash))

    def test_failed_configure_stops_before_build_verify_or_flash(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(installer, 'ROOT', Path(directory)), \
                mock.patch.object(installer.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['cmake'])) as run, \
                redirect_stderr(io.StringIO()):
            self.assertEqual(installer.main(['flash']), 1)
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][0], 'cmake')

    def test_saved_local_identity_is_reused_on_later_installation(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(installer, 'ROOT', Path(directory)):
            chosen = installer.commands(installer.arguments(['--primary-servo-id', '42']))
            saved = Path(directory) / 'config/robot_2026_local.hpp'
            self.assertIn('robot_2026(42)', saved.read_text())
            later = installer.commands(installer.arguments([]))
            self.assertEqual(chosen[0], later[0])
            self.assertIn('-DEUROBOT_BOARD_CONFIG=' + str(saved), later[0])

    def test_additional_motor_option_alias_preserves_existing_cli(self):
        self.assertEqual(installer.arguments(['--additional-main-servo-id', '42']).primary_servo_id, 42)
        self.assertEqual(installer.arguments(['--primary-servo-id', '42']).primary_servo_id, 42)

    def test_invalid_and_already_assigned_primary_ids_rejected(self):
        for value in (0, 254, 255, -1, *sorted(installer.KNOWN_IDS)):
            with self.subTest(primary=value), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    installer.arguments(['--primary-servo-id', str(value)])
                self.assertEqual(stopped.exception.code, 2)

    def test_individually_chosen_primary_generates_only_local_config_under_mock_root(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(installer, 'ROOT', Path(directory)), \
                mock.patch.object(installer.subprocess, 'run') as run, redirect_stdout(io.StringIO()):
            self.assertEqual(installer.main(['--primary-servo-id', '42']), 0)
            local = Path(directory) / 'config/robot_2026_local.hpp'
            self.assertEqual(local.read_text(), '#pragma once\n#include "eurobot/robot_2026.hpp"\n'
                             'inline eurobot::board::Deployment make_deployment() {\n'
                             '  return eurobot::board::robot_2026(42);\n}\n')
            self.assertIn('-DEUROBOT_BOARD_CONFIG=' + str(local), run.call_args_list[0].args[0])
            self.assertFalse((Path(directory) / 'examples/robot_2026.hpp').exists())
            self.assertEqual(run.call_count, 3)


class InventoryTests(unittest.TestCase):
    def test_readonly_success_and_device_timeout_continue_without_role_guessing(self):
        client = FakeClient({7: probe(7), 42: b'\x09', 43: probe(43, 29)})
        report = inventory.collect(client, [7, 42, 43])
        self.assertTrue(report['read_only'])
        self.assertEqual(report['no_response_ids'], [42])
        self.assertEqual([device['actual_id'] for device in report['devices']], [7, 43])
        self.assertEqual(report['devices'][0]['source_role'], 'primary_side1')
        self.assertIsNone(report['devices'][1]['source_role'])
        self.assertEqual(report['devices'][1]['model_number'], 29)
        self.assertEqual(report['source_primary_main_id'], 112)
        self.assertFalse(report['primary_main_responded'])
        self.assertEqual([opcode for opcode, _ in client.calls], [8, 0x24, 0x24, 0x24])

    def test_primary_response_is_evidence_separate_from_source_binding(self):
        report = inventory.collect(FakeClient({112: probe(112)}), [112])
        self.assertEqual(report['source_primary_main_id'], 112)
        self.assertTrue(report['primary_main_responded'])
        self.assertEqual(report['devices'][0]['source_role'], 'primary_main')

    def test_armed_leased_or_old_owned_controller_rejected_before_probe(self):
        for flags, owner in ((1, 0), (4, 0), (0, 77), (5, 77)):
            with self.subTest(flags=flags, owner=owner):
                client = FakeClient(initial=status(flags, owner))
                with self.assertRaises(board.BoardError):
                    inventory.collect(client, [42])
                self.assertEqual(client.calls, [(8, b'')])

    def test_other_controller_errors_abort_without_scanning_next_id(self):
        for error in (1, 5, 7, 8, 10, 12):
            with self.subTest(error=error):
                client = FakeClient({42: bytes((error,)), 43: probe(43)})
                with self.assertRaises(board.ControllerError) as stopped:
                    inventory.collect(client, [42, 43])
                self.assertEqual(stopped.exception.error, error)
                self.assertEqual(len(client.calls), 2)

    def test_unknown_error_code_and_extra_timeout_nack_body_are_protocol_failures(self):
        for raw in (b'\x0d', b'\xff', b'\x09\x00'):
            with self.subTest(raw=raw):
                client = FakeClient({42: raw, 43: probe(43)})
                with self.assertRaises(board.ProtocolError):
                    inventory.collect(client, [42, 43])
                self.assertEqual(len(client.calls), 2)

    def test_truncated_or_wrong_identity_probe_success_never_becomes_inventory(self):
        for raw in (b'', b'\x00', probe(42)[:-1], probe(43), probe(42) + b'\x00'):
            with self.subTest(raw=raw):
                client = FakeClient({42: raw, 43: probe(43)})
                with self.assertRaises(board.ProtocolError):
                    inventory.collect(client, [42, 43])
                self.assertEqual(len(client.calls), 2)

    def test_actual_client_serial_timeout_aborts_and_latches_failed_session(self):
        class TimeoutPort:
            def __init__(self):
                self.writes = []
                self.status_reply = None

            def write_all(self, raw, deadline):
                frame, = board.Parser().feed(raw)
                self.writes.append(frame)
                if frame.opcode == 8:
                    self.status_reply = board.encode_frame(board.Frame(0x88, frame.sequence, frame.owner, status()))

            def read(self, deadline):
                if self.status_reply is not None:
                    reply, self.status_reply = self.status_reply, None
                    return reply
                raise board.BoardError('mock serial ACK timeout')

        port = TimeoutPort()
        client = board.Client(port, timeout=0.02)
        with self.assertRaisesRegex(board.BoardError, 'mock serial ACK timeout'):
            inventory.collect(client, [42, 43])
        self.assertTrue(client.failed)
        self.assertEqual([frame.opcode for frame in port.writes], [8, 0x24])
        self.assertEqual(port.writes[-1].payload[0:2], bytes((36, 42)))

    def test_cli_rejects_broadcast_and_duplicate_ids_before_opening_hardware(self):
        for ids in (['0'], ['254'], ['255'], ['42', '42']):
            with self.subTest(ids=ids), mock.patch.object(board, 'SerialPort') as serial, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    inventory.main(['--port', '/must/not/open', '--ids', *ids])
                self.assertEqual(stopped.exception.code, 2)
                serial.assert_not_called()


if __name__ == '__main__':
    unittest.main()
