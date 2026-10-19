#!/usr/bin/env python3
"""Record actual linked firmware modules without contacting hardware."""
import hashlib
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
groups = {
    'framing_authority': ('wire::Dispatcher::dispatch', 'wire::FrameParser::push', 'Controller::'),
    'pump_chassis': ('PumpBank::', 'Chassis::', 'OmniKinematics::'),
    'gripper_application': ('gripper::Engine::', 'application::Application::'),
    'peripheral_protocols': ('DxlProtocol1::', 'Maxon::', 'DynamixelServo::', 'read_battery',
                            'Pca9685::', 'L6474::', 'StepperPlanner::'),
    'hardware_io': ('board::serial_exchange', 'board::shift_write', 'board::pwm_pulse',
                    'board::dc_drive', 'board::dc_brake', 'board::leds_write',
                    'board::debug_write', 'USART1_IRQHandler', 'UART5_IRQHandler',
                    'TIM1_UP_TIM10_IRQHandler'),
    'runtime': ('Hardware::maintenance_request', 'Hardware::job_request', 'Hardware::actuator',
                'control_task', 'comms_task', 'watchdog_task'),
}
report = {}
failed = False
for preset in ('stm32-debug', 'stm32-release', 'stm32-board8-plan'):
    directory = root / 'build' / preset
    cache = (directory / 'CMakeCache.txt').read_text()
    match = re.search(r'^CMAKE_NM:FILEPATH=(.+)$', cache, re.M)
    if not match:
        raise SystemExit('CMAKE_NM missing in ' + preset)
    elf = directory / 'firmware/eurobot_next.elf'
    map_file = directory / 'firmware/eurobot_next.map'
    symbols = subprocess.check_output([match[1], '-C', '-S', '--defined-only', str(elf)], text=True).splitlines()
    found = {name: {term: [line for line in symbols if term in line] for term in terms}
             for name, terms in groups.items()}
    missing = [term for terms in found.values() for term, lines in terms.items() if not lines]
    forbidden = [line for line in symbols if re.search(
        r'\b(malloc|calloc|realloc|free|pvPortMalloc|vPortFree|__cxa_throw|__cxa_allocate_exception)\b|operator new|Fake|fake_', line)]
    report[preset] = {'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                      'map_sha256': hashlib.sha256(map_file.read_bytes()).hexdigest(),
                      'groups': found, 'missing': missing,
                      'forbidden_allocation_or_test_symbols': forbidden}
    failed = failed or bool(missing or forbidden)
    print(preset + ': ' + ('FAIL' if missing or forbidden else 'linked feature audit PASS'))
output = root / 'artifacts/elf-feature-audit.json'
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2) + '\n')
raise SystemExit(1 if failed else 0)
