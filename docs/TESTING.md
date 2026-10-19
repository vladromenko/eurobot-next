# Проверки и границы подтверждения

Дата протокола:19 октября2026. Итог текущей интеграции, команды и журналы фиксируются в [VALIDATION_REPORT.md](VALIDATION_REPORT.md). Здесь различаются проверки алгоритмов на host, ARM compilation/link, сборка ROS и физическое оборудование. Написанный тест, CI workflow или ELF не доказывает выполнение на роботе.

## Текущая редакция

Итоговая проверка текущих исходников завершена успешно19.10.2026. Журнал [results.json](../artifacts/validation/results.json) фиксирует `success=true` и `sources_unchanged=true`. Верификационный fingerprint: `b7e41e28f5971b6b6b0555966a72579108fb14cf735a0d6dd5ac7139f969f8e1`. Старые34/34 и старые размеры образов не используются как подтверждение этого среза.

| Проверка | Фактический результат | Журнал |
|---|---|---|
| Host C++23 |73/73 CTest PASS|[host](../artifacts/validation/test_host.log)|
| Host C++23 ASan/UBSan |73/73 CTest PASS|[sanitizers](../artifacts/validation/test_host-sanitize.log)|
| Host C++26 experiment |73/73 CTest PASS|[C++26](../artifacts/validation/test_cxx26-probe.log)|
| Core / devices / gripper / application / runtime |1427 /7931 /282 /2672 /145 checks PASS|[core](../artifacts/validation/core_unit_details.log), [devices](../artifacts/validation/device_unit_details.log), [gripper](../artifacts/validation/gripper_unit_details.log), [application](../artifacts/validation/application_unit_details.log), [runtime](../artifacts/validation/runtime_unit_details.log)|
| STM32 debug |ELF/HEX/BIN/MAP/manifest PASS; Flash143316B, SRAM53240B, CCM0|[build](../artifacts/validation/build_stm32-debug.log)|
| STM32 release |ELF/HEX/BIN/MAP/manifest PASS; Flash107464B, SRAM53240B, CCM0|[build](../artifacts/validation/build_stm32-release.log)|
| STM32 C++26 experiment |Полная сборка/офлайн verify PASS; Flash107464B, SRAM53240B, CCM0|[build](../artifacts/validation/build_stm32-cxx26-probe.log)|
| STM32 board8 plan |Отдельный новый deployment8+8: ELF/HEX/BIN/MAP/manifest и linked audit PASS; Flash105768B, SRAM53248B, CCM0|[build](../artifacts/validation/build_stm32-board8-plan.log)|
| STM32 robot2026 |Заполненный source-профиль и новые явные software policies: ELF/HEX/BIN/MAP/manifest/linked audit PASS; Flash106720B, SRAM53248B|[build](../artifacts/validation/build_stm32-robot2026.log)|
| ROS2 Jazzy Docker Linux |3 packages build/test;3tests,0errors/0failures/0skipped|[colcon](../artifacts/validation/ros2_jazzy.log)|
| ROS2 Lyrical Docker Linux |3 packages build/test;3tests,0errors/0failures/0skipped|[colcon](../artifacts/validation/ros2_lyrical.log)|
| Clang static analysis |18 translation units PASS|[analysis](../artifacts/validation/static_analysis.log)|
| Format / source policy / pinned dependencies |PASS|[format](../artifacts/validation/format.log), [policy](../artifacts/validation/source_policy.log), [dependencies](../artifacts/validation/dependencies.log)|
| Оригинальные source files |164/164 SHA256 совпадают, vendor notices сохранены|[audit](../artifacts/validation/source_integrity.log)|

В73 CTest входят63 GoogleTests:18 deployment/profile,11 inputs,3 sensors,3 gripper wire,9 maintenance wire,9 board connectors/installation,10 source robot2026; остальные — отдельные harness/инструменты. Counts1427/7931/282/2672/145 — утверждения внутри программ, не число физических сценариев. Mac client28/28 local PTY/mock и flash-tool24/24 mocks входят в общую проверку; реальный UART/SWD не открывался. Reference header отдельно прошёл строгую C++23 syntax check и остаётся uncommissioned.

Все пять ARM ELF прошли hard-float/vector/feature/no-heap/no-exception/no-RWX проверки и согласование manifest. SRAM totals включают4096B резерва MSP; статическая map не измеряет stack high-water/WCET. Release ELF: text103140B, data4316B, bss48920B. Образ SHA256 debug: `0c80e2ba88d22aabd00ddb9563fd5a5dbbfc123769b3c52a74ada780120a65c3`; release/C++26: `e4ebc2bd329e53039357d9bb7f8e3b12a26a3a81859ea20a84754466b163fe65`. Изменение firmware/build inputs или deployment требует новой сборки и проверки fingerprint.

**Программный результат: PASS. Статус первого испытания логики/связи с отключёнными нагрузками: READY FOR HARDWARE VALIDATION.** Диагностические presets остаются uncommissioned; отдельный stm32-robot2026 уже содержит заполненный по существующим данным профиль с явными новыми software policies, требует live owner/arm и не включает движение на boot. Только main0-рецепты, использующие неизвестный индивидуальныйID вместо254, отвергаются до любых выходов. Сборка не является аппаратной приёмкой и не превращает протокольные domains в измеренные механические упоры. [Подробные параметры](ROBOT_2026_PROFILE.md).

## Что проверяется программно

| Область | Существенные сценарии |
|---|---|
| PumpBank |8/16/32 logical channels, masks, max_active, requested/commanded/measured, one latch, backend failure, отдельный deadline выбранного канала|
| Authority |Owner/sequence/replay/conflict, lease, explicit arm, ESTOP, stop errors, reconnect без возобновления|
| Kinematics / chassis |Обе геометрии2025/2026 и знаки, TPM/метры/радианы, norm/acceleration/wheel limits, invalid encoder feedback, неизвестная pose|
| Device protocols |Dynamixel checksums/status/errors/registers32 vs38, независимые BMS reads, Maxon registers, PCA/PWM/DC bounds, L6474 current quantization/reset/direction|
| Stepper |Source19200Hz profile/edge table/ramp, distance-adaptive motion, relative overflow, bounded physical-switch homing, timeout/disable/stop|
| Application |13 inputs, debounce/hold/init/click/stale, wraparound, strategy0…2, cord/motor/authority guards,99s match, reset pending/stop failures, event overflow, явная2025 motor-button policy|
| Gripper |Bounded plans,2026 cases,2025 unique ops, capture/release/prepare, timed chassis movement, paired feedback, cancellation/deadlines, evidence categories и resource ownership|
| Framing / runtime |CRC/version/length/bounds/noise, malformed payloads, exact ACK snapshots, command/lease expiry, finite jobs, named maintenance|
| Transport |PTY status/sensors/actuator/maintenance, matched seq/owner/ACK, no replay/reconnect, reset ACK ambiguity, claim-only mutators|
| LED |Конфигурируемое число до60 pixels, frame bounds и waveform algorithm; электрический waveform и DMA timing требуют стенда|

PumpBank refresh другого канала не продлевает прежний канал. Finite plan удерживает только явно перечисленные им ресурсы, включая уже активные, которые понадобятся позже. Это не включает выключенные нагрузки и не удерживает посторонние. Heartbeat owner нужен отдельно. После job обычный250ms deadline снова требует свежей команды; необходимое сквозное удержание задаётся явным composite.

## Воспроизводимые проверки

```sh
cmake --preset host
cmake --build --preset host
ctest --preset host --output-on-failure
cmake --preset host-sanitize
cmake --build --preset host-sanitize
ctest --preset host-sanitize --output-on-failure
cmake --preset stm32-debug
cmake --build --preset stm32-debug
cmake --preset stm32-release
cmake --build --preset stm32-release
python3 tools/audit_sources.py --legacy-root ..
```

Зависимости и отдельный deployment: [INSTALL.md](INSTALL.md), [CMAKE_BUILD.md](CMAKE_BUILD.md). C++26 presets являются дополнительным экспериментом, основной стандартC++23. Их успех подтверждает использованный набор возможностей и режим конкретного компилятора, не всю будущую стандартную библиотеку. Образы проверяются офлайн по vectors/ABI/LOAD segments/manifest/source fingerprint. Vendor Reset_Handler может оставаться weak; реально используемые IRQ/RTOS handlers должны быть strong. RAM totals включают резерв основного стека4096bytes; статическая map не измеряет stack high-water илиWCET.

Полный ROS2 colcon build/test имеет отдельный результат; PTY harness его не заменяет. Инструкция: [ROS2_BUILD.md](ROS2_BUILD.md). Установленный Docker CLI не доказывает доступность daemon или USB. macOS host build не доказывает Ubuntu native execution или запущенный CI.

## Аппаратные испытания — не выполнены

| Испытание | Критерий |
|---|---|
| MCU / clocks |МаркировкаF407VG, Flash-size, HSE16MHz, SYSCLK168MHz, UART/I2C/PWM clocks|
| Safe boot/reset/brownout |Нет drive/pump/servo torque/motion или automatic homing до explicit arm; подтверждённое reset state|
| SWD / Flash |Реальный probe/target, backup readability, erase/program/independent readback; адрес и отсутствие custom bootloader|
| SPI outputs |Реальный byte order/latch, mask0 без all-on, сохранение L6474 NRST bits|
| Pumps / valves |Полярность, ток запуска, питание, max_active, срок команды и реальные датчики при наличии|
| Wheels / encoders |ID/знаки, ticks/rev, unitsTPM, XYZ, limits и calibration|
| Servo / PCA / stepper |Модель/ID вместо254, ranges, feedback, физические hard limits, homing polarity/deadline/abort, удержание груза|
| UART5 / shared owner |Проверенная проводкаPC12/PD2,115200 и отсутствие второго независимого owner|
| LED |Реальные60 LEDs/питание, TIM8/DMA timings, reset gap и отказ DMA|
| Faults / safety latency |UART/I2C/SPI failures, остановка всех механизмов даже при одном отказавшем backend; worst-case latency|
| RTOS / watchdog |Стек каждой задачи,WCET Control10ms/Comms, starvation, watchdog при остановке critical tasks|
| ROS с роботом |Fresh cmd_vel, explicit rearm, потеря node/кабеля, reconnect без автовозобновления|
| Emergency isolation |Снятие энергии независимо от MCU/ROS/UART, удержание/безопасный сброс груза|

Плата не прошивалась, исполнительные механизмы не запускались. Выполненная ранее read-only macOS USB inventory не нашла ST-LINK/ST vendor candidates в полученном JSON; это не доказывает физическое отсутствие платы или доступность Flash. CubeProgrammer ST-LINK listing запускался:0 probes. Реальный install_robot.py flash пересобрал/проверил source-образ и остановился до target connect с No ST-LINK serial number found. Backup/read/write не выполнялись. Журнал: [flash-attempt.log](../artifacts/flash-attempt.log). Все HIL статусы остаются открытыми независимо от программных результатов.

Реестр [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md)/[legacy_functions.csv](legacy_functions.csv) охватывает603 определения legacy; disabled examples, пустые функции и заменённая внутренняя glue logic выделяются отдельно. HARDWARE_TESTED требует протокола с версией платы/механики, firmware fingerprint, deployment, питанием, условиями, измерениями и оператором.

Аудит SHA256 оригиналов/прямых копий/notice/явных секретов не является юридическим разрешением или гарантией отсутствия всех секретов. Инструкции подключения, самостоятельной прошивки и последующих тестов: [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md), [MACOS_BOARD_TEST.md](MACOS_BOARD_TEST.md), [MAINTENANCE.md](MAINTENANCE.md). Их команды не выполняются установкой автоматически.

## Проверки карты платы

Девять новых GoogleTests проверяют28 GPIO,13 input aliases, PWM1…4 TIM4 CH4…1, нумерацию Stepper1 снизу,20 power bits +4 NRST и их непересечение, все8 пар нового профиля, физические биты каналов5…8 и OFF с сохранением NRST, независимость групп захватов0…3/4…7, запрет arm при неизвестных параметрах. Реальный adapter использует эти же таблицы из board_connectors.hpp. Новый preset stm32-board8-plan проверяет компиляцию настоящего make_deployment, а не только portable тест.

Инвентаризация linked модулей воспроизводится tools/audit_elf_features.py и включена в полный validate. Она проверяет Debug, Release и board8-plan, пишет hashes ELF/MAP и списки символов в [elf-feature-audit.json](../artifacts/elf-feature-audit.json), завершает проверку ошибкой при отсутствии требуемой группы или найденном heap/exception/fake symbol.

## Рабочий source-профиль и установка

10 GoogleTests проверяют фактические байты L6474, source-настройки9AX/12PCA,нольhome0 и диапазонwireheight,4-насосный softwarecap при8 независимых routes, derivedwheelbounds на360 направлениях и±angularcap,неоднозначные/конфликтующиеID и отсутствие записей main0. В отдельном установочном harness13tests проверяют отсутствие flash в defaultbuild, остановку после ошибки configure, передачу безопасному backup/readbackloader, исключение broadcast/collidingIDs, сохранение localbinding,read-only inventory,продолжение только после явного device-timeoutNACK и abort при serial/malformed/busy. Все обращения к USB/плате в этих тестах подставные. Runtime145checks и Mac28tests включают новый typed servo_probe36.

Полный валидатор запускает ROSдистрибутивы последовательно. При повторной проверке Docker использовал готовый слой сборки для неизменившихся ROS-источников; затем обе colcontest suites реально выполнены в новых контейнерах: [Jazzy](../artifacts/validation/ros2_jazzy_rerun.log), [Lyrical](../artifacts/validation/ros2_lyrical_sequential.log), по3tests/0failures. Одна дополнительная попытка параллельного запуска Lyrical завершилась testfailure; при последовательном запуске все3PASS. Оба fixture используют ROS_DOMAIN_ID173; взаимное влияние не инструментировалось, поэтому причина не выдаётся за доказанную. Сохраняется рекомендованный последовательный запуск. Эта попытка и последующие результаты зафиксированы в results.json; её журнал не удалён.
