#!/usr/bin/env python3
"""Render only fresh saved validation evidence, never infer hardware results."""
import collections
import csv
import json
import re
from pathlib import Path
from source_fingerprint import validation_fingerprint

root = Path(__file__).resolve().parents[1]
logs = root / 'artifacts/validation'
data = json.loads((logs / 'results.json').read_text())
if not data.get('sources_unchanged') or data.get('verification_fingerprint') != validation_fingerprint(root):
    raise SystemExit('Saved validation belongs to changed sources; run tools/validate.py again.')
rows = list(csv.DictReader((root / 'docs/legacy_functions.csv').open()))
counts = collections.Counter(row['status'] for row in rows)
text = ['# EUROBOT NEXT — результаты проверки', '',
        'Статус: **BLOCKED BY MISSING HARDWARE INFORMATION** для вооружённой конфигурации робота. Программные проверки ниже завершены; безопасный диагностический образ не разрешает исполнительные выходы без commissioned deployment.', '',
        f"Дата: {data['timestamp']} (Europe/Moscow). Итог свежего прогона: **{'PASS' if data['success'] else 'FAIL'}**.", '',
        '[Машинный результат и журналы](../artifacts/validation/results.json). Source fingerprint фиксирует firmware, ROS, тесты, инструменты и примеры. Журналы/образы исключены из Git; они доступны в локальном проекте.', '',
        '| Проверка | Результат | Журнал |', '|---|---|---|']
for step in data['steps']:
    text.append(f"| {step['name']} | {'PASS' if step['returncode'] == 0 else 'FAIL'} | [{step['log']}](../artifacts/validation/{step['log']}) |")
text += ['', '## Образы STM32F407VG', '',
         'Свежие чистые сборки создают ELF/HEX/BIN/MAP и manifest. Проверены регионы Flash1МиБ/SRAM128КиБ/CCM64КиБ, Thumb/Cortex-M4F hard-float, сильные обработчики RTOS/UART1/UART5/TIM1, отсутствие RWX LOAD, наличие реализаций основных подсистем и отсутствие linked heap/exception runtime. SRAM включает 4096 байт резерва MSP; это не измерение worst-case stack.', '',
         '| Preset | BIN/Flash байт | SRAM байт | SHA256 BIN |', '|---|---:|---:|---|']
for preset, image in data['images'].items():
    log = logs / ('build_' + preset + '.log')
    content = log.read_text() if log.is_file() else ''
    ram = re.search(r'SRAM with MSP reservation (\d+)/', content)
    text.append(f"| {preset} | {image['image_size']} | {ram.group(1) if ram else 'см. MAP'} | `{image['sha256']}` |")
text += ['', '## ROS 2', '']
for distro, result in data.get('ros2_colcon', {}).items():
    text.append(f'- {distro}: **{result}**, полный Linux ARM64 Docker build rosidl/interfaces, hardware bridge, bringup и colcon tests. Журнал: [ros2_{distro}.log](../artifacts/validation/ros2_{distro}.log).')
text += ['', 'Тесты ROS используют настоящий node/graph и PTY, не физическую плату. Mac Docker проверяет сборку Linux; работу USB на Mac внутри контейнера этот результат не подтверждает. Ubuntu на отдельной машине и удалённый GitHub Actions не запускались.', '',
         '## Реализованное поведение', '',
         'Шасси: реальный Maxon protocol1, encoder/TPM feedback, кинематика2026 и вариант2025, одометрия, ограничения/разгон/торможение, PID-компонент, controlled stop и lease. Насосы/клапаны: маски8/16/32, независимые deadlines, max-on/max-active, физический SPI-GPO backend и сохранение NRST. Захваты: все активные2026 микросценарии, lift/slide/relative/timed/wheel moves, capture/release/prepare, отдельные2025 reset/cycle; bounded plans, feedback, отмена и допустимое независимое управление.', '',
         'Драйверы: Dynamixel AX/MX protocol1, Maxon, BMS, PCA9685, L6474 с настоящим профилем разгона/торможения и finite homing, RC/PWM servo, generic PWM, DC/H-bridge/brake, SPI-GPO, WS2812B RGB/HSV до60, UART/RS485, GPIO/концевики, watchdog. Приложение:13 дебаунсированных входов, click/hold/events, cord/side/strategy/motor button, match99s, communication/reset. Maintenance0…35 доступен из ROS и Mac. Полный разбор отличий: [COMPARISON_2025_2026_NEXT.md](COMPARISON_2025_2026_NEXT.md), [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md).', '',
         '## Аппаратные ограничения', '',
         'Реальные SWD connect/read/flash, STM32 boot, UART робота и движения НЕ выполнялись. Default deployment намеренно не вооружается. До испытания нагрузок нужно подтвердить MCU/HSE, исполнение новой разводки PO1…PO16 и мощность, клапаны/полярности, физическое подключение USART3/DIO, индивидуальный servo вместо broadcastID254, конфликтующие legacy IDs, реальные ограничения механизмов/колёс, homing/ESTOP и параметры силовых драйверов. Для16/32 физических насосов с клапанами требуется подходящий расширенный output backend: известная исходная плата имеет20 силовых выходов; универсальные32-битные маски не добавляют выводы.', '',
         'После конфигурации: питание логики без нагрузок → SWD identity/backup/flash/readback → ручной reset после halt → UART status/sensors/maintenance readonly → проверка safe boot и сигналов → по одному механизму с ограничением энергии → потеря связи/ESTOP → ROS. Подробно: [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md), [MACOS_BOARD_TEST.md](MACOS_BOARD_TEST.md), [FLASH_UBUNTU.md](FLASH_UBUNTU.md).', '',
         'Аппаратно измерить: пины/полярности и временные диаграммы, SPI/L6474/PWM/DE/DMA, механические пределы, токи, worst-case stop/WCET/RTOS stacks и работу watchdog. Наличие ACK и software pulse count не доказывает физическое движение.', '',
         '## Реестр и публикация', '',
         f'{len(rows)} legacy-определения инвентаризированы с эквивалентом или явной причиной замены/неприменимости. Статусы не являются процентом готовности робота:']
for status, count in sorted(counts.items()):
    text.append(f'- {status}: {count}')
text += ['', 'SHA256164 исходных справочных файлов проверены без изменения оригиналов. Локальный Git подготовлен. Публичная публикация не выполняется до установления прав на исходное поведение и выбора лицензии; сторонние notices сохранены. См. [PUBLICATION.md](PUBLICATION.md).', '',
         'Ни host tests, ни ARM link, ни ROS tests не являются аппаратной валидацией. Основной язык C++23; C++26 probe подтверждает только реально собранные конструкции конкретных компиляторов.']
text += ["", '## Карта разъёмов владельца\n\nРаспиновка GPIO/PWM/Stepper и24 бит силового регистра централизована в board_connectors.hpp и используется реальным STM32 adapter. Новый eight_pump_installation()/examples/board_8_pumps.hpp назначает8 насосов и8 клапанов на PO1…PO16, захваты используют разные логические группы0…3/4…7; PO17…20 свободны. Профиль остаётся uncommissioned до аппаратной настройки. Проверяются все13 входов, PWM bank order, partition20PO+4NRST, включение новых каналов и сохранение reset. Таблица подключения: [BOARD_WIRING.md](BOARD_WIRING.md).\n\n' + '## Дополнительный анализ ELF/MAP\n\nВ обоих основных ELF проверены присутствующие реализации FrameParser/Dispatcher/Controller, PumpBank/Chassis/кинематики, Gripper Engine/Application, Maxon/Dynamixel/BMS/PCA9685/L6474/StepperPlanner, аппаратные serial/SPI/PWM/DC/LED/UART5, Control/Comms/Watchdog и maintenance/job handlers. Подробности символов: [elf-feature-audit.json](../artifacts/elf-feature-audit.json). Секции устройств не подменены host fakes. Неиспользуемые библиотечные utility (например, Pid и HSV conversion) могут быть удалены linker garbage collection: PID колёс находится во внешних Maxon-платах; встроенная LED-команда принимает RGB, а HSV converter доступен в проверенной C++ библиотеке. Это не потеря активного legacy PID/HSV — такого работающего STM32 поведения в исходниках не найдено.\n\nПри заключительном review исправлены ограничение хода обоих вариантов homing; проверка локальной input/match policy до arming; восстановление ALARM_EN/TOFF_MIN L6474 после NRST; общий stop при частичной ошибке записывающего maintenance; конфликт повторно назначаемых живых Maxon ID; поздний CANCEL с завершённым job; старые Twist с допустимым future stamp; получение terminal error/stop_error после автономного fault MCU. На эти случаи добавлены регрессии, включённые в свежий общий прогон выше.\n']
(root / 'docs/VALIDATION_REPORT.md').write_text('\n'.join(text) + '\n')
print('Saved docs/VALIDATION_REPORT.md from fresh actual results')
