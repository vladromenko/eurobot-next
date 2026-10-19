"""ROS graph + real bridge executable + bounded PTY peer; never opens hardware."""
import os
import select
import struct
import subprocess
import threading
import time
from contextlib import ExitStack
from pathlib import Path

import rclpy
from action_msgs.msg import GoalStatus
from diagnostic_msgs.msg import DiagnosticArray
from eurobot_interfaces.action import Gripper
from eurobot_interfaces.msg import ControllerState
from eurobot_interfaces.srv import Arm, Maintenance, SetActuator, SetOutputs
from std_srvs.srv import Trigger
from geometry_msgs.msg import TwistStamped
from rclpy.action import ActionClient
from rclpy.qos import qos_profile_sensor_data


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Peer:
    def __init__(self):
        self.master, slave = os.openpty()
        self.path = os.ttyname(slave)
        os.close(slave)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.owner = 0
        self.armed = False
        self.leased = False
        self.job = 0
        self.operation = 0
        self.gripper = 0
        self.queries = 0
        self.phase = 0
        self.auto_complete = False
        self.drop_job_query = False
        self.reject_next_submit = False
        self.complete_before_cancel = False
        self.fault_on_heartbeat = False
        self.job_error = 0
        self.stop_error = 0
        self.disarmed_job_queries = 0
        self.cancel_stamp_ns = 0
        self.last_twist = (0.0, 0.0, 0.0)
        self.twist_during_job = 0
        self.twist_during_motion = 0
        self.submissions = []
        self.maintenance_calls = []
        self.opcodes = []
        self.errors = []
        try:
            self.thread.start()
        except BaseException:
            os.close(self.master)
            raise

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        os.close(self.master)

    def status(self):
        result = bytearray(84)
        result[1] = int(self.armed) | (4 if self.leased else 0)
        result[2] = 8
        struct.pack_into('<I', result, 4, self.owner)
        for index, velocity in enumerate(self.last_twist):
            struct.pack_into('<i', result, 40 + index * 4, int(velocity * 1000))
        result[76] = 4  # Commanded wheel zero is known; no measured wheel/pose claim.
        return result

    def job_status(self):
        result = bytearray(25)
        result[1:11] = bytes((self.phase, self.operation, self.gripper,
                              2 if self.phase == 2 else 0, 2, 2, 0,
                              2 if self.phase == 2 else 0, 0, 0))
        result[11:13] = bytes((self.job_error, self.stop_error))
        struct.pack_into('<III', result, 13, self.job, self.owner, self.queries * 50)
        return result

    def response(self, opcode, owner, payload):
        self.opcodes.append(opcode)
        if opcode == 1:
            self.owner = owner
            self.leased = True
            self.armed = False
        elif opcode == 2:
            self.armed = True
        elif opcode == 3 and self.fault_on_heartbeat and self.phase == 1 and self.queries >= 1:
            # Firmware faults/disarms after submit but retains the same lease for final query.
            self.fault_on_heartbeat = False
            self.phase = 4
            self.job_error = 9
            self.stop_error = 8
            self.armed = False
            self.last_twist = (0.0, 0.0, 0.0)
        elif opcode in (6, 7):
            self.armed = False
            self.leased = False
        elif opcode == 4:
            # This explicit test profile reserves pump channel 0 for PREPARE.
            result = self.status()
            if self.phase == 1 and struct.unpack_from('<I', payload)[0] & 1:
                result[0] = 5
            return result
        elif opcode == 0x20:
            # DC channel 0 overlaps this job; DC channel 1 is independent.
            result = self.status()
            if self.phase == 1 and payload[:2] == bytes((6, 0)):
                result[0] = 5
            return result
        elif opcode == 5:
            self.last_twist = struct.unpack('<fff', payload)
            if self.phase == 1:
                self.twist_during_job += 1
                if self.operation in (6, 8, 9, 12, 22):
                    self.twist_during_motion += 1
        elif opcode == 9:
            return bytes((10,))  # Valid unsupported sensor query; no stale battery.
        elif opcode == 0x24:
            assert len(payload) == 22
            operation = payload[0]
            self.maintenance_calls.append(operation)
            if operation == 0:
                return bytes((0, 1, int(self.armed), 0, 0)) + struct.pack('<III', 0x04000000, 1000, self.owner)
            if operation in (3, 4):
                return bytes(14)
            if operation == 7:
                return bytes(13)
            if operation in (13, 17, 19):
                return bytes(9)
            if operation == 27:
                return b'\x00test board diagnostics'
            return bytes((0,))
        elif opcode == 0x21:
            assert len(payload) == 22
            if self.reject_next_submit:
                self.reject_next_submit = False
                return bytes((5,))
            self.job += 1
            self.operation = payload[1]
            self.gripper = payload[0]
            self.queries = 0
            self.phase = 1
            self.job_error = 0
            self.stop_error = 0
            self.submissions.append(bytes(payload))
            return self.job_status()
        elif opcode in (0x22, 0x23):
            assert len(payload) == 4 and struct.unpack('<I', payload)[0] == self.job
            if self.drop_job_query:
                return None
            self.queries += 1
            if not self.armed:
                self.disarmed_job_queries += 1
            if opcode == 0x23:
                self.cancel_stamp_ns = time.time_ns() + 40000000
                if self.complete_before_cancel:
                    self.complete_before_cancel = False
                    self.phase = 2
                    result = self.job_status()
                    result[0] = 2  # Engine rejects cancelling an already-completed job.
                    return result
                self.phase = 3
                self.last_twist = (0.0, 0.0, 0.0)  # Cancel physically stops all resources.
            elif self.auto_complete and self.queries >= 3:
                self.phase = 2
            return self.job_status()
        return self.status()

    def run(self):
        pending = bytearray()
        try:
            while not self.stop.is_set():
                readable, _, _ = select.select([self.master], [], [], 0.02)
                if readable:
                    try:
                        chunk = os.read(self.master, 4096)
                    except OSError:
                        chunk = b''  # Slave not open yet or bridge reconnecting.
                        time.sleep(0.01)
                    pending.extend(chunk)
                parsing = True
                while parsing and len(pending) >= 16:
                    magic = pending.find(b'\xa5\x5a')
                    if magic < 0:
                        pending[:] = pending[-1:]
                        parsing = False
                    elif magic > 0:
                        del pending[:magic]
                    else:
                        length = struct.unpack_from('<H', pending, 12)[0]
                        if length > 128:
                            del pending[:2]
                        elif len(pending) < 16 + length:
                            parsing = False
                        else:
                            frame = bytes(pending[:16 + length])
                            del pending[:16 + length]
                            assert frame[2] == 1 and crc16(frame[2:-2]) == struct.unpack('<H', frame[-2:])[0]
                            opcode = frame[3]
                            sequence, owner = struct.unpack_from('<II', frame, 4)
                            answer = self.response(opcode, owner, frame[14:-2])
                            if answer is not None:
                                body = struct.pack('<BBIIH', 1, opcode | 0x80,
                                                   sequence, owner, len(answer)) + answer
                                packet = b'\xa5\x5a' + body + struct.pack('<H', crc16(body))
                                sent = 0
                                while sent < len(packet):
                                    sent += os.write(self.master, packet[sent:])
        except BaseException as error:
            self.errors.append(error)


def spin_until(node, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not predicate():
        rclpy.spin_once(node, timeout_sec=0.02)
    assert predicate(), 'bounded ROS test wait expired'


def future_result(node, future):
    spin_until(node, future.done)
    return future.result()


def stop_bridge(child):
    if child.poll() is None:
        child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=3)


def stop_ros():
    if rclpy.ok():
        rclpy.shutdown()


def test_gripper_action_session_cancel_completion_and_link_loss(tmp_path):
    executable = os.environ['BRIDGE_EXECUTABLE']
    assert Path(executable).is_file()
    with ExitStack() as resources:
        peer = Peer()
        resources.callback(peer.close)
        log = resources.enter_context((tmp_path / 'bridge.log').open('w'))
        child = subprocess.Popen([executable, '--ros-args', '-r', '__ns:=/eurobot_test',
                                  '-p', f'port:={peer.path}', '-p', 'ack_timeout_ms:=60',
                                  '-p', 'maintenance_enabled:=true'],
                                 stdout=log, stderr=subprocess.STDOUT)
        resources.callback(stop_bridge, child)
        resources.callback(stop_ros)
        rclpy.init()
        node = rclpy.create_node('gripper_test_client')
        resources.callback(node.destroy_node)
        states = []
        diagnostics = []
        node.create_subscription(DiagnosticArray, '/eurobot_test/diagnostics',
                                                   diagnostics.append, 10)
        node.create_subscription(ControllerState, '/eurobot_test/state',
                                             states.append, 10)
        publisher = node.create_publisher(TwistStamped, '/eurobot_test/cmd_vel',
                                          qos_profile_sensor_data)
        action = ActionClient(node, Gripper, '/eurobot_test/gripper')
        resources.callback(action.destroy)
        arm = node.create_client(Arm, '/eurobot_test/arm')
        outputs = node.create_client(SetOutputs, '/eurobot_test/set_outputs')
        actuator = node.create_client(SetActuator, '/eurobot_test/set_actuator')
        maintenance = node.create_client(Maintenance, '/eurobot_test/maintenance')
        disarm = node.create_client(Trigger, '/eurobot_test/disarm')
        assert action.wait_for_server(timeout_sec=8)
        assert arm.wait_for_service(timeout_sec=8)
        assert outputs.wait_for_service(timeout_sec=8)
        assert actuator.wait_for_service(timeout_sec=8)
        assert maintenance.wait_for_service(timeout_sec=8)
        assert disarm.wait_for_service(timeout_sec=8)
        spin_until(node, lambda: bool(states) and states[-1].connected and states[-1].state_valid)
        # Discovery must complete before a best-effort sample can be expected to arrive.
        spin_until(node, lambda: publisher.get_subscription_count() > 0)
        info = future_result(node, maintenance.call_async(Maintenance.Request(operation=Maintenance.Request.BOARD_INFO)))
        assert info.accepted and len(info.data) == 16 and info.data[0] == 1
        refused = future_result(node, maintenance.call_async(Maintenance.Request(operation=Maintenance.Request.CLEAR_RESET)))
        assert not refused.accepted and refused.error_code == 5
        claimed = future_result(node, arm.call_async(Arm.Request(control_token=0, maintenance_session=True)))
        assert claimed.accepted and claimed.control_token != 0
        maintenance_token = claimed.control_token
        spin_until(node, lambda: bool(states) and not states[-1].armed and
                   states[-1].lease_active and states[-1].owner == maintenance_token)
        cleared = future_result(node, maintenance.call_async(Maintenance.Request(
            control_token=maintenance_token, operation=Maintenance.Request.CLEAR_RESET)))
        assert cleared.accepted and 2 in peer.maintenance_calls
        forbidden_output = future_result(node, outputs.call_async(SetOutputs.Request(control_token=maintenance_token)))
        assert not forbidden_output.accepted and forbidden_output.error_code == 5
        transition = future_result(node, arm.call_async(Arm.Request(control_token=maintenance_token)))
        assert not transition.accepted  # Maintenance does not silently enable normal outputs.
        stopped = future_result(node, disarm.call_async(Trigger.Request()))
        assert stopped.success
        armed = future_result(node, arm.call_async(Arm.Request(control_token=0)))
        assert armed.accepted and armed.control_token != 0
        token = armed.control_token
        forbidden_reset = future_result(node, maintenance.call_async(Maintenance.Request(
            control_token=token, operation=Maintenance.Request.BOARD_RESET)))
        assert not forbidden_reset.accepted and forbidden_reset.error_code == 5

        requested_velocity = [0.1]
        motion_enabled = [True]

        def send_fresh_motion():
            if motion_enabled[0]:
                message = TwistStamped()
                message.header.frame_id = 'base_link'
                message.header.stamp = node.get_clock().now().to_msg()
                message.twist.linear.x = requested_velocity[0]
                publisher.publish(message)

        node.create_timer(0.05, send_fresh_motion)
        spin_until(node, lambda: peer.last_twist[0] > 0)

        def goal_for(control_token=None, operation=Gripper.Goal.PREPARE):
            return Gripper.Goal(control_token=token if control_token is None else control_token,
                                gripper=0, operation=operation, value=0.0,
                                argument=0.0, speed=0.0, mask=15, timeout_ms=5000)

        wrong = future_result(node, action.send_goal_async(goal_for(token ^ 1)))
        assert not wrong.accepted and not peer.submissions
        peer.reject_next_submit = True
        rejected_handle = future_result(node, action.send_goal_async(goal_for()))
        assert rejected_handle.accepted  # ROS reservation precedes bounded firmware submit.
        rejected = future_result(node, rejected_handle.get_result_async())
        assert rejected.status == GoalStatus.STATUS_ABORTED
        assert rejected.result.outcome_known and rejected.result.error_code == 5
        assert rejected.result.job_id == 0 and not peer.submissions
        spin_until(node, lambda: states[-1].armed and states[-1].owner == token)

        # A nonmotion plan can begin while an independent chassis stream is active.
        handle = future_result(node, action.send_goal_async(goal_for()))
        assert handle.accepted
        spin_until(node, lambda: len(peer.submissions) == 1)
        raw = peer.submissions[0]
        assert len(raw) == 22 and struct.unpack_from('<I', raw, 10)[0] == 5000
        other = future_result(node, action.send_goal_async(goal_for()))
        assert not other.accepted
        busy_maintenance = future_result(node, maintenance.call_async(Maintenance.Request(
            control_token=token, operation=Maintenance.Request.MATCH_DISARM)))
        assert not busy_maintenance.accepted and busy_maintenance.error_code == 5
        overlapping = future_result(node, outputs.call_async(SetOutputs.Request(
            control_token=token, pumps_select=1, pumps_values=1)))
        independent = future_result(node, outputs.call_async(SetOutputs.Request(
            control_token=token, pumps_select=128, pumps_values=128)))
        assert not overlapping.accepted and overlapping.error_code == 5
        assert independent.accepted
        actuator_overlap = future_result(node, actuator.call_async(SetActuator.Request(
            control_token=token, kind=SetActuator.Request.DC_DUTY, id=0, value=0.5, rate=0.0)))
        actuator_free = future_result(node, actuator.call_async(SetActuator.Request(
            control_token=token, kind=SetActuator.Request.DC_DUTY, id=1, value=0.5, rate=0.0)))
        assert not actuator_overlap.accepted and actuator_overlap.error_code == 5
        assert actuator_free.accepted
        spin_until(node, lambda: peer.twist_during_job > 0)
        assert states[-1].armed  # Resource conflict is not a session/transport failure.
        requested_velocity[0] = 0.0
        spin_until(node, lambda: peer.last_twist == (0.0, 0.0, 0.0))
        cancelled = future_result(node, handle.cancel_goal_async())
        assert cancelled.goals_canceling
        result = future_result(node, handle.get_result_async())
        assert result.status == GoalStatus.STATUS_CANCELED
        assert result.result.outcome_known and not result.result.completed
        assert not result.result.physically_verified and 0x23 in peer.opcodes

        # A packet created before cancellation may be 40ms ahead of the bridge clock.
        # It must not restart motion; later freshly stamped commands must still work.
        motion_enabled[0] = False
        old_motion = TwistStamped()
        old_motion.header.frame_id = 'base_link'
        old_motion.header.stamp.sec = peer.cancel_stamp_ns // 1000000000
        old_motion.header.stamp.nanosec = peer.cancel_stamp_ns % 1000000000
        old_motion.twist.linear.x = 0.2
        publisher.publish(old_motion)
        quiet_until = time.monotonic() + 0.08
        spin_until(node, lambda: time.monotonic() >= quiet_until, timeout=1)
        assert peer.last_twist == (0.0, 0.0, 0.0)
        requested_velocity[0] = 0.1
        motion_enabled[0] = True
        spin_until(node, lambda: peer.last_twist[0] > 0)
        assert states[-1].armed and states[-1].owner == token

        # A cancel races with successful MCU completion: report success, keep the
        # same lease and independent motion, and never falsely label it cancelled.
        peer.complete_before_cancel = True
        late_handle = future_result(node, action.send_goal_async(goal_for()))
        assert late_handle.accepted
        spin_until(node, lambda: peer.phase == 1 and peer.queries >= 1)
        previous_submits = len(peer.submissions)
        releases = peer.opcodes.count(7)
        assert future_result(node, late_handle.cancel_goal_async()).goals_canceling
        late_result = future_result(node, late_handle.get_result_async())
        assert late_result.status == GoalStatus.STATUS_SUCCEEDED
        assert late_result.result.outcome_known and late_result.result.completed
        assert late_result.result.error_code == 0 and not late_result.result.physically_verified
        assert peer.opcodes.count(7) == releases and peer.armed and peer.leased
        assert peer.owner == token and len(peer.submissions) == previous_submits
        spin_until(node, lambda: peer.last_twist[0] > 0)

        def restart_after_global_stop():
            nonlocal token
            motion_enabled[0] = False
            future_result(node, disarm.call_async(Trigger.Request()))
            # The next session must not accept a delayed packet from before the stop.
            drain_deadline = time.monotonic() + 0.25
            spin_until(node, lambda: time.monotonic() >= drain_deadline, timeout=1)
            acquired = future_result(node, arm.call_async(Arm.Request(control_token=0)))
            assert acquired.accepted and acquired.control_token != token
            token = acquired.control_token
            requested_velocity[0] = 0.0
            motion_enabled[0] = True

        restart_after_global_stop()
        peer.auto_complete = True
        handle = future_result(node, action.send_goal_async(goal_for()))
        assert handle.accepted
        result = future_result(node, handle.get_result_async())
        assert result.status == GoalStatus.STATUS_SUCCEEDED and result.result.completed
        assert result.result.outcome_known and not result.result.physically_verified
        assert result.result.completion_evidence == Gripper.Result.EVIDENCE_SETTLED

        # Motion plans still require an idle chassis and hold external Twist exclusively.
        requested_velocity[0] = 0.1
        spin_until(node, lambda: peer.last_twist[0] > 0)
        moving_goal = goal_for(operation=Gripper.Goal.TIMED_MOVE)
        moving_goal.value = 0.1
        moving_goal.argument = 500.0
        moving_goal.mask = 0
        assert not future_result(node, action.send_goal_async(moving_goal)).accepted
        requested_velocity[0] = 0.0
        spin_until(node, lambda: peer.last_twist == (0.0, 0.0, 0.0) and
                   states[-1].wheel_commanded_ticks_per_minute[0] == 0)
        peer.auto_complete = False
        motion_handle = future_result(node, action.send_goal_async(moving_goal))
        assert motion_handle.accepted
        spin_until(node, lambda: peer.phase == 1 and peer.operation == 6 and peer.queries >= 2)
        assert peer.twist_during_motion == 0
        assert future_result(node, motion_handle.cancel_goal_async()).goals_canceling
        motion_result = future_result(node, motion_handle.get_result_async())
        assert motion_result.status == GoalStatus.STATUS_CANCELED
        restart_after_global_stop()

        # A disarming MCU fault still permits one same-owner terminal status query,
        # but cannot renew the lease, execute commands or auto-arm from a later ACK.
        peer.fault_on_heartbeat = True
        fault_handle = future_result(node, action.send_goal_async(goal_for()))
        assert fault_handle.accepted
        fault_result = future_result(node, fault_handle.get_result_async())
        assert fault_result.status == GoalStatus.STATUS_ABORTED
        assert fault_result.result.outcome_known and fault_result.result.error_code == 9
        assert fault_result.result.stop_error == 8 and fault_result.result.phase == 4
        assert peer.disarmed_job_queries == 1 and not peer.leased and not peer.armed
        stale_outputs = future_result(node, outputs.call_async(SetOutputs.Request(
            control_token=token, pumps_select=128, pumps_values=128)))
        assert not stale_outputs.accepted
        assert not future_result(node, action.send_goal_async(goal_for())).accepted
        restart_after_global_stop()

        peer.auto_complete = False
        peer.drop_job_query = True
        handle = future_result(node, action.send_goal_async(goal_for()))
        assert handle.accepted
        result = future_result(node, handle.get_result_async())
        assert result.status == GoalStatus.STATUS_ABORTED
        assert not result.result.outcome_known and result.result.error_code == 9
        submissions = len(peer.submissions)
        spin_until(node, lambda: bool(states) and states[-1].connected and
                   states[-1].transport_faults >= 1 and not states[-1].armed)
        assert len(peer.submissions) == submissions  # No automatic replay after ambiguous ACK.
        stale = future_result(node, action.send_goal_async(goal_for()))
        assert not stale.accepted
        assert not peer.errors
        assert child.poll() is None
