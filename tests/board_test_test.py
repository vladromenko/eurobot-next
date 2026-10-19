#!/usr/bin/env python3
"""Local POSIX PTY mocks only; never enumerates or opens a real controller."""
import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import pty
import select
import struct
import sys
import threading
import time
import unittest

SPEC = importlib.util.spec_from_file_location("eurobot_board_test", Path(__file__).resolve().parents[1] / "tools/board_test.py")
board = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = board
SPEC.loader.exec_module(board)


def status(error=0, owner=0, flags=0):
    data = bytearray(84)
    data[0:4] = bytes((error, flags, 8, 0))
    struct.pack_into("<I", data, 4, owner)
    return bytes(data)


def sensors():
    data = bytearray(25)
    struct.pack_into("<HH", data, 20, 0x0401, 0x1fff)
    data[24] = 10
    return bytes(data)


def job(owner=77, job_id=1, phase=1, operation=0, gripper=0, error=0):
    data = bytearray(25)
    data[:7] = bytes((error, phase, operation, gripper, 0 if phase == 1 else 1, 1, 1))
    data[7] = 1 if phase == 2 else 0
    struct.pack_into("<III", data, 13, job_id, owner, 20)
    return bytes(data)


class Peer:
    def __init__(self, handler):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.handler, self.received, self.errors = handler, [], []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        parser = board.Parser()
        try:
            while not self.stop.is_set():
                if select.select([self.master], [], [], 0.02)[0]:
                    raw = os.read(self.master, 1024)
                    for frame in parser.feed(raw):
                        self.received.append(frame)
                        reply = self.handler(frame)
                        if reply is not None:
                            packet = reply if isinstance(reply, bytes) else board.encode_frame(reply)
                            for start in range(0, len(packet), 7):
                                os.write(self.master, packet[start:start+7])
        except OSError:
            if not self.stop.is_set():
                self.errors.append("PTY closed")
        except BaseException as error:
            self.errors.append(str(error))

    def close(self):
        self.stop.set()
        self.thread.join(1)
        os.close(self.master)
        os.close(self.slave)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def ack(frame, payload):
    return board.Frame(frame.opcode | 0x80, frame.sequence, frame.owner, payload)


class WireTests(unittest.TestCase):
    def test_crc_reference_and_known_packet(self):
        self.assertEqual(board.crc16(b"123456789"), 0x29b1)
        wire = board.encode_frame(board.Frame(8, 0x12345678, 99))
        self.assertEqual(wire[:14], bytes.fromhex("a55a010878563412630000000000"))
        self.assertEqual(board.Parser().feed(wire), [board.Frame(8, 0x12345678, 99)])

    def test_fragmentation_noise_crc_and_oversize(self):
        packet = board.encode_frame(board.Frame(0x88, 1, 0, status()))
        broken = bytearray(packet); broken[-1] ^= 1
        excessive = bytes.fromhex("a55a01880000000000000000ff7f")
        parser, frames = board.Parser(), []
        for byte in b"junk" + excessive + bytes(broken) + packet:
            frames += parser.feed(bytes((byte,)))
        self.assertEqual(frames, [board.Frame(0x88, 1, 0, status())])
        self.assertGreaterEqual(parser.rejected, 2)
        parser.feed(b"noise" * 10000)
        self.assertLessEqual(len(parser.buffer), 144)

    def test_strict_lengths_and_validity(self):
        for count in (0, 1, 83, 85):
            with self.assertRaises(board.ProtocolError):
                board.parse_status(bytes(count))
        self.assertFalse(board.parse_status(status())["pose_known"])
        bad = bytearray(status()); bad[2] = 0
        with self.assertRaises(board.ProtocolError): board.parse_status(bad)
        bad = bytearray(status()); struct.pack_into("<I", bad, 8, 0x100)
        with self.assertRaises(board.ProtocolError): board.parse_status(bad)
        bad = bytearray(status()); bad[76] = 2; struct.pack_into("<f", bad, 64, float("nan"))
        with self.assertRaises(board.ProtocolError): board.parse_status(bad)

    def test_sensor_offsets_unknowns_and_malformed(self):
        data = bytearray(sensors()); data[1] = 1; data[24] = 0
        struct.pack_into("<I4HHH", data, 2, 12000, 4000, 3990, 3980, 3970, 15940, 750)
        data[18:20] = bytes((75, 4))
        value = board.parse_sensors(data)
        self.assertEqual(value["raw_current_ma"], 750)
        self.assertEqual(value["voltage_mv"], 15940)
        self.assertEqual(value["cell_voltage_mv"], (4000, 3990, 3980, 3970))
        self.assertEqual(value["current_sign"], "unknown")
        self.assertEqual(value["gpio_raw_levels"], 0x401)
        self.assertFalse(board.parse_sensors(b"\x0a")["battery_known"])
        for malformed in (b"\x00", bytes(24), bytes(26)):
            with self.assertRaises(board.ProtocolError): board.parse_sensors(malformed)
        data[18] = 101
        with self.assertRaises(board.ProtocolError): board.parse_sensors(data)

    def test_job_offsets_and_error_payload(self):
        value = board.parse_job(job())
        self.assertEqual((value["job_id"], value["owner"], value["elapsed_ms"]), (1, 77, 20))
        with self.assertRaises(board.ControllerError): board.parse_job(b"\x0a")
        for malformed in (bytes(24), b"\x00", bytes(26)):
            with self.assertRaises(board.ProtocolError): board.parse_job(malformed)
        malformed = bytearray(job()); malformed[6] = 5
        with self.assertRaises(board.ProtocolError): board.parse_job(malformed)

    def test_empty_completed_and_inconsistent_evidence_rejected(self):
        cases = []
        empty = bytearray(job(phase=2)); empty[4] = empty[5] = empty[7] = 0
        cases.append(empty)
        missing = bytearray(job(phase=2)); missing[7] = 0
        cases.append(missing)
        excess = bytearray(job()); excess[7] = 1
        cases.append(excess)
        running_finished = bytearray(job()); running_finished[4] = 1
        cases.append(running_finished)
        for field in (13, 17):
            no_identity = bytearray(job()); struct.pack_into("<I", no_identity, field, 0)
            cases.append(no_identity)
        for malformed in cases:
            with self.subTest(payload=malformed.hex()), self.assertRaises(board.ProtocolError):
                board.parse_job(malformed)

    def test_deployment_info_is_strict_read_only_and_distinguishes_modes(self):
        value = board.parse_maintenance(37, 0, 0, bytes((0, 1, 0, 1, 0, 0, 4, 0)))
        self.assertEqual(value['mode'], 'diagnostic')
        self.assertEqual(value['profile'], 'ROBOT_2025_HARDWARE_PROFILE')
        self.assertFalse(value['commissioned'])
        self.assertIn(37, board.READ_ONLY_MAINTENANCE)
        for bad in (bytes((0, 2, 0, 1, 0, 0, 4, 0)), bytes((0, 1, 2, 1, 0, 0, 4, 0)), bytes((0, 1, 0, 1, 0, 0, 4, 5)), b'\x00', bytes(9)):
            with self.assertRaises(board.ProtocolError): board.parse_maintenance(37, 0, 0, bad)

    def test_cli_gates_before_port_open(self):
        for flags in (("--exercise", "gripper"), ("--allow-motion",), ("--value", "nan"),
                      ("--exercise", "output", "--allow-motion"), ("--values", "2", "--select", "1")):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                board.arguments(["--port", "/never/open", *flags])


class PtyTests(unittest.TestCase):
    def test_default_main_only_status_sensors(self):
        def handle(frame):
            return ack(frame, status() if frame.opcode == 8 else sensors())
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(board.main(["--port", peer.path, "--timeout", "0.2"]), 0)
            self.assertEqual([f.opcode for f in peer.received], [8, 9])
            self.assertTrue(all(f.owner == 0 for f in peer.received))
            self.assertIn('"gpio_known_mask": 8191', output.getvalue())
            self.assertEqual(peer.errors, [])

    def test_wrong_identity_and_corrupt_ack_ignored(self):
        def handle(frame):
            wrong_seq = board.encode_frame(board.Frame(0x88, frame.sequence + 1, frame.owner, status()))
            wrong_owner = board.encode_frame(board.Frame(0x88, frame.sequence, frame.owner + 1, status()))
            wrong_op = board.encode_frame(board.Frame(0x89, frame.sequence, frame.owner, status()))
            broken = bytearray(board.encode_frame(ack(frame, status()))); broken[-1] ^= 2
            return wrong_seq + wrong_owner + wrong_op + bytes(broken) + board.encode_frame(ack(frame, status()))
        with Peer(handle) as peer:
            port = board.SerialPort(peer.path)
            try:
                client = board.Client(port, 0.3)
                self.assertEqual(client.status_command(8)["error"], "ok")
                self.assertEqual(client.parser.rejected, 1)
            finally: port.close()

    def test_timeout_no_replay(self):
        with Peer(lambda frame: None) as peer:
            port = board.SerialPort(peer.path)
            try:
                client = board.Client(port, 0.05)
                started = time.monotonic()
                with self.assertRaises(board.BoardError): client.status_command(8)
                with self.assertRaises(board.BoardError): client.status_command(8)
                self.assertLess(time.monotonic() - started, 0.3)
                self.assertEqual(len(peer.received), 1)
            finally: port.close()

    def test_exclusive_open(self):
        with Peer(lambda frame: None) as peer:
            first = board.SerialPort(peer.path)
            try:
                with self.assertRaises(OSError): board.SerialPort(peer.path)
            finally: first.close()

    def test_gripper_payload_heartbeat_and_cleanup(self):
        queries = 0
        def handle(frame):
            nonlocal queries
            if frame.opcode in (1, 2, 3): return ack(frame, status(owner=frame.owner, flags=4 if frame.opcode == 1 else 5))
            if frame.opcode == 0x21:
                fields = struct.unpack("<BBfIIff", frame.payload)
                self.assertEqual(fields, (0, 0, 1, 0, 1000, 0, 0))
                return ack(frame, job(owner=frame.owner))
            if frame.opcode == 0x22:
                queries += 1
                return ack(frame, job(owner=frame.owner, phase=2 if queries >= 10 else 1))
            return ack(frame, status(owner=frame.owner, flags=6 if frame.opcode == 6 else 2))
        args = board.arguments(["--port", "unused", "--exercise", "gripper", "--allow-motion", "--value", "1", "--job-timeout-ms", "1000"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
            port = board.SerialPort(peer.path)
            try: board.exercise(board.Client(port, 0.2), args)
            finally: port.close()
            operations = [f.opcode for f in peer.received]
            self.assertIn(3, operations)
            self.assertEqual(operations[-2:], [6, 7])
            self.assertNotIn(0x23, operations) # Completed jobs are not spuriously cancelled.
            self.assertEqual(len({f.owner for f in peer.received}), 1)
            self.assertNotEqual(peer.received[0].owner, 0)
            self.assertEqual([f.sequence for f in peer.received], sorted({f.sequence for f in peer.received}))

    def test_nack_does_not_retry_and_stops_owned_session(self):
        def handle(frame):
            if frame.opcode == 1: return ack(frame, status(owner=frame.owner, flags=4))
            if frame.opcode == 2: return ack(frame, status(owner=frame.owner, flags=5))
            if frame.opcode == 0x21: return ack(frame, b"\x01")
            return ack(frame, status(owner=frame.owner, flags=2))
        args = board.arguments(["--port", "unused", "--exercise", "gripper", "--allow-motion"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
            port = board.SerialPort(peer.path)
            try:
                with self.assertRaises(board.ControllerError): board.exercise(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertEqual([f.opcode for f in peer.received], [1, 2, 0x21, 6, 7])

    def test_bad_completed_ack_never_reports_success(self):
        def handle(frame):
            if frame.opcode == 1: return ack(frame, status(owner=frame.owner, flags=4))
            if frame.opcode == 2: return ack(frame, status(owner=frame.owner, flags=5))
            if frame.opcode == 0x21:
                malformed = bytearray(job(owner=frame.owner, phase=2))
                malformed[4] = malformed[5] = malformed[7] = 0
                return ack(frame, malformed)
            return ack(frame, status(owner=frame.owner, flags=2))
        args = board.arguments(["--port", "unused", "--exercise", "gripper", "--allow-motion"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()) as output:
            port = board.SerialPort(peer.path)
            try:
                with self.assertRaises(board.ProtocolError): board.exercise(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertNotIn('"phase": "completed"', output.getvalue())
            self.assertEqual([f.opcode for f in peer.received], [1, 2, 0x21, 6, 7])

    def test_claim_conflict_does_not_estop_other_owner(self):
        with Peer(lambda frame: ack(frame, status(error=5, owner=999, flags=5))) as peer:
            port = board.SerialPort(peer.path)
            args = board.arguments(["--port", "unused", "--exercise", "gripper", "--allow-motion"])
            try:
                with self.assertRaises(board.ControllerError): board.exercise(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertEqual([f.opcode for f in peer.received], [1])

    def test_output_refresh_and_stop_finally(self):
        with Peer(lambda frame: ack(frame, status(owner=frame.owner, flags=4 if frame.opcode == 1 else 5))) as peer, contextlib.redirect_stdout(io.StringIO()):
            port = board.SerialPort(peer.path)
            args = board.arguments(["--port", "unused", "--exercise", "output", "--allow-motion", "--select", "1", "--values", "1", "--duration-ms", "150"])
            try: board.exercise(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertGreaterEqual(sum(f.opcode == 4 for f in peer.received), 2)
            self.assertEqual([f.opcode for f in peer.received][-2:], [6, 7])


class MaintenanceTests(unittest.TestCase):
    @staticmethod
    def application_body(phase=0):
        body = bytearray(24)
        body[0:4] = bytes((0, phase, 2, 3))
        struct.pack_into("<4HII", body, 4, 0x1fff, 1, 0x1ff, 0x1ff, 123, 4)
        return bytes(body)

    def test_all_typed_response_shapes(self):
        bodies = {0: struct.pack("<4BIII", 1, 0, 1, 0, 0x1234, 45, 99),
                  3: b"\x07" + struct.pack("<3i", 1, -2, 3),
                  4: b"\x07" + struct.pack("<3f", 0.25, -0.5, 1),
                  5: b"\x07" + bytes(12),
                  7: struct.pack("<BfhhBBB", 1, 90, -12, 34, 120, 30, 0),
                  13: struct.pack("<II", 1, 100000), 17: struct.pack("<II", 50, 19999),
                  19: struct.pack("<II", 0x15, 0x1fff), 21: self.application_body(),
                  27: b"armed=0 estop=1 fault=0", 28: struct.pack("<5BI", 1, 2, 2, 1, 2, 800),
                  30: b"\x01" + struct.pack("<I", 12000), 34: struct.pack("<H", 0x1234)}
        for operation in range(36):
            with self.subTest(operation=operation):
                body = bodies.get(operation, b"")
                result = board.parse_maintenance(operation, 0, 0, b"\x00" + body)
                self.assertEqual(result["error"], "ok")
                if operation != 27:
                    with self.assertRaises(board.ProtocolError):
                        board.parse_maintenance(operation, 0, 0, b"\x00" + body + b"\x00")
                with self.assertRaises(board.ControllerError):
                    board.parse_maintenance(operation, 0, 0, b"\x0a")
                with self.assertRaises(board.ProtocolError):
                    board.parse_maintenance(operation, 0, 0, b"\x0a\x00")
        value = board.parse_maintenance(7, 103, 0, b"\x00" + bodies[7])
        self.assertEqual(value["present_speed_raw"], -12)
        value = board.parse_maintenance(21, 0, 0, b"\x00" + bodies[21])
        self.assertEqual(value["phase"], "disarmed")
        self.assertEqual(value["dropped_events"], 4)

    def test_servo_probe_exact_raw_reply_and_rejections(self):
        body = struct.pack("<HBBHHHBB", 12, 34, 98, 0, 1023, 0x1234, 1, 2)
        value = board.parse_maintenance(36, 98, 0, b"\x00" + body)
        self.assertEqual((value["model_number"], value["actual_id"], value["present_position_raw"]), (12, 98, 0x1234))
        self.assertNotIn("position_deg", value)
        for bad in (body[:-1], body + b"\x00", body[:3] + b"\x63" + body[4:],
                    body[:10] + b"\x02" + body[11:], body[:11] + b"\x03"):
            with self.assertRaises(board.ProtocolError): board.parse_maintenance(36, 98, 0, b"\x00" + bad)
        with self.assertRaises(board.ControllerError): board.parse_maintenance(36, 98, 0, b"\x04")
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(36, 98, 0, b"\x04\x00")
        for flags in (("--channel", "0"), ("--channel", "254"), ("--channel", "98", "--arg", "1")):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                board.arguments(["--port", "/never/open", "--maintenance", "servo_probe", *flags])

    def test_servo_probe_pty_is_one_query_no_claim_arm_or_retry(self):
        body = struct.pack("<HBBHHHBB", 12, 34, 98, 0, 1023, 0x1234, 0, 2)
        for response, expected in ((b"\x00" + body, 0), (b"\x04", 1)):
            def handle(frame):
                if frame.opcode == 8: return ack(frame, status())
                if frame.opcode == 9: return ack(frame, sensors())
                self.assertEqual(frame.payload, struct.pack("<BBII3f", 36, 98, 0, 0, 0, 0, 0))
                return ack(frame, response)
            with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(board.main(["--port", peer.path, "--maintenance", "servo_probe", "--channel", "98"]), expected)
                self.assertEqual([f.opcode for f in peer.received], [8, 9, 0x24])
                self.assertTrue(all(f.owner == 0 for f in peer.received))

    def test_quantities_stepper_and_corrupt_app_event(self):
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(255, 0, 0, b"\x00")
        quantities = (struct.pack("<I", 13000), struct.pack("<4H", 3990, 4000, 3980, 3970),
                      struct.pack("<H", 15940), struct.pack("<H", 500), bytes((80,)), bytes((4,)))
        for channel, value in enumerate(quantities):
            result = board.parse_maintenance(30, channel, 0, b"\x00\x01" + value)
            self.assertTrue(result["known"])
            if channel == 3: self.assertEqual(result["current_sign"], "unknown")
            unknown = board.parse_maintenance(30, channel, 0, b"\x00\x00" + value)
            self.assertFalse(unknown["known"])
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(30, 4, 0, b"\x00\x01\x65")
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(30, 5, 0, b"\x00\x01\x00")
        value = board.parse_maintenance(34, 0, 0x16, b"\x00" + struct.pack("<I", 0x88))
        self.assertEqual(value["parameter"], 0x88)
        bad = bytearray(self.application_body()); bad[23] = 1
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(21, 0, 0, b"\x00" + bad)
        bad = bytearray(self.application_body()); bad[20] = 13
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(21, 0, 0, b"\x00" + bad)
        bad = bytearray(self.application_body()); struct.pack_into("<H", bad, 4, 0)
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(21, 0, 0, b"\x00" + bad)
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(28, 0, 0, b"\x00\x01\x0d" + bytes(7))
        self.assertFalse(board.parse_maintenance(28, 0, 0, bytes(10))["available"])
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(27, 0, 0, b"\x00" + b"a" * 97)
        with self.assertRaises(board.ProtocolError): board.parse_maintenance(27, 0, 0, b"\x00bad\x00text")

    def test_permission_and_channel_gates(self):
        for name, operation in board.MAINTENANCE.items():
            if operation not in board.READ_ONLY_MAINTENANCE:
                with self.subTest(name=name), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    board.arguments(["--port", "/never/open", "--maintenance", name])
        rejected = [("--maintenance", "match_arm", "--allow-maintenance"),
                    ("--maintenance", "clear_reset", "--exercise", "maintenance", "--allow-motion", "--allow-maintenance"),
                    ("--maintenance", "servo_feedback", "--channel", "254"),
                    ("--maintenance", "battery_value", "--channel", "6"),
                    ("--maintenance", "stepper_set_parameter", "--allow-maintenance", "--arg", "0x19"),
                    ("--maintenance", "stepper_parameter", "--arg", "0x77"),
                    ("--maintenance", "battery_calibration", "--allow-maintenance", "--channel", "0x17"),
                    ("--maintenance", "gpio", "--floats", "nan", "0", "0"),
                    ("--maintenance", "gpio", "--exercise", "gripper", "--allow-motion")]
        for flags in rejected:
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                board.arguments(["--port", "/never/open", *flags])
        args = board.arguments(["--port", "/never/open", "--maintenance", "SERVO_SPEED", "--channel", "103",
                                "--signed-argument", "-12", "--maintenance-session"])
        self.assertEqual(args.arg, 0xfffffff4)
        self.assertFalse(args.allow_motion)
        self.assertEqual(board.maintenance_payload(args), struct.pack("<BBII3f", 9, 103, 0xfffffff4, 0, 0, 0, 0))

    def test_read_query_never_claims_even_when_someone_owns_board(self):
        def handle(frame):
            if frame.opcode == 8: return ack(frame, status(owner=777, flags=5))
            if frame.opcode == 9: return ack(frame, sensors())
            self.assertEqual(struct.unpack("<BBII3f", frame.payload), (0, 0, 0, 0, 0, 0, 0))
            return ack(frame, b"\x00" + struct.pack("<4BIII", 1, 1, 0, 0, 0, 44, 777))
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(board.main(["--port", peer.path, "--maintenance", "board_info"]), 0)
            self.assertEqual([f.opcode for f in peer.received], [8, 9, 0x24])
            self.assertTrue(all(f.owner == 0 for f in peer.received))

    def test_disarmed_write_and_post_match_comm_enable_never_arm(self):
        for name, argument in (("clear_reset", 0), ("communication", 1)):
            def handle(frame):
                if frame.opcode == 1: return ack(frame, status(owner=frame.owner, flags=6))
                if frame.opcode == 0x24:
                    self.assertEqual(frame.payload, struct.pack("<BBII3f", board.MAINTENANCE[name], 0, argument, 0, 0, 0, 0))
                    return ack(frame, b"\x00")
                return ack(frame, status(flags=2))
            args = board.arguments(["--port", "unused", "--maintenance", name, "--arg", str(argument), "--allow-maintenance"])
            with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
                port = board.SerialPort(peer.path)
                try: board.maintenance_session(board.Client(port, 0.2), args)
                finally: port.close()
                self.assertEqual([f.opcode for f in peer.received], [1, 0x24, 7])
                self.assertTrue(all(f.owner != 0 for f in peer.received))

    def test_reset_ack_has_no_post_reset_serial_request(self):
        def handle(frame):
            return ack(frame, status(owner=frame.owner, flags=4) if frame.opcode == 1 else b"\x00")
        args = board.arguments(["--port", "unused", "--maintenance", "board_reset", "--allow-maintenance"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()) as output:
            port = board.SerialPort(peer.path)
            try: board.maintenance_session(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertEqual([f.opcode for f in peer.received], [1, 0x24])
            self.assertIn('"board_reset_acknowledged": true', output.getvalue())

    def test_reset_timeout_has_no_retry_or_release(self):
        def handle(frame):
            return ack(frame, status(owner=frame.owner, flags=4)) if frame.opcode == 1 else None
        args = board.arguments(["--port", "unused", "--maintenance", "board_reset", "--allow-maintenance"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()) as output:
            port = board.SerialPort(peer.path)
            try:
                with self.assertRaises(board.BoardError): board.maintenance_session(board.Client(port, 0.05), args)
            finally: port.close()
            self.assertEqual([f.opcode for f in peer.received], [1, 0x24])
            self.assertIn('"board_reset_acknowledged": false', output.getvalue())

    def test_rejected_write_releases_claim_without_estop_or_replay(self):
        def handle(frame):
            if frame.opcode == 1: return ack(frame, status(owner=frame.owner, flags=4))
            return ack(frame, b"\x02" if frame.opcode == 0x24 else status())
        for operation in ("board_reset", "clear_reset"):
            args = board.arguments(["--port", "unused", "--maintenance", operation, "--allow-maintenance"])
            with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
                port = board.SerialPort(peer.path)
                try:
                    with self.assertRaises(board.ControllerError): board.maintenance_session(board.Client(port, 0.2), args)
                finally: port.close()
                self.assertEqual([f.opcode for f in peer.received], [1, 0x24, 7])

    def test_armed_pca_refresh_uses_new_sequence_and_stops(self):
        def handle(frame):
            if frame.opcode in (1, 2, 3): return ack(frame, status(owner=frame.owner, flags=4 if frame.opcode == 1 else 5))
            return ack(frame, b"\x00" if frame.opcode == 0x24 else status())
        args = board.arguments(["--port", "unused", "--maintenance", "pca_allpulse", "--arg", "1500",
                                "--exercise", "maintenance", "--allow-motion", "--allow-maintenance", "--hold-ms", "150"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
            port = board.SerialPort(peer.path)
            try: board.maintenance_session(board.Client(port, 0.2), args)
            finally: port.close()
            ops = [f.opcode for f in peer.received]
            self.assertEqual(ops[:2], [1, 2])
            self.assertGreaterEqual(ops.count(0x24), 2)
            self.assertEqual(ops[-2:], [6, 7])
            self.assertEqual(len({f.sequence for f in peer.received}), len(peer.received))

    def test_match_arm_is_once_then_queries_until_finished(self):
        def handle(frame):
            if frame.opcode in (1, 2, 3): return ack(frame, status(owner=frame.owner, flags=4 if frame.opcode == 1 else 5))
            if frame.opcode == 0x24:
                return ack(frame, b"\x00" + (self.application_body(3) if frame.payload[0] == 21 else b""))
            return ack(frame, status())
        args = board.arguments(["--port", "unused", "--maintenance", "match_arm", "--exercise", "maintenance",
                                "--allow-motion", "--allow-maintenance", "--hold-ms", "200"])
        with Peer(handle) as peer, contextlib.redirect_stdout(io.StringIO()):
            port = board.SerialPort(peer.path)
            try: board.maintenance_session(board.Client(port, 0.2), args)
            finally: port.close()
            self.assertEqual([f.payload[0] for f in peer.received if f.opcode == 0x24], [22, 21])
            self.assertEqual([f.opcode for f in peer.received][-2:], [6, 7])


if __name__ == "__main__":
    unittest.main()
