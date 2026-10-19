# EUROBOT NEXT — результаты проверки

Статус: **READY FOR HARDWARE VALIDATION** для первого испытания логики/связи с отключёнными нагрузками. Диагностические presets не arm; новый stm32-robot2026 содержит настроенный по источникам рабочий профиль и требует явного arm. Main0 и связанные рецепты работают через112 с проверкой его положения; бывший слот254 неактивен. Физическая работа робота требует стендовой проверки.

Дата: 2026-10-19T04:24:39.655503+03:00 (Europe/Moscow). Итог свежего прогона: **PASS**.

[Машинный результат и журналы](../artifacts/validation/results.json). Source fingerprint фиксирует firmware, ROS, тесты, инструменты и примеры. Журналы/образы исключены из Git; они доступны в локальном проекте.

| Проверка | Результат | Журнал |
|---|---|---|
| format | PASS | [format.log](../artifacts/validation/format.log) |
| source_policy | PASS | [source_policy.log](../artifacts/validation/source_policy.log) |
| dependencies | PASS | [dependencies.log](../artifacts/validation/dependencies.log) |
| configure_host | PASS | [configure_host.log](../artifacts/validation/configure_host.log) |
| build_host | PASS | [build_host.log](../artifacts/validation/build_host.log) |
| test_host | PASS | [test_host.log](../artifacts/validation/test_host.log) |
| configure_host-sanitize | PASS | [configure_host-sanitize.log](../artifacts/validation/configure_host-sanitize.log) |
| build_host-sanitize | PASS | [build_host-sanitize.log](../artifacts/validation/build_host-sanitize.log) |
| test_host-sanitize | PASS | [test_host-sanitize.log](../artifacts/validation/test_host-sanitize.log) |
| core_unit_details | PASS | [core_unit_details.log](../artifacts/validation/core_unit_details.log) |
| device_unit_details | PASS | [device_unit_details.log](../artifacts/validation/device_unit_details.log) |
| gripper_unit_details | PASS | [gripper_unit_details.log](../artifacts/validation/gripper_unit_details.log) |
| application_unit_details | PASS | [application_unit_details.log](../artifacts/validation/application_unit_details.log) |
| runtime_unit_details | PASS | [runtime_unit_details.log](../artifacts/validation/runtime_unit_details.log) |
| static_analysis | PASS | [static_analysis.log](../artifacts/validation/static_analysis.log) |
| configure_stm32-debug | PASS | [configure_stm32-debug.log](../artifacts/validation/configure_stm32-debug.log) |
| build_stm32-debug | PASS | [build_stm32-debug.log](../artifacts/validation/build_stm32-debug.log) |
| verify_stm32-debug | PASS | [verify_stm32-debug.log](../artifacts/validation/verify_stm32-debug.log) |
| configure_stm32-release | PASS | [configure_stm32-release.log](../artifacts/validation/configure_stm32-release.log) |
| build_stm32-release | PASS | [build_stm32-release.log](../artifacts/validation/build_stm32-release.log) |
| verify_stm32-release | PASS | [verify_stm32-release.log](../artifacts/validation/verify_stm32-release.log) |
| configure_stm32-board8-plan | PASS | [configure_stm32-board8-plan.log](../artifacts/validation/configure_stm32-board8-plan.log) |
| build_stm32-board8-plan | PASS | [build_stm32-board8-plan.log](../artifacts/validation/build_stm32-board8-plan.log) |
| verify_stm32-board8-plan | PASS | [verify_stm32-board8-plan.log](../artifacts/validation/verify_stm32-board8-plan.log) |
| configure_stm32-robot2026 | PASS | [configure_stm32-robot2026.log](../artifacts/validation/configure_stm32-robot2026.log) |
| build_stm32-robot2026 | PASS | [build_stm32-robot2026.log](../artifacts/validation/build_stm32-robot2026.log) |
| verify_stm32-robot2026 | PASS | [verify_stm32-robot2026.log](../artifacts/validation/verify_stm32-robot2026.log) |
| linked_features | PASS | [linked_features.log](../artifacts/validation/linked_features.log) |
| configure_cxx26-probe | PASS | [configure_cxx26-probe.log](../artifacts/validation/configure_cxx26-probe.log) |
| build_cxx26-probe | PASS | [build_cxx26-probe.log](../artifacts/validation/build_cxx26-probe.log) |
| test_cxx26-probe | PASS | [test_cxx26-probe.log](../artifacts/validation/test_cxx26-probe.log) |
| configure_stm32-cxx26-probe | PASS | [configure_stm32-cxx26-probe.log](../artifacts/validation/configure_stm32-cxx26-probe.log) |
| build_stm32-cxx26-probe | PASS | [build_stm32-cxx26-probe.log](../artifacts/validation/build_stm32-cxx26-probe.log) |
| verify_stm32-cxx26-probe | PASS | [verify_stm32-cxx26-probe.log](../artifacts/validation/verify_stm32-cxx26-probe.log) |
| ros2_jazzy | PASS | [ros2_jazzy.log](../artifacts/validation/ros2_jazzy.log) |
| ros2_lyrical | PASS | [ros2_lyrical.log](../artifacts/validation/ros2_lyrical.log) |
| python_syntax | PASS | [python_syntax.log](../artifacts/validation/python_syntax.log) |
| source_integrity | PASS | [source_integrity.log](../artifacts/validation/source_integrity.log) |

## Образы STM32F407VG

Свежие чистые сборки создают ELF/HEX/BIN/MAP и manifest. Проверены регионы Flash1МиБ/SRAM128КиБ/CCM64КиБ, Thumb/Cortex-M4F hard-float, сильные обработчики RTOS/UART1/UART5/TIM1, отсутствие RWX LOAD, наличие реализаций основных подсистем и отсутствие linked heap/exception runtime. SRAM включает 4096 байт резерва MSP; это не измерение worst-case stack.

| Preset | BIN/Flash байт | SRAM байт | SHA256 BIN |
|---|---:|---:|---|
| stm32-debug | 144052 | 53272 | `44d75d7a3e8cbaaadff5e66b51fcc10360a67d9022e6f252792c4ec5a2c5829f` |
| stm32-release | 107176 | 53272 | `92ae3bbbfe3fd86c5960258c1bec2d933dc27e07b33bf564565911ce5c8311ae` |
| stm32-board8-plan | 105496 | 53280 | `2fb1897a2fe26d8d146e75c848255dad36d84c64e7086338ef75cd9a8400c186` |
| stm32-robot2026 | 106448 | 53280 | `55701a648c07f6ee9d9690439a48fd5488c4c8f213f6d11f4f7c363e7e4d6391` |
| stm32-cxx26-probe | 107176 | 53272 | `92ae3bbbfe3fd86c5960258c1bec2d933dc27e07b33bf564565911ce5c8311ae` |

## ROS 2

- jazzy: **PASS**, полный Linux ARM64 Docker build rosidl/interfaces, hardware bridge, bringup и colcon tests. Журнал: [ros2_jazzy.log](../artifacts/validation/ros2_jazzy.log).
- lyrical: **PASS**, полный Linux ARM64 Docker build rosidl/interfaces, hardware bridge, bringup и colcon tests. Журнал: [ros2_lyrical.log](../artifacts/validation/ros2_lyrical.log).

Тесты ROS используют настоящий node/graph и PTY, не физическую плату. Mac Docker проверяет сборку Linux; работу USB на Mac внутри контейнера этот результат не подтверждает. Ubuntu на отдельной машине и удалённый GitHub Actions не запускались.

## Реализованное поведение

Шасси: реальный Maxon protocol1, encoder/TPM feedback, кинематика2026 и вариант2025, одометрия, ограничения/разгон/торможение, PID-компонент, controlled stop и lease. Насосы/клапаны: маски8/16/32, независимые deadlines, max-on/max-active, физический SPI-GPO backend и сохранение NRST. Захваты: все активные2026 микросценарии, lift/slide/relative/timed/wheel moves, capture/release/prepare, отдельные2025 reset/cycle; bounded plans, feedback, отмена и допустимое независимое управление.

Драйверы: Dynamixel AX/MX protocol1, Maxon, BMS, PCA9685, L6474 с настоящим профилем разгона/торможения и finite homing, RC/PWM servo, generic PWM, DC/H-bridge/brake, SPI-GPO, WS2812B RGB/HSV до60, UART/RS485, GPIO/концевики, watchdog. Приложение:13 дебаунсированных входов, click/hold/events, cord/side/strategy/motor button, match99s, communication/reset. Maintenance0…36 доступен из ROS и Mac. Полный разбор отличий: [COMPARISON_2025_2026_NEXT.md](COMPARISON_2025_2026_NEXT.md), [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md).

## Аппаратные ограничения

Реальные SWD connect/read/flash, STM32 boot, UART робота и движения НЕ выполнялись. Default deployment намеренно не вооружается. Отдельный stm32-robot2026 автоматически заполняет известные параметры2026 и явные новые программные ограничения; он пригоден для последующих поэтапных испытаний с явным arm. Его параметры не объявляются аппаратными измерениями. До нагрузок сверяются штатное питание/MCU, проводка PO1…PO16, физический USART3/DIO и соответствие установленных servo source-карте. Main0 настроен на112; неизвестный второй адрес для его выполнения не требуется. Концевик switch2 задаёт ноль0, отрицательный seek−100000 не принимается за координату нуля. После снятия тока lift требуется новое home. Подробные источники, политики и исключения: [ROBOT_2026_PROFILE.md](ROBOT_2026_PROFILE.md). Для16/32 физических насосов с клапанами требуется подходящий расширенный output backend: известная исходная плата имеет20 силовых выходов; универсальные32-битные маски не добавляют выводы.

После конфигурации: питание логики без нагрузок → SWD identity/backup/flash/readback → ручной reset после halt → UART status/sensors/maintenance readonly → проверка safe boot и сигналов → по одному механизму с ограничением энергии → потеря связи/ESTOP → ROS. Подробно: [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md), [MACOS_BOARD_TEST.md](MACOS_BOARD_TEST.md), [FLASH_UBUNTU.md](FLASH_UBUNTU.md).

Аппаратно измерить: пины/полярности и временные диаграммы, SPI/L6474/PWM/DE/DMA, механические пределы, токи, worst-case stop/WCET/RTOS stacks и работу watchdog. Наличие ACK и software pulse count не доказывает физическое движение.

## Реестр и публикация

603 legacy-определения инвентаризированы с эквивалентом или явной причиной замены/неприменимости. Статусы не являются процентом готовности робота:
- BUILT: 235
- INTEGRATION_TESTED: 79
- NOT_APPLICABLE: 31
- UNIT_TESTED: 258

SHA256164 исходных справочных файлов проверены без изменения оригиналов. Локальный Git подготовлен. Публичная публикация не выполняется до установления прав на исходное поведение и выбора лицензии; сторонние notices сохранены. См. [PUBLICATION.md](PUBLICATION.md).

Ни host tests, ни ARM link, ни ROS tests не являются аппаратной валидацией. Основной язык C++23; C++26 probe подтверждает только реально собранные конструкции конкретных компиляторов.

## Карта разъёмов владельца

Распиновка GPIO/PWM/Stepper и24 бит силового регистра централизована в board_connectors.hpp и используется реальным STM32 adapter. Новый eight_pump_installation()/examples/board_8_pumps.hpp назначает8 насосов и8 клапанов на PO1…PO16, захваты используют разные логические группы0…3/4…7; PO17…20 свободны. Профиль остаётся uncommissioned до аппаратной настройки. Проверяются все13 входов, PWM bank order, partition20PO+4NRST, включение новых каналов и сохранение reset. Таблица подключения: [BOARD_WIRING.md](BOARD_WIRING.md).

## Дополнительный анализ ELF/MAP

В обоих основных ELF проверены присутствующие реализации FrameParser/Dispatcher/Controller, PumpBank/Chassis/кинематики, Gripper Engine/Application, Maxon/Dynamixel/BMS/PCA9685/L6474/StepperPlanner, аппаратные serial/SPI/PWM/DC/LED/UART5, Control/Comms/Watchdog и maintenance/job handlers. Подробности символов: [elf-feature-audit.json](../artifacts/elf-feature-audit.json). Секции устройств не подменены host fakes. Неиспользуемые библиотечные utility (например, Pid и HSV conversion) могут быть удалены linker garbage collection: PID колёс находится во внешних Maxon-платах; встроенная LED-команда принимает RGB, а HSV converter доступен в проверенной C++ библиотеке. Это не потеря активного legacy PID/HSV — такого работающего STM32 поведения в исходниках не найдено.

При заключительном review исправлены ограничение хода обоих вариантов homing; проверка локальной input/match policy до arming; восстановление ALARM_EN/TOFF_MIN L6474 после NRST; общий stop при частичной ошибке записывающего maintenance; конфликт повторно назначаемых живых Maxon ID; поздний CANCEL с завершённым job; старые Twist с допустимым future stamp; получение terminal error/stop_error после автономного fault MCU. На эти случаи добавлены регрессии, включённые в свежий общий прогон выше.
