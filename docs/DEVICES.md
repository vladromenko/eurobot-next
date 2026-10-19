# Собственные драйверы оборудования

Реализация: `firmware/include/eurobot/devices.hpp`, `firmware/src/devices.cpp`. Собственный код C++23, без `.c`, malloc/new, исключений, RTTI, бесконечных ожиданий и оператора `continue`. Объекты принадлежат управляющей задаче; драйвер не создаёт скрытые потоки. Протокольные поля кодируются явно, без `memcpy` структур C.

`SerialBus`, `I2cBus`, `SpiBus` и выходные callback — аппаратные границы. Это реальные кодеки и алгоритмы драйверов, которые необходимо подключить к конкретной периферии платы. Callback обязан ограничивать **всю транзакцию** параметром timeout_us, включая захват шины, передачу, направление RS-485, ожидание TC и приём. По умолчанию 5000 µs — программный бюджет, который нужно проверить на фактической длине пакетов, baudrate и нагрузке. Драйвер не делает бесконечных повторов.

## Dynamixel Protocol 1.0 и контроллеры Maxon

`DxlProtocol1` формирует пакет `FF FF ID LENGTH INSTRUCTION PARAMS CHECKSUM`; Ping/Read/Write/SyncWrite. Приём проверяет заголовок, ID, длину, checksum и device-error. Ответ с ошибкой не изменяет выходные измерения. Сохраняется байт `device_error()`. Буферы ограничены, payload ≤64 байт. `sync_write()` проверяет непустой список уникальных ID, ширину и точную длину данных; отправляет один broadcast без фиктивного ACK. Формат сверён с [официальным описанием ROBOTIS](https://emanual.robotis.com/docs/en/dxl/protocol1/).

Write по умолчанию transmit-only: старые конфигурации могут отвечать только на чтение. Успешная передача означает лишь accepted/commanded. Если Status Return Level устройства требует ACK на запись, вызывайте `write(...,true)` и настройте адаптер на этот режим. Перед запросом адаптер должен очищать устаревшие ответы и переключать half-duplex только после последнего переданного stop-bit. Протокол не является универсальным драйвером устройств Protocol 2.0.

`Maxon` использует обнаруженную карту собственного внешнего контроллера Eurobot:

| Регистр | Значение |
|---|---|
| 0x01 | ID |
| 0x04 | нижний/верхний диапазон TPM, 2×u32 |
| 0x10 | signed motor encoder ticks/minute, i32 |
| 0x14 | суммарные ticks, i32 |
| 0x18 | enable, u8 |

Реализованы set/get TPM, get/set ticks, enable/disable, get/set range, set ID. SetID/SetRange/SetTicks не выполняются автоматически при запуске. Показания становятся известными только после валидного ответа. Изменение ID/диапазона — операция обслуживания с проверкой фактического контроллера.

Источники: `02_FIRMWARE_2026/SOURCE/Core/Lib/maxon.c`, `Lib/Include/maxon.h`, аналогичные файлы 2025. Старый комментарий «Ticks Per Second (TPM)» противоречит названию; фактическая кинематика использует коэффициент 60, поэтому новый интерфейс и документация явно задают ticks/minute.

## AX/MX

`DynamixelServo` поддерживает обнаруженные карты AX 0…300°/1023 и MX Protocol1 0…360°/4095, аппаратные ID задаются конфигурацией. Реализованы положение с индивидуальными разрешёнными углами, signed raw speed, torque enable/limit, LED, чтение положения/скорости/нагрузки/напряжения/температуры/moving. Знак скорости/нагрузки декодируется как sign-magnitude. Нулевой MovingSpeed в joint-режиме означает специальный режим устройства; приложение не должно трактовать его как гарантированную остановку.

`wheel_mode()` записывает CW/CCW limits, `set_id()` меняет ID. Это EEPROM-настройки, запрещённые для автоматического повторения в управляющем цикле. Точный режим, модель, torque и механические пределы должны быть подтверждены на установленном устройстве. Параметр ServoConfig.maximum_deg — ограничение механизма, не паспортная гарантия безопасности.

Источники: `Lib/dynamixel_ax.c`, `Lib/dynamixel_mx.c`, `Lib/Include/dynamixel_ax.h`, `Lib/Include/dynamixel_mx.h`. Пакетные тайм-ауты заменяют старые циклы `while(response.is_data_ready==0) taskYIELD()`.

## BMS

`read_battery()` читает ровно 18 байт с 0x04; `decode_battery()` явно извлекает power u32, четыре cell mV u16, voltage mV u16, current mA u16, SOC u8, число ячеек u8. Отклоняются неполный пакет, SOC>100 и число ячеек вне 1…4; старые показания при ошибке сохраняются у вызывающего кода и должны отмечаться недостоверными. Текущий register-map определяет ток как unsigned: направление тока не выдумывается.

`calibrate_battery()` — отдельно помеченная операция обслуживания, допускает только чётные calibration-регистры 0x16…0x20 и требует ACK. Boot и обычные motor/pump commands её не вызывают; явный Maintenance 20 с disarmed owned lease открывает этот метод после проверки конфигурации. Дополнительно доступны отдельные чтения power/cells/voltage/current/SOC/count и смена BMS ID через Maintenance 30/29.

Источник: `Lib/bms_board.c`, `Lib/Include/bms_board.h`. В старом GetBatteryState длина запроса 18 байт, затем используется `sizeof(struct)`; на типичном ABI с хвостовым padding это 20 байт. Новый декодер не читает padding и не зависит от упаковки C-структуры.

## PWM, RC-servo и PCA9685

`servo_pulse()` отображает калиброванные углы в µs, отвергает NaN и неверные/обратные диапазоны. Это общий код для прямого таймера STM32 и PCA. Дефолтные 500…2500 µs/0…180° воспроизводят конфигурацию старого PCA-драйвера, но каждый реальный сервопривод требует своей подтверждённой калибровки.

`Pca9685` использует 7-bit I2C address, проверяет prescale, сначала отправляет `ALL_LED_OFF_H=0x10`, затем sleep/AI, prescale, MODE2, wake, реальную задержку 500 µs и restart. Внешний OE/аппаратное отключение следует удерживать в безопасном состоянии при загрузке и сбоях шины. EXTCLK-конфигурация отклоняется: этот драйвер настроен на известный внутренний генератор, переключать sticky EXTCLK без reset нельзя. Реальная PWM-частота вычисляется с округлённым prescale.

`full_off()` устанавливает специальный FULL_OFF через один регистр, независимо от AI. ALL_LED-регистры заполняют соответствующие LEDn-регистры: их нельзя считать отдельным глобальным override. После full-off запись одного канала не возвращает старые импульсы других каналов. Равные ON/OFF counts отвергаются. `all_pulse_us()` явно задаёт все каналы, `sleep()` сначала отключает все, `wakeup()` выдерживает 500 µs и оставляет их отключёнными. Эти детали сверены с [NXP PCA9685, §§7.3.1, 7.3.4–7.3.5](https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf).

Источники старого проекта: `Lib/pca9685.c`, `Lib/rc_servo.c`. В старом PCA init «пустое чтение» использовано вместо реальной задержки, а all-off записывает нулевые ON/OFF counts. Новая реализация исправляет обе причины неопределённого запуска.

## H-bridge

`HBridge` принимает конечный signed duty в диапазоне −1…1, до первого движения пишет ноль, перед реверсом также пишет ноль, сообщает ошибки и достоверность commanded. `BridgeSink` обязан обеспечить соответствующую схеме комбинацию PWM, разрешение выхода и необходимое deadtime; переносимый модуль не предполагает полярность конкретного силового каскада. Нельзя считать duty=0 универсальным электрическим тормозом. Параметр `stop(brake)` должен поддерживаться и проверяться адаптером.

Источник: `Lib/h_bridge.c`: две пары PWM1/PWM2 и PWM3/PWM4, собственная таблица сигналов. Изменение полярности без проверки схемы не выполнялось.

## L6474 и шаговое движение

`L6474` реализует SetParam/GetParam, ENABLE B8, DISABLE A8, GET_STATUS D0, реальную ширину регистров, маски допустимых битов и big-endian данные. Только известные регистры доступны, ADC/STATUS не записываются, недопустимые STEP_SEL отклоняются. При записи STEP_MODE принудительно выставляются обязательные биты 7 и 3 (`value|0x88`, ST §9.1.10). Контекст SpiBus выбирает нужный драйвер; аппаратный адаптер должен поднимать CS между **каждыми байтами**, выдерживать tdisCS и соблюдать SPI mode/частоту. Эти требования подтверждает [ST L6474 datasheet, §8](https://www.st.com/resource/en/datasheet/l6474.pdf).

`StepperPlanner` хранит командную координату после явно установленного reference, ограничения хода и скорости. Без reference обычное движение запрещено. Homing имеет обязательный конечный timeout и заканчивается по реальному switch. Home-switch должен соответствовать подтверждённой границе minimum_steps/maximum_steps. Одно tick выдаёт максимум один полный импульс; пропущенное время не догоняется пачкой. Слишком редкий tick, deadline или отказ выхода останавливает движение и инвалидирует reference. Pulse callback обязан выдерживать аппаратные high/low и direction setup; для высокой скорости нужен аппаратный таймер/ISR. Значение `commanded_steps` не является измеренной координатой двигателя; датчик пропущенных шагов отсутствует.

Источник: `BSP/l6474_drvr.c`, `BSP/Include/l6474_drvr.h`, `BSP/stepper.c`, `OS_Tasks/stepper_task.c`. Токи1100/1500mA из старых init не применяются автоматически к неизвестной новой механике.

`StepperProfile` и move_profile/move_relative_profile/home_profile восстанавливают division-based разгон/торможение 2026: fixed table ≤64, edge/working divisions, base frequency и acceleration_ms. Короткий ход понижает peak speed, чтобы разгон/торможение уместились; zero/tiny stage counters не underflow. Profile factory default 19200 Hz / edge32 / working10 / 200 мс — техническая reference, не автоматическая настройка неизвестного мотора. Runtime service TIM1 реально 100 мкс; elapsed base ticks накапливаются дробно, за один service разрешён максимум один полный pulse, поздние вызовы не догоняются пачкой. Overspeed/deadline/output error инвалидируют reference. Explicit actuator 12 использует configured ramp; homing может использовать его при stepper_profile_enabled. Подтверждённые switch direction, travel/current/microstep limits остаются обязательными.

Реальный L6474 hard_reset удерживает NRST и затем выдерживает release delay; failure оставляет безопасное reset состояние. Maintenance 16/34/35 дают reset/status/GET_PARAM/SET_PARAM через существующий profile/channel и disarmed gate для изменений. Запись параметра сбрасывает software reference: её нельзя считать новым физическим нулём.

## WS2812B

`ws2812_waveform()` кодирует цвета в порядке GRB, MSB-first, выдаёт compare-values и явный хвост LOW reset, проверяет размер выходного буфера и порядок длительностей. Runtime и DMA buffer поддерживают до 60 светодиодов 2026; фактическое led_count задаётся профилем и проверяется перед доступом. Наличие буфера не означает физически подключённые60 LED. Частота таймера, zero/one high и длина reset задаются аппаратным профилем; модуль не считает произвольный MCU clock подходящим. Board adapter DMA подтверждает завершение либо timeout и не меняет активный буфер. Actuator 9 допускает индекс 0..59 только в пределах configured led_count; HSV→RGB и span conversion проверяют весь вход до изменения выходов.

Источник: `BSP/ws2812b.c`, `BSP/Include/ws2812b.h`: GRB-порядок структуры, старые CCR_LOW=68/CCR_HIGH=142. Эти числа допустимы только вместе с восстановленной частотой TIM8.

## Доступность методов через единый API

Actuator 0x20 предоставляет AX/MX position/torque, PCA/PWM position, explicit/profiled stepper movement/home, DC duty/brake, generic PWM duty/disable и LED RGB. Maintenance 0x24 предоставляет servo LED/speed/limit/wheel-mode/ID/readback, Maxon range/ID/ticks/path, BMS calibration/ID/отдельные quantities, PWM info/frequency, PCA full-off/sleep/wake/init/all-pulse и L6474 reset/status/parameters. Это reachable request handlers, а не только библиотечные определения. Для EEPROM/калибровки и большей части изменений нужен disarmed owner; armed исключения и длины ACK описаны в ROS2_INTERFACES.md. Успешная передача не становится измеренной позицией; отсутствующие реальные routes возвращают invalid_config/unsupported.

HBridge и generic PWM используют profile frequency/group exclusions. Runtime устанавливает индивидуальные deadlines вспомогательных outputs; команда LED не продлевает старую DC/servo/PWM команду. Stop проходит все приводы, даже если один backend сообщил ошибку. Совместное использование таймерного банка и электрическая brake polarity проверяются профилем, а не предполагаются библиотекой.

## Доступные проверки

`tests/device_tests.cpp` запускается на компьютере, имеет собственный main и не зависит от NDEBUG. Проверены эталонный Ping пакет, signed Maxon TPM и реальные адреса, read failures без изменения измерений, повреждение checksum/ID/length/device-error, timeout, SyncWrite count0/дубликаты/длина, AX/MX mappings и sign-magnitude, EEPROM-команды только при явном вызове, BMS18/padding, PCA с независимой моделью LED-регистров, sleep/wakeup/full-off, PWM calibration, H-bridge реверс, L6474 byte-order/masks, stepper homing/motion/deadline/wrap и GRB/reset-buffer.

Текущие suites tests/device_tests.cpp, runtime harness и maintenance wire tests проверяют эти кодеки/ограничения/dispatch. Итог актуального host/ASan/UBSan запуска указан в VALIDATION_REPORT.md; старые результаты не заменяют проверку нового образа. Модуль отдельно компилируется ARM GNU14.3 для Cortex-M4F, C++23, hard-float, без исключений/RTTI. Это подтверждает программную часть и кодирование транзакций; аппаратная работа, реальные уровни, токи, направления, тайминги и временной бюджет ещё требуют HIL. Ни один драйвер не подменяет отсутствие датчика фиктивным measured-success.

## Восстановление электрических настроек L6474

`prepare_stepper` после каждого снятия NRST заново программирует ALARM_EN, CONFIG, STEP_MODE (с обязательными fixed bits0x88), TVAL, OCD и при заданном override TOFF_MIN. Профиль сохраняет source2026 ALARM_EN[0x03,0x87,0x87,0x87]; TOFF_MIN первого драйвера0x1f, значение255 для остальных означает оставить reset default. Это source baseline, не подтверждение пригодности токов/защиты конкретного механизма. `stepper_alarm_enable` и `stepper_toff_min` проверяются до разрешения профиля; тест runtime проверяет реальные SPI bytes и повторное восстановление после reset.

Constant-speed и profiled homing ограничены и временем, и полным настроенным ходом. Отсутствующий switch даёт timeout без дополнительного импульса за пределом поиска; программная позиция остаётся неизвестной.
