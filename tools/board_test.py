#!/usr/bin/env python3
"""Explicit, bounded Eurobot UART smoke client; default is read-only. Never flashes."""
from __future__ import annotations
import argparse
import collections
import dataclasses
import fcntl
import json
import math
import os
import secrets
import select
import struct
import sys
import termios
import time

MAGIC = b"\xa5\x5a"
MAX_PAYLOAD = 128
ERRORS = ("ok", "invalid_config", "invalid_argument", "not_armed", "estop", "lease_conflict",
          "lease_expired", "stale_sequence", "output_failure", "timeout", "unsupported", "malformed", "crc")
PHASES = ("idle", "running", "completed", "cancelled", "faulted")
EVIDENCE = ("unknown", "commanded", "settled", "referenced_steps", "measured")
OPERATIONS = {"cursor": 0, "side_grips": 1, "rotate_main": 2, "pumps": 3, "valves": 4,
              "blocks_rotate": 5, "timed_move": 6, "lift_height": 7, "relative_move": 8,
              "wheel_calibration": 9, "blocks_shift": 10, "blocks_rotate_only": 11,
              "relative_move_poll": 12, "slide": 13, "legacy_2025_reset": 20,
              "legacy_2025_cycle": 21, "capture": 22, "release": 23, "prepare": 24}


class BoardError(Exception):
    pass


class ProtocolError(BoardError):
    pass


class ControllerError(BoardError):
    def __init__(self, error: int):
        self.error = error
        super().__init__(f"Firmware rejected request: {error_name(error)} ({error})")


def error_name(code: int) -> str:
    return ERRORS[code] if 0 <= code < len(ERRORS) else f"unknown_error_{code}"


def crc16(data: bytes) -> int:
    value = 0xffff
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xffff
    return value


@dataclasses.dataclass(frozen=True)
class Frame:
    opcode: int
    sequence: int
    owner: int
    payload: bytes = b""


def encode_frame(frame: Frame) -> bytes:
    if not 0 <= frame.opcode <= 255 or len(frame.payload) > MAX_PAYLOAD:
        raise ProtocolError("Frame opcode/length out of range")
    if not 0 <= frame.sequence <= 0xffffffff or not 0 <= frame.owner <= 0xffffffff:
        raise ProtocolError("Frame owner/sequence out of range")
    body = struct.pack("<BBIIH", 1, frame.opcode, frame.sequence, frame.owner, len(frame.payload)) + frame.payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class Parser:
    """Resynchronizes bounded chunks; corrupt/unrelated frames never become a matching ACK."""
    def __init__(self):
        self.buffer = bytearray()
        self.rejected = 0

    def feed(self, data: bytes) -> list[Frame]:
        result = []
        # Consume incrementally so even a large caller-provided chunk cannot grow storage unbounded.
        for byte in data:
            self.buffer.append(byte)
            while self.buffer:
                index = self.buffer.find(MAGIC)
                if index < 0:
                    self.buffer[:] = b"\xa5" if self.buffer[-1] == 0xa5 else b""
                    break
                if index:
                    del self.buffer[:index]
                if len(self.buffer) < 14:
                    break
                version, opcode, sequence, owner, length = struct.unpack_from("<BBIIH", self.buffer, 2)
                if version != 1 or length > MAX_PAYLOAD:
                    self.rejected += 1
                    del self.buffer[0]
                    continue
                size = length + 16
                if len(self.buffer) < size:
                    break
                packet = bytes(self.buffer[:size])
                if crc16(packet[2:-2]) != struct.unpack_from("<H", packet, size - 2)[0]:
                    self.rejected += 1
                    del self.buffer[0]
                    continue
                result.append(Frame(opcode, sequence, owner, packet[14:-2]))
                del self.buffer[:size]
        return result


def _error(payload: bytes) -> int:
    if not payload or payload[0] >= len(ERRORS):
        raise ProtocolError("Invalid firmware error code")
    return payload[0]


def parse_status(payload: bytes) -> dict:
    if len(payload) != 84:
        raise ProtocolError(f"Expected status84, received {len(payload)} bytes")
    error = _error(payload)
    flags, channels, measured_mask = payload[1:4]
    feedback = payload[76]
    if flags & ~15 or not 1 <= channels <= 32 or measured_mask & ~7 or feedback & ~7:
        raise ProtocolError("Invalid status flags/channel count")
    if any(code >= len(ERRORS) for code in payload[77:80]):
        raise ProtocolError("Invalid status fault code")
    masks = struct.unpack_from("<8I", payload, 8)
    valid_mask = (1 << channels) - 1
    if any(mask & ~valid_mask for mask in masks):
        raise ProtocolError("Status output mask exceeds channel count")
    pose = struct.unpack_from("<3f", payload, 64)
    if feedback & 2 and not all(math.isfinite(value) for value in pose):
        raise ProtocolError("Known pose contains non-finite values")
    if feedback & 1 and measured_mask != 7:
        raise ProtocolError("Inconsistent wheel feedback validity")
    names = ("pumps_requested", "pumps_commanded", "pumps_commanded_known", "pumps_measured",
             "pumps_measured_known", "valves_requested", "valves_commanded", "valves_commanded_known")
    return {"error": error_name(error), "error_code": error, "armed": bool(flags & 1),
            "estop_latched": bool(flags & 2), "lease_active": bool(flags & 4), "fault": bool(flags & 8),
            "channels": channels, "owner": struct.unpack_from("<I", payload, 4)[0],
            **dict(zip(names, masks)), "wheel_measured_known": measured_mask,
            "wheel_commanded_tpm": struct.unpack_from("<3i", payload, 40),
            "wheel_measured_tpm": struct.unpack_from("<3i", payload, 52),
            "pose_known": bool(feedback & 2), "pose_m_m_rad": pose if feedback & 2 else None,
            "wheel_commanded_known": bool(feedback & 4),
            "controller_error": error_name(payload[77]), "pump_error": error_name(payload[78]),
            "chassis_error": error_name(payload[79]), "uptime_ms": struct.unpack_from("<I", payload, 80)[0]}


def parse_sensors(payload: bytes) -> dict:
    error = _error(payload)
    if len(payload) == 1 and error:
        return {"error": error_name(error), "battery_known": False, "gpio_known_mask": 0}
    if len(payload) != 25 or payload[1] > 1 or payload[24] >= len(ERRORS):
        raise ProtocolError("Expected valid sensor25 payload or one-byte error NACK")
    levels, known = struct.unpack_from("<HH", payload, 20)
    if (levels | known) & ~0x1fff:
        raise ProtocolError("GPIO bits exceed the13 physical inputs")
    battery_known = error == 0 and payload[1] == 1 and payload[24] == 0
    if battery_known and (payload[18] > 100 or not 1 <= payload[19] <= 4):
        raise ProtocolError("Invalid known battery SOC/cell count")
    cells = payload[19] if battery_known else 0
    return {"error": error_name(error), "battery_error": error_name(payload[24]),
            "battery_known": battery_known, "power_mw": struct.unpack_from("<I", payload, 2)[0] if battery_known else None,
            "cell_voltage_mv": struct.unpack_from("<4H", payload, 6)[:cells],
            "voltage_mv": struct.unpack_from("<H", payload, 14)[0] if battery_known else None,
            "raw_current_ma": struct.unpack_from("<H", payload, 16)[0] if battery_known else None,
            "current_sign": "unknown", "soc_percent": payload[18] if battery_known else None,
            "gpio_raw_levels": levels if error == 0 else 0, "gpio_known_mask": known if error == 0 else 0}


def parse_job(payload: bytes) -> dict:
    error = _error(payload)
    if len(payload) == 1 and error:
        raise ControllerError(error)
    if len(payload) != 25:
        raise ProtocolError("Expected job25 payload or one-byte error NACK")
    if payload[1] >= len(PHASES) or payload[2] not in OPERATIONS.values() or payload[3] > 1:
        raise ProtocolError("Invalid job phase/operation/gripper")
    if payload[5] > 64 or payload[4] > payload[5] or payload[6] >= len(EVIDENCE):
        raise ProtocolError("Invalid job step/evidence")
    if any(count > payload[5] for count in payload[7:11]):
        raise ProtocolError("Invalid job evidence counters")
    if payload[11] >= len(ERRORS) or payload[12] >= len(ERRORS):
        raise ProtocolError("Invalid job error code")
    job_id, owner, elapsed = struct.unpack_from("<III", payload, 13)
    evidence_total = sum(payload[7:11])
    phase, step, count = payload[1], payload[4], payload[5]
    if evidence_total > step:
        raise ProtocolError("Evidence counters exceed completed steps")
    if phase == 1 and step >= count:
        raise ProtocolError("Running job has no remaining step")
    if phase == 2 and (count == 0 or step != count or evidence_total != count):
        raise ProtocolError("Completed job has incomplete/empty evidence")
    if phase != 0 and (job_id == 0 or owner == 0):
        raise ProtocolError("Accepted job has zero identity")
    return {"error": error_name(error), "error_code": error, "phase": PHASES[payload[1]],
            "operation": payload[2], "gripper_index": payload[3], "step": payload[4], "step_count": payload[5],
            "evidence": EVIDENCE[payload[6]], "commanded_steps": payload[7], "settled_steps": payload[8],
            "referenced_steps": payload[9], "measured_steps": payload[10], "job_error": error_name(payload[11]),
            "stop_error": error_name(payload[12]), "job_id": job_id, "owner": owner, "elapsed_ms": elapsed}


MAINTENANCE = {"board_info": 0, "board_reset": 1, "clear_reset": 2, "wheel_ticks": 3,
               "wheel_path": 4, "reset_wheel_ticks": 5, "set_pose": 6, "servo_feedback": 7,
               "servo_led": 8, "servo_speed": 9, "servo_torque_limit": 10, "servo_wheel_mode": 11,
               "servo_id": 12, "motor_range": 13, "set_motor_range": 14, "motor_id": 15,
               "stepper_reset": 16, "pwm_info": 17, "set_pwm_frequency": 18, "gpio": 19,
               "battery_calibration": 20, "application_state": 21, "match_arm": 22,
               "match_disarm": 23, "application_mode": 24, "communication": 25,
               "wheel_holding": 26, "diagnostic_text": 27, "application_event": 28,
               "battery_id": 29, "battery_value": 30, "device_ping": 31, "pca_control": 32,
               "pca_allpulse": 33, "stepper_parameter": 34, "stepper_set_parameter": 35, "servo_probe": 36}
READ_ONLY_MAINTENANCE = frozenset((0, 3, 4, 7, 13, 17, 19, 21, 27, 28, 30, 31, 34, 36))
ARMED_MAINTENANCE = frozenset((22, 26, 33))
L6474_REGISTERS = {0x01: 0x3fffff, 0x02: 0x1ff, 0x03: 0x3fffff, 0x09: 0x7f,
                    0x0e: 0xff, 0x0f: 0x7f, 0x10: 0x7f, 0x12: 0x3f, 0x13: 0xf,
                    0x16: 0xff, 0x17: 0xcf, 0x18: 0x7faf, 0x19: 0xffff}


def parse_maintenance(operation: int, channel: int, argument: int, payload: bytes) -> dict:
    if operation not in MAINTENANCE.values():
        raise ProtocolError("Unknown named maintenance operation")
    error = _error(payload)
    if error:
        if len(payload) != 1:
            raise ProtocolError("Malformed maintenance NACK has unexpected body")
        raise ControllerError(error)
    body = payload[1:]
    expected = {0: 16, 3: 13, 4: 13, 5: 13, 7: 12, 13: 8, 17: 8, 19: 8, 21: 24, 28: 9, 36: 12}
    if operation == 30:
        if not 0 <= channel <= 5:
            raise ProtocolError("Unsupported battery quantity")
        length = (5, 9, 3, 3, 2, 2)[channel]
    elif operation == 34:
        length = 2 if argument == 0 else 4
    else:
        length = expected.get(operation, 0)
    if operation == 27:
        if not 1 <= len(body) <= 96 or any(byte < 32 or byte > 126 for byte in body):
            raise ProtocolError("Invalid bounded diagnostic text")
    elif len(body) != length:
        raise ProtocolError(f"Maintenance operation{operation} expected body{length}, received{len(body)}")
    result = {"operation": next((name for name, number in MAINTENANCE.items() if number == operation), str(operation)),
              "error": "ok", "channel": channel}
    if operation == 0:
        if any(flag > 1 for flag in body[:4]):
            raise ProtocolError("Invalid board-info booleans")
        reset, uptime, owner = struct.unpack_from("<III", body, 4)
        result.update(reset_latched=bool(body[0]), armed=bool(body[1]), estop=bool(body[2]),
                      fault=bool(body[3]), reset_cause=reset, uptime_ms=uptime, owner=owner)
    elif operation in (3, 4, 5):
        if body[0] & ~7:
            raise ProtocolError("Invalid wheel known mask")
        values = struct.unpack_from("<3f" if operation == 4 else "<3i", body, 1)
        if operation == 4 and any(body[0] & (1 << i) and not math.isfinite(value)
                                  for i, value in enumerate(values)):
            raise ProtocolError("Non-finite known wheel path")
        result.update(known_mask=body[0], **{"path_m" if operation == 4 else "total_ticks":
                      tuple(value if body[0] & (1 << i) else None for i, value in enumerate(values))})
    elif operation == 7:
        if body[0] > 1 or body[11] > 1:
            raise ProtocolError("Invalid servo feedback validity/moving")
        position = struct.unpack_from("<f", body, 1)[0]
        speed, load = struct.unpack_from("<hh", body, 5)
        if body[0] and (not math.isfinite(position) or not 0 <= position <= 360 or
                        abs(speed) > 1023 or abs(load) > 1023):
            raise ProtocolError("Invalid known servo feedback")
        result.update(known=bool(body[0]), position_deg=position if body[0] else None,
                      present_speed_raw=speed if body[0] else None,
                      present_load_raw=load if body[0] else None,
                      voltage_decivolts=body[9] if body[0] else None,
                      temperature_c=body[10] if body[0] else None, moving=bool(body[11]) if body[0] else None)
    elif operation == 36:
        model, firmware, actual_id, cw, ccw, position, torque, return_level = struct.unpack("<HBBHHHBB", body)
        if not 1 <= channel <= 253 or actual_id != channel or torque > 1 or return_level > 2:
            raise ProtocolError("Invalid servo probe identity/torque/status return level")
        result.update(model_number=model, firmware_version=firmware, actual_id=actual_id,
                      cw_angle_limit_raw=cw, ccw_angle_limit_raw=ccw,
                      present_position_raw=position, torque_enabled=bool(torque),
                      status_return_level=return_level)
    elif operation in (13, 17):
        first, second = struct.unpack("<II", body)
        if operation == 13:
            if first > second:
                raise ProtocolError("Invalid motor range")
            result.update(lower_tpm=first, upper_tpm=second)
        else:
            result.update(frequency_hz=first, autoreload=second)
    elif operation == 19:
        raw, known = struct.unpack("<II", body)
        if (raw | known) & ~0x1fff:
            raise ProtocolError("Invalid maintenance GPIO mask")
        result.update(raw_levels=raw, known_mask=known)
    elif operation == 21:
        if body[0] > 1 or body[1] > 4 or body[3] & ~0x7f or body[23] != 0:
            raise ProtocolError("Invalid application state enums/flags")
        known, pressed, switch_known, switches = struct.unpack_from("<4H", body, 4)
        if (known | pressed) & ~0x1fff or (switch_known | switches) & ~0x1ff:
            raise ProtocolError("Invalid application input masks")
        if pressed & ~known or switches & ~switch_known:
            raise ProtocolError("Application pressed bits are not known")
        if body[20] >= len(ERRORS) or body[21] >= len(ERRORS) or body[22] > 8:
            raise ProtocolError("Invalid application error/reason")
        flags = body[3]
        elapsed, dropped = struct.unpack_from("<II", body, 12)
        result.update(mode=("game", "debug")[body[0]],
                      phase=("disarmed", "waiting_for_release", "running", "finished", "faulted")[body[1]],
                      strategy=body[2], communication_enabled=bool(flags & 1), reset_latched=bool(flags & 2),
                      reset_pending=bool(flags & 4), motor_enable_requested=bool(flags & 8),
                      cord_attached=bool(flags & 16), side_right=bool(flags & 32),
                      start_action_skipped=bool(flags & 64), input_known=known, input_pressed=pressed,
                      switch_known=switch_known, switches_pressed=switches, match_elapsed_ms=elapsed,
                      dropped_events=dropped, application_error=error_name(body[20]),
                      stop_error=error_name(body[21]), stop_reason=body[22])
    elif operation == 27:
        result["text"] = body.decode("ascii")
    elif operation == 28:
        if body[0] > 1:
            raise ProtocolError("Invalid event available flag")
        if body[0] == 0:
            if any(body[1:]):
                raise ProtocolError("Empty event contains nonzero data")
            result["available"] = False
        else:
            if body[1] >= 13 or body[2] > 2 or body[3] > 2 or body[4] > 2:
                raise ProtocolError("Invalid application event enums")
            if (body[2] == 2 and body[3] == 0) or (body[2] != 2 and body[3] != 0):
                raise ProtocolError("Inconsistent application callback/action")
            result.update(available=True, button=body[1],
                          callback=("init_complete", "state_changed", "action")[body[2]],
                          action=("none", "click", "hold")[body[3]],
                          state=("unknown", "pressed", "released")[body[4]],
                          at_ms=struct.unpack_from("<I", body, 5)[0])
    elif operation == 30:
        if body[0] > 1:
            raise ProtocolError("Invalid battery value known flag")
        value = (struct.unpack_from("<I", body, 1)[0] if channel == 0 else
                 struct.unpack_from("<4H", body, 1) if channel == 1 else
                 struct.unpack_from("<H", body, 1)[0] if channel in (2, 3) else body[1])
        if body[0] and ((channel == 4 and value > 100) or (channel == 5 and not 1 <= value <= 4)):
            raise ProtocolError("Invalid known battery quantity")
        names = ("power_mw", "cells_mv", "voltage_mv", "raw_current_ma", "soc_percent", "cell_count")
        result.update(known=bool(body[0]), **{names[channel]: value if body[0] else None})
        if channel == 3:
            result["current_sign"] = "unknown"
    elif operation == 34:
        result["status" if argument == 0 else "parameter"] = struct.unpack("<H" if argument == 0 else "<I", body)[0]
    return result


def maintenance_payload(args) -> bytes:
    return struct.pack("<BBII3f", MAINTENANCE[args.maintenance], args.channel,
                       args.arg, args.secondary, *args.floats)


def maintenance_session(client, args) -> None:
    operation = MAINTENANCE[args.maintenance]
    payload = maintenance_payload(args)
    if operation in READ_ONLY_MAINTENANCE:
        emit("maintenance", client.maintenance_command(operation, args.channel, args.arg, payload))
        return
    if not args.allow_maintenance:
        raise BoardError("Named maintenance mutation requires--allow-maintenance")
    claimed, reset_outcome = False, "not_sent"
    armed = operation in ARMED_MAINTENANCE
    client.owner = secrets.randbits(32) or 1
    cleanup_errors = []
    try:
        status = client.status_command(1, struct.pack("<I", 2000))
        claimed = True
        if status["owner"] != client.owner or not status["lease_active"] or status["armed"]:
            raise ProtocolError("Maintenance claim must confirm this disarmed owner")
        if armed:
            if args.exercise != "maintenance" or not args.allow_motion:
                raise BoardError("Armed maintenance requires--exercise maintenance --allow-motion")
            status = client.status_command(2)
            if not status["armed"] or status["owner"] != client.owner:
                raise ProtocolError("Maintenance arm ACK does not confirm this owner")
        if operation == 1:
            reset_outcome = "sent"
        try:
            result = client.maintenance_command(operation, args.channel, args.arg, payload)
        except ControllerError:
            reset_outcome = "rejected" if operation == 1 else reset_outcome
            raise
        if operation == 1:
            reset_outcome = "acknowledged"
        emit("maintenance", result)
        if armed:
            deadline = time.monotonic() + args.hold_ms / 1000
            heartbeat_at = time.monotonic()
            while time.monotonic() < deadline:
                time.sleep(min(0.1, max(0, deadline - time.monotonic())))
                if time.monotonic() - heartbeat_at >= 0.4:
                    client.status_command(3)
                    heartbeat_at = time.monotonic()
                if operation == 33:
                    # This is an explicit repeated refresh with NEW sequence, never an error retry.
                    client.maintenance_command(operation, args.channel, args.arg, payload)
                elif operation == 22:
                    query = struct.pack("<BBII3f", 21, 0, 0, 0, 0, 0, 0)
                    snapshot = client.maintenance_command(21, 0, 0, query)
                    if snapshot["phase"] in ("finished", "faulted", "disarmed"):
                        emit("application", snapshot)
                        break
    finally:
        if claimed and reset_outcome in ("sent", "acknowledged"):
            emit("cleanup", {"board_reset_acknowledged": reset_outcome == "acknowledged",
                             "expected_disconnect": True, "release_after_reset_not_sent": True,
                             "physical_stop_verified": False})
        elif claimed:
            commands = ((6, "ESTOP"), (7, "release")) if armed else ((7, "release"),)
            for opcode, name in commands:
                try:
                    client.status_command(opcode, cleanup=True)
                except (OSError, BoardError) as error:
                    cleanup_errors.append(f"{name}: {error}")
            emit("cleanup", {"errors": cleanup_errors, "armed_exercise": armed,
                             "physical_stop_verified": False})
            if cleanup_errors and sys.exc_info()[0] is None:
                raise BoardError("Maintenance release not confirmed; no retry/reconnect")


class SerialPort:
    def __init__(self, path: str, baud: int = 115200):
        if baud != 115200:
            raise BoardError("This firmware contract requires115200baud")
        self.fd = -1
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            exclusive = getattr(termios, "TIOCEXCL", None)
            if exclusive is None:
                raise BoardError("TIOCEXCL unavailable; refusing non-exclusive serial access")
            fcntl.ioctl(self.fd, exclusive)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB |
                                    getattr(termios, "CRTSCTS", 0))) | termios.CS8 | termios.CLOCAL | termios.CREAD
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def write_all(self, data: bytes, deadline: float):
        view = memoryview(data)
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BoardError("Serial write deadline expired")
            if not select.select([], [self.fd], [], remaining)[1]:
                raise BoardError("Serial write timeout")
            try:
                count = os.write(self.fd, view)
            except BlockingIOError:
                continue
            if count <= 0:
                raise BoardError("Serial write failed")
            view = view[count:]

    def read(self, deadline: float) -> bytes:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], max(0, remaining))[0]:
                raise BoardError("Serial ACK timeout; no retry or reconnect")
            try:
                data = os.read(self.fd, 1024)
            except BlockingIOError:
                continue
            if not data:
                raise BoardError("Serial disconnected")
            return data


class Client:
    def __init__(self, port: SerialPort, timeout: float = 0.5, owner: int = 0):
        if not 0.02 <= timeout <= 1:
            raise BoardError("ACK timeout must be0.02…1second")
        self.port, self.timeout, self.owner = port, timeout, owner
        self.sequence = 0
        self.parser = Parser()
        self.pending = collections.deque()
        self.failed = False

    def request(self, opcode: int, payload: bytes = b"", *, cleanup: bool = False) -> bytes:
        if self.failed and not cleanup:
            raise BoardError("Session failed; automatic replay/reconnect is forbidden")
        self.sequence = (self.sequence + 1) & 0xffffffff
        frame = Frame(opcode, self.sequence, self.owner, payload)
        deadline = time.monotonic() + self.timeout
        try:
            self.port.write_all(encode_frame(frame), deadline)
            while True:
                while self.pending:
                    reply = self.pending.popleft()
                    if (reply.opcode, reply.sequence, reply.owner) == (opcode | 0x80, frame.sequence, frame.owner):
                        return reply.payload
                if time.monotonic() >= deadline:
                    raise BoardError("Matching ACK deadline expired")
                self.pending.extend(self.parser.feed(self.port.read(deadline)))
        except (OSError, BoardError):
            self.failed = True
            raise

    def status_command(self, opcode: int, payload: bytes = b"", *, cleanup: bool = False) -> dict:
        try:
            result = parse_status(self.request(opcode, payload, cleanup=cleanup))
            if result["error_code"]:
                raise ControllerError(result["error_code"])
            return result
        except BoardError:
            self.failed = True
            raise

    def maintenance_command(self, operation: int, channel: int, argument: int, payload: bytes) -> dict:
        try:
            return parse_maintenance(operation, channel, argument, self.request(0x24, payload))
        except BoardError:
            self.failed = True
            raise

    def job_command(self, opcode: int, payload: bytes, *, cleanup: bool = False) -> dict:
        try:
            result = parse_job(self.request(opcode, payload, cleanup=cleanup))
            if result["error_code"]:
                raise ControllerError(result["error_code"])
            return result
        except BoardError:
            self.failed = True
            raise


def emit(label: str, value: dict):
    print(json.dumps({label: value}, ensure_ascii=False, allow_nan=False), flush=True)


def require_idle(status: dict):
    if status["owner"] != 0 or status["lease_active"] or status["armed"]:
        raise BoardError("Controller already owned/armed; refusing to take over")


def exercise(client: Client, args) -> None:
    # Every potentially enabling call follows explicit CLI authorization and a fresh nonce.
    if not args.allow_motion:
        raise BoardError("--exercise requires explicit --allow-motion")
    claimed, job_id, job_active = False, 0, False
    cleanup_errors = []
    client.owner = secrets.randbits(32) or 1
    try:
        status = client.status_command(1, struct.pack("<I", 2000))
        claimed = True
        if status["owner"] != client.owner or not status["lease_active"]:
            raise ProtocolError("Claim ACK does not confirm this owner/lease")
        armed = client.status_command(2)
        if not armed["armed"] or armed["owner"] != client.owner:
            raise ProtocolError("Arm ACK does not confirm this armed owner")
        heartbeat_at = time.monotonic()
        if args.exercise == "gripper":
            operation = OPERATIONS[args.operation]
            payload = struct.pack("<BBfIIff", args.gripper - 1, operation, args.value,
                                  args.mask, args.job_timeout_ms, args.argument, args.speed)
            result = client.job_command(0x21, payload)
            job_id = result["job_id"]
            job_active = result["phase"] == "running"
            if job_id == 0 or result["owner"] != client.owner:
                raise ProtocolError("Submit ACK has invalid job identity")
            deadline = time.monotonic() + args.job_timeout_ms / 1000 + 1
            previous = None
            while True:
                if result["job_id"] != job_id or result["owner"] != client.owner:
                    raise ProtocolError("Job snapshot belongs to another job/owner")
                if result["operation"] != operation or result["gripper_index"] != args.gripper - 1:
                    raise ProtocolError("Job operation/gripper changed")
                signature = (result["phase"], result["step"], result["evidence"])
                if signature != previous:
                    emit("job", result)
                    previous = signature
                if result["phase"] != "running":
                    job_active = False
                    if result["phase"] != "completed" or result["job_error"] != "ok" or result["stop_error"] != "ok":
                        raise BoardError("Job did not complete successfully")
                    break
                if time.monotonic() >= deadline:
                    raise BoardError("Host job deadline expired")
                if time.monotonic() - heartbeat_at >= 0.4:
                    client.status_command(3)
                    heartbeat_at = time.monotonic()
                time.sleep(0.05)
                result = client.job_command(0x22, struct.pack("<I", job_id))
        else:
            # Pump command freshness is distinct from lease: refresh the selected output every100ms.
            deadline = time.monotonic() + args.duration_ms / 1000
            payload = struct.pack("<4I", args.select, args.values, args.valve_select, args.valve_values)
            while True:
                client.status_command(4, payload)
                if time.monotonic() >= deadline:
                    break
                if time.monotonic() - heartbeat_at >= 0.4:
                    client.status_command(3)
                    heartbeat_at = time.monotonic()
                time.sleep(min(0.1, max(0, deadline - time.monotonic())))
    finally:
        if claimed:
            if job_id and job_active:
                try:
                    client.job_command(0x23, struct.pack("<I", job_id), cleanup=True)
                except (OSError, BoardError) as error:
                    cleanup_errors.append(f"cancel: {error}")
            # ESTOP is deliberate at the end of an authorized exercise and latches stop.
            for opcode, name in ((6, "ESTOP"), (7, "release")):
                try:
                    client.status_command(opcode, cleanup=True)
                except (OSError, BoardError) as error:
                    cleanup_errors.append(f"{name}: {error}")
            emit("cleanup", {"errors": cleanup_errors, "estop_latched_expected": True,
                             "physical_stop_verified": False})
            if cleanup_errors and sys.exc_info()[0] is None:
                raise BoardError("Cleanup not confirmed; use hardware energy isolation")


def integer(value: str) -> int:
    result = int(value, 0)
    if not 0 <= result <= 0xffffffff:
        raise argparse.ArgumentTypeError("Expected uint32")
    return result


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Explicit /dev/cu.* on Mac or /dev/serial/by-id/... on Linux")
    parser.add_argument("--baud", type=int, default=115200, choices=(115200,))
    parser.add_argument("--timeout", type=float, default=0.5, help="Bounded ACK seconds0.02…1")
    parser.add_argument("--exercise", choices=("gripper", "output", "maintenance"))
    parser.add_argument("--allow-motion", action="store_true", help="Explicitly permit claim/arm and this one physical exercise")
    parser.add_argument("--operation", choices=tuple(OPERATIONS), default="cursor")
    parser.add_argument("--gripper", type=int, choices=(1, 2), default=1)
    parser.add_argument("--value", type=float, default=0)
    parser.add_argument("--argument", type=float, default=0)
    parser.add_argument("--speed", type=float, default=0)
    parser.add_argument("--mask", type=integer, default=0)
    parser.add_argument("--job-timeout-ms", type=int, default=10000)
    parser.add_argument("--select", type=integer, default=0)
    parser.add_argument("--values", type=integer, default=0)
    parser.add_argument("--valve-select", type=integer, default=0)
    parser.add_argument("--valve-values", type=integer, default=0)
    parser.add_argument("--duration-ms", type=int, default=500)
    parser.add_argument("--maintenance", type=str.lower, choices=tuple(MAINTENANCE),
                        help="One named, typed board/peripheral operation; queries use owner0")
    parser.add_argument("--allow-maintenance", "--maintenance-session", dest="allow_maintenance",
                        action="store_true", help="Explicitly permit one named maintenance mutation")
    parser.add_argument("--channel", type=integer, default=0)
    parser.add_argument("--arg", type=integer, default=0)
    parser.add_argument("--secondary", type=integer, default=0)
    parser.add_argument("--floats", nargs=3, type=float, default=(0.0, 0.0, 0.0))
    parser.add_argument("--signed-argument", type=int, help="Signed servo_speed value(-1023…1023)")
    parser.add_argument("--hold-ms", type=int, default=500,
                        help="Bounded armed maintenance hold0…120000ms; heartbeat only while this client owns it")
    args = parser.parse_args(argv)
    if not 0.02 <= args.timeout <= 1 or not 100 <= args.job_timeout_ms <= 60000:
        parser.error("ACK timeout0.02…1s and job timeout100…60000ms required")
    if not 1 <= args.duration_ms <= 5000:
        parser.error("Output duration must be1…5000ms")
    if not all(math.isfinite(value) and abs(value) <= 3.4028234663852886e38
               for value in (args.value, args.argument, args.speed, *args.floats)):
        parser.error("Parameters must be finite float32")
    if args.values & ~args.select or args.valve_values & ~args.valve_select:
        parser.error("Output values must be subsets of select masks")
    if bool(args.exercise) != args.allow_motion:
        parser.error("Use --exercise together with --allow-motion; default is read-only")
    if args.exercise == "output" and not (args.select or args.valve_select):
        parser.error("Output exercise needs an explicit nonzero select mask")
    if args.channel > 255 or not 0 <= args.hold_ms <= 120000:
        parser.error("Maintenance channel must beuint8; hold0…120000ms")
    if args.allow_maintenance and not args.maintenance:
        parser.error("--allow-maintenance needs a named--maintenance operation")
    if args.maintenance and args.exercise in ("gripper", "output"):
        parser.error("Maintenance cannot share a gripper/output exercise")
    if args.signed_argument is not None:
        if args.maintenance != "servo_speed" or not -1023 <= args.signed_argument <= 1023:
            parser.error("--signed-argument is only for servo_speed -1023…1023")
        if args.arg != 0:
            parser.error("Use either--arg or--signed-argument, not both")
        args.arg = args.signed_argument & 0xffffffff
    if args.maintenance:
        op = MAINTENANCE[args.maintenance]
        if op not in READ_ONLY_MAINTENANCE and not args.allow_maintenance:
            parser.error("Maintenance mutation requires--allow-maintenance(or--maintenance-session)")
        if op in ARMED_MAINTENANCE:
            if args.exercise != "maintenance" or not args.allow_motion:
                parser.error("This operation requires--exercise maintenance --allow-motion")
        elif args.exercise == "maintenance":
            parser.error("Read-only/disarmed maintenance must not arm the controller")
        if op in (7, 8, 9, 10, 11, 12, 36) and not 1 <= args.channel <= 253:
            parser.error("Servo channel is an individual ID1…253; broadcast forbidden")
        if op == 36 and (args.arg != 0 or args.secondary != 0 or any(args.floats)):
            parser.error("servo_probe accepts only--channel; extra values must be zero")
        if op in (13, 14, 15) and args.channel >= 3:
            parser.error("Motor channel is wheel index0…2")
        if op in (16, 34, 35) and args.channel >= 4:
            parser.error("Stepper channel is index0…3")
        if op in (17, 18) and args.channel > 1:
            parser.error("PWM channel is timer group0/1")
        if op in (8, 11, 24, 25, 26) and args.arg > 1:
            parser.error("This operation requires boolean/enum argument0/1")
        signed_speed = args.arg if args.arg < 0x80000000 else args.arg - 0x100000000
        if op == 9 and not -1023 <= signed_speed <= 1023:
            parser.error("Servo raw speed must be-1023…1023")
        if op == 10 and args.arg > 1023:
            parser.error("Servo torque limit must be0…1023")
        if op in (12, 15, 29) and not 1 <= args.arg <= 253:
            parser.error("New device ID must be1…253")
        if op == 14 and args.arg > args.secondary:
            parser.error("Motor lower range exceeds upper range")
        if op == 18 and not 1 <= args.arg <= 1000000:
            parser.error("PWM frequency outside1…1000000Hz")
        if op == 20 and (args.channel not in range(0x16, 0x21, 2) or args.arg > 65535):
            parser.error("BMS calibration register must beeven0x16…0x20 and valueuint16")
        if op == 30 and args.channel > 5:
            parser.error("Battery quantity channel must be0…5")
        if op == 31 and not ((args.channel == 0 and args.arg < 3) or
                             (args.channel == 1 and 1 <= args.arg <= 253) or args.channel == 2):
            parser.error("Ping target must bemotor index0…2,allowedservo ID,orconfiguredBMS")
        if op == 32 and args.arg > 3:
            parser.error("PCAcontrol argument0off/1sleep/2wake/3initialize")
        if op == 33 and not 1 <= args.arg <= 65535:
            parser.error("PCAallpulse requires explicituint16 pulse inus")
        if op in (34, 35):
            if args.arg not in L6474_REGISTERS and not (op == 34 and args.arg == 0):
                parser.error("Unknown typedL6474 parameter; no arbitraryregister passthrough")
            if op == 35 and (args.arg in (0x12, 0x19) or args.secondary & ~L6474_REGISTERS[args.arg]):
                parser.error("Read-onlyL6474register or value exceeds registermask")
    elif args.exercise == "maintenance":
        parser.error("--exercise maintenance needs a named--maintenance operation")
    return args


def main(argv=None) -> int:
    args = arguments(argv)
    port = None
    try:
        port = SerialPort(args.port, args.baud)
        client = Client(port, args.timeout)
        status = client.status_command(8)
        emit("status", status)
        emit("sensors", parse_sensors(client.request(9)))
        if args.maintenance:
            if MAINTENANCE[args.maintenance] not in READ_ONLY_MAINTENANCE:
                require_idle(status)
            maintenance_session(client, args)
        elif args.exercise:
            require_idle(status)
            exercise(client, args)
        return 0
    except (OSError, BoardError, KeyboardInterrupt) as error:
        print(f"FAILED: {error}; no automatic retry/reconnect/flash", file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if port is not None:
            port.close()


if __name__ == "__main__":
    raise SystemExit(main())
