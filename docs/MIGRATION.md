# Миграция полезной функциональности

Новый интерфейс единый: ROS 2 bridge → кадры wire → Controller с владельцем, sequence, lease и arming → прикладной компонент → один аппаратный драйвер. Старые команды 0x0F/0x21 и строковые команды high-level не сохраняются ради совместимости. Любое устройство должно получать команды через тот же safety gate. Если extension не связан с реальным драйвером, он возвращает unsupported.

| Старая область | Новый компонент / решение | Сохранённое поведение и ограничения |
|---|---|---|
| SPI_GPO_Set/ResetOutputPin | Deployment::output_word / board::shift_write и один владелец SPI1 | Слово 24, порядок байтов, сохранение reset bits; изменение pump/valve за одну защёлку |
| TogglePumps / Gripper_SetPumps | PumpBank::set, Controller::pumps | Маски select/value, независимо до 32 каналов, группы, timeout; исключён legacy all-on glitch |
| ToggleValves / Gripper_SetValves | Тот же PumpBank и та же output transaction | Одно согласованное изменение pump+valve; физическая полярность требует проверки |
| WaitForGripperCompletion | Wire transaction sequence, bounded ACK deadline | Отдельно accepted/error/unknown; timeout не равен успеху |
| MatrixInit / SpeedTo_TPM / TPM_ToSpeed | OmniKinematics, legacy_2025_geometry, legacy_2026_geometry | Обе геометрии, порядок/знаки, calibration coefficients; проверка finite/range |
| Odometry.UpdatePath / SetCoord | Chassis::feedback/reset_pose | Encoder increments, midpoint integration, reference reset; feedback_known отдельно от command |
| Maxon.Set/Get_TPM/TotalTicks/Enable | Maxon и DxlProtocol1 transactions | Протокол внешних плат, ticks/min; PID внешних плат неизвестен |
| MaxonsStop/Disable, StopAllActuators | Controller::emergency_stop/tick | Потеря lease/связи вызывает stop; latch estop и explicit rearm |
| Dynamixel AX/MX | DxlProtocol1 / servo register access | Angle/torque/mode/speed/feedback/LED/ID; физическая модель и ID задаются явно |
| RC_Servo/PWM/H_Bridge | servo_pulse / board PWM / HBridge | Общая frequency group, pulse/duty limits, исключение конфликтующих назначений |
| PCA9685 | Pca9685 через bounded I2C transaction |16 каналов, frequency/pulse/angle/full-off/sleep/wakeup; ACK не подтверждает угол |
| L6474 / Stepper | L6474 register driver и bounded step planner | Current/microstep/status, направление/число импульсов; homing и TIM1 binding отдельно |
| BMS | read_battery/decode_battery | U/I/P/SOC/cells; calibration writes отдельны и не вызываются на boot |
| WS2812B RGB | Кодирование PWM/DMA | Timing и размер буфера; HSV legacy пустой |
| ButtonsPoll/Callback | Фильтрация входов и snapshot/events под одним владельцем | Application:13inputs/debounce/click/hold/strategy/cord и99s match с explicit authority |
| HL_RTU/FIFO/Serial | wire::FrameParser/Dispatcher и bounded UART | Versioned CRC16 protocol; malformed/oversize/timeout/replay rejection |
| Main/clock/startup/IRQ | C++ board/startup и FreeRTOS | F407VG, memory layout, FPU/ABI, безопасный старт |
| Game/Demo/GripperTask sequences | Application и gripper::Engine; source-empty/inactive Demo ветки явно описаны в реестре | Не заменяются одним servo driver; нет автоматического перемещения на boot |
| ROS communication string API | Typed ROS 2 interfaces | cmd_vel, output masks, actuator service, state/diagnostics, explicit arming |

Таблица показывает распределение ответственности; доказанный статус каждой функции и результаты проверок находятся в [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md). Упоминание компонента не означает, что все режимы старой функции перенесены. Реестр включает конкретные пробелы, а не скрывает их под названием нового класса.

## Ввод в эксплуатацию

1. Сохранить и проверить исходную прошивку/настройки, когда устройство доступно для чтения. Не обходить read protection.
2. Сверить плату и PINOUT.md; для новой разводки8+8 использовать examples/board_8_pumps.hpp и собрать PO1…PO16 по BOARD_WIRING; заполнить модели/ID servo, полярности клапанов, пределы шасси и токи приводов.
3. Снять питание нагрузок, проверить сборку, векторы и память; проанализировать SPI/UART/PWM/I2C.
4. После отдельного разрешения владельца прошить через ST-LINK с readback verification; option bytes/RDP не менять.
5. Проверить каждый канал и отказы; измерить реальную задержку останова при watchdog/потере связи.
6. Испытать реализованные homing/gripper/relative-motion/match sequences по одному после заполнения reviewed профиля.
7. Проверить ROS 2 bridge с физическим контроллером, потерей связи и reconnect без автоматического возобновления.

Для изменения статуса каждой строки нужны ссылка на C++ реализацию, цель сборки, тест и протокол аппаратной проверки при HARDWARE_TESTED.

Реестр отдельно классифицирует решения миграции в поле migration_decision: поведение перенесено, внутренний API намеренно заменён, старый body пуст, условная конфигурация платы не используется, полезное поведение отсутствует либо есть часть с блокером. Статус BUILT/UNIT_TESTED относится к указанному новому компоненту; INTERNAL_SUPERSEDED не означает обязательство восстановить старую архитектуру. Неудачно смешивать полный gripper sequence с его работающим packet encoder.

## Текущая доработка: приоритет финальной2026

Написан отдельный gripper Engine и runtime adapters: cursor/side/main, masks, полный/раздельный block rotate/shift, lift, slide, timed/relative movement, wheel calibration, job query/cancel, выбранные capture/release/prepare. Старые task handles/notification glue намеренно заменены. Фазы и source-selected high-level baseline перечислены в GRIPPER_SCENARIOS. 2025 — только база/fallback. Свежие C++/ARM/ROS проверки перечислены в VALIDATION_REPORT; historical статусы CSV не заменяют текущую верификацию.
