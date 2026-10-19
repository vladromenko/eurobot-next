# Сервисные команды платы и приложение матча

Новый firmware предоставляет типизированные операции обслуживания через wire0x24 и ROS service `eurobot_interfaces/srv/Maintenance`. Это конкретные операции известных устройств, не произвольная пересылка UART/SPI/I2C. Все тела имеют ограниченный размер; неизвестные операции, неверная длина и non-finite параметры отвергаются.

Итоговая программная интеграция прошла: host/sanitizer/C++26 по53/53 CTest, STM32 debug/release/C++26 и ROS Jazzy/Lyrical build/test; результаты и журналы — в TESTING/VALIDATION_REPORT. Наличие этого документа не означает аппаратной приёмки. Физическая плата не прошивалась и не использовалась разработчиками; команды ниже предназначены для самостоятельного запуска владельцем после свежей сборки, настройки профиля и загрузки.

## Доступ и безопасность

- **Чтение:** owner0 разрешён. Нет claim/arm. Чтение не включает исполнительные устройства. `application_event` снимает одно событие из ограниченной диагностической очереди; это отмечено отдельно.
- **Disarmed:** нужен текущий owner/lease, но `armed=false`. Сюда относятся изменение ID/калибровок/режимов, reset, координаты и конфигурация периферии. Клиент получает только claim и затем release; arm не отправляется.
- **Owned:** match_disarm и communication требуют owner/lease и доступны также после снятия armed. Это позволяет вернуть communication=true после окончания матча, без автоматического включения механизмов.
- **Armed:** match_arm, wheel_holding, pca_allpulse требуют той же явной сессии и armed. В Mac-клиенте нужны одновременно `--allow-maintenance --exercise maintenance --allow-motion`. По окончании — ESTOP/release; reconnect/replay отсутствуют.

Диагностический default uncommissioned image допускает чтение, но не получает управляющую сессию при неуспешной аппаратной/профильной инициализации. Поэтому claim и записывающий maintenance могут вернуть invalid_config; это ожидаемый отказ, а не предложение включить commissioned без проверки. Чтение конкретного servo/stepper/BMS также требует его разрешённой привязки. Для изменений нужен подтверждённый deployment; disarmed session по-прежнему не выполняет arm.

На USART1 и дополнительном UART5 действует **один Controller и один owner/sequence**. Debug UART не является обходом safety gates. Профиль `debug_uart_enabled` по умолчанию false. При его включении UART5 PC12 TX / PD2 RX работает115200,8N1 с тем же новым framing; прежний raw fixed-struct debug protocol не принимается. Разъёмы и физические номера контактов требуют проверки.

## Mac CLI

По умолчанию `tools/board_test.py` только читает status+sensors. Примеры отдельных именованных запросов:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance board_info
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance application_state
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance wheel_ticks
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance wheel_path
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance diagnostic_text
```

Все значения пути — примеры, не найденные устройства. Для записи/смены состояния нужен отдельный флаг разрешения. `--maintenance-session` — alias `--allow-maintenance`, а не автоматическое вооружение.

```sh
# Очистить только признак reset; claim, без arm, затем release.
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance clear_reset --allow-maintenance

# Возврат приёма команд после99s timeout; это НЕ arm.
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance communication --arg 1 --allow-maintenance

# Явно заданная pose reference; только disarmed и при проверенной неподвижности.
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance set_pose --floats 0.2 0.3 0.0 --allow-maintenance
```

Не подменяйте реальные координаты примерными. Изменение ID требует инвентаризации по одному устройству и последующего обновления deployment: скрипт не переписывает конфигурационный header автоматически. BMS calibration — persistent maintenance; случайные значения недопустимы.

Пример **потенциально включает механизмы**: запуск match waiting state на подготовленном стенде. Только владелец после проверки физической кнопки/корда, reserve-side IDs и независимого stop:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance match_arm --allow-maintenance \
  --exercise maintenance --allow-motion --hold-ms 100000
```

Сначала нужен known attached cord, разрешающий motor input и текущий controller arm/lease. Извлечение корда запускает таймер. Клиент посылает match_arm **один раз**, затем поддерживает heartbeat и читает application state. Значение hold ограничивает срок жизни тестовой сессии; при её окончании или Ctrl-C выполняется ESTOP/release. По окончании матча firmware сам останавливает устройства и отключает communication. Повторный запуск требует новой явной процедуры.

`pca_allpulse` с armed упражнением обновляется только пока клиентом явно удерживается действие. `--hold-ms` допускает0…120000ms; это не бесконечная lease. Для остальных disarmed операций его значение не вызывает ожидания или arm.

## Все операции

В таблице `channel` — строго указанная сущность, а не произвольный bus address. `argument`/`secondary` — uint32. `values` — три float32; неиспользуемые поля задаются нулями. `--floats a b c` передаёт values. Числа `--arg`, `--secondary` поддерживают десятичную/0x запись. Servo speed можно задать через `--signed-argument -12`.

| Op / CLI name | Доступ | Параметры и смысл |
|---|---|---|
|0 board_info|read|Reset latch/cause, armed/estop/fault, uptime, owner|
|1 board_reset|disarmed|Stop всех устройств → ACK полностью передан через TC → системный reset|
|2 clear_reset|disarmed|Очистить latch и аппаратные reset-cause flags, без reset|
|3 wheel_ticks|read|Все три накопленных encoder totals, known mask|
|4 wheel_path|read|Все три пути в метрах с коэффициентами выбранной геометрии|
|5 reset_wheel_ticks|disarmed|Проверить неподвижность, сбросить три external totals, проверить нули; инвалидировать/синхронизировать reference|
|6 set_pose|disarmed|values=x[m],y[m],yaw[rad]; только после stationary baseline|
|7 servo_feedback|read|channel=allowlisted AX/MX ID1…253; position/present speed/load/voltage/temperature/moving|
|8 servo_led|disarmed|channel=ID; argument0/1|
|9 servo_speed|disarmed|channel=ID; signed raw speed−1023…1023 в two’s complement; readback командного register32|
|10 servo_torque_limit|disarmed|channel=ID; argument0…1023|
|11 servo_wheel_mode|disarmed|channel=ID; argument0 joint/1 wheel|
|12 servo_id|disarmed|channel=текущий ID; argument=новый1…253, без broadcast/конфликта|
|13 motor_range|read|channel=wheel index0…2; lower/upper TPM range|
|14 set_motor_range|disarmed|channel=wheel index; argument lower, secondary upper|
|15 motor_id|disarmed|channel=wheel index; argument новый ID1…253|
|16 stepper_reset|disarmed|channel0…3; реальный NRST с выдержками; теряется open-loop reference|
|17 pwm_info|read|channel0=TIM4 bank,1=TIM5 bank; frequency/ARR|
|18 set_pwm_frequency|disarmed|channel=timer bank; argument Hz; проверка групповых ресурсов и разрешённого generic PWM|
|19 gpio|read|Raw13 levels и known mask; без debounce/click policy|
|20 battery_calibration|disarmed|channel=чётный calibration register0x16…0x20; argument uint16|
|21 application_state|read|Filtered buttons/switches, mode, strategy, match timer/phase, communication/reset flags, errors/drop count|
|22 match_arm|armed|Явно разрешить ожидание извлечения корда|
|23 match_disarm|owned|Остановить и снять match state|
|24 application_mode|disarmed|argument0 game/1 debug; во время матча изменение запрещено|
|25 communication|owned|argument0 запрещает внешнее управление и останавливает;1 разрешает приём, но не arm|
|26 wheel_holding|armed|argument0/1; нулевая скорость и выбранная политика удержания|
|27 diagnostic_text|read|Ограниченный текст фактического controller state, а не константа «all OK»|
|28 application_event|read|Снять одно init/state/click/hold событие; available=false при пустой очереди|
|29 battery_id|disarmed|argument новый BMS ID1…253|
|30 battery_value|read|channel0 power,1 cells,2 voltage,3 raw current,4 SOC,5 cell count; независимые чтения|
|31 device_ping|read|channel0 motor/argument wheel index;1 allowlisted servo/argument ID;2 configured BMS|
|32 pca_control|disarmed|argument0 full_off,1 sleep,2 wake,3 initialize; при3 values[0] frequency/values[1] oscillator|
|33 pca_allpulse|armed|argument uint16 pulse µs; все16каналов должны быть разрешены/откалиброваны|
|34 stepper_parameter|read|channel0…3; argument0=status u16, иначе известный параметр u32; не подготавливает неизвестный driver|
|35 stepper_set_parameter|disarmed|channel0…3; argument известный register, secondary value; mask/writable validation, reference становится неизвестным|
|36 servo_probe|read, idle bus|channel=unicast1…253, три чтения identity/torque/position; без allowlist и writes|

Клиент не предоставляет произвольную строку CLI, чужой serial instruction или произвольный адрес памяти. L6474 register interface ограничен таблицей известных параметров, read-only fields защищены. ID254 в legacy main profile остаётся не назначенным до проверки. Пределы и pins не меняются в обход deployment validation.

## Обнаружение неизвестного Dynamixel: только чтение

`servo_probe` (operation36) обращается к одному явно указанному unicast ID1…253 на servo bus USART3. Не требует commissioned profile или AX/MX allowlist, не посылает torque/position/EEPROM writes и не сканирует шину автоматически. При armed Controller или действующей owner lease возвращает lease_conflict, даже если запрос отправил владелец: сначала явно завершить управление.

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance servo_probe --channel 98
```

Порт — host USART1, не прямое соединение Mac с servo bus. Фактический разъём USART3/DIO и питание сервопривода должны быть подтверждены по схеме. Обнаружение не включает питание и не изменяет состояние torque: оно может прочитать уже включённый torque. Неизвестная модель не преобразуется в угол; CW/CCW/position выводятся raw, чтобы не предполагать шкалу AX300° или MX360°. Timeout/NACK не доказывает отсутствие устройства. Broadcast254/255 и ID0 запрещены. Остальные поля запроса должны быть нулевыми.

Три bounded reads: register0 длина18 (model0/1,firmware2,ID3,CW6/7,CCW8/9,status-return16), torque-enable24 длина1, present-position36 длина2. Ответ:12bytes тела,13 с Error. Смещения тела:0 modelu16;2 firmwareu8;3 actualIDu8;4 CWu16;6 CCWu16;8 positionu16;10 torqueu8;11 status-returnu8. ActualID обязан совпасть с channel, torque0/1 и status-return0…2; при нарушении нет success body. Нет raw-register passthrough. Это программная функция; реальная шина при разработке не опрашивалась.

## Приложение финальной2026

`application.hpp/.cpp` заменяет глобальные flags и разделённые button/game tasks одним ограниченным состоянием Control owner. Сохраняются13 inputs, debounce8ms, hold500ms, init/state/click/hold events, циклическая strategy0…2, cord/side/motor flag, девять механических switches с обратной электрической логикой. Runtime sampling имеет период Control10ms: это временной debounce с такой дискретностью, не утверждение о восьми независимых1ms измерениях. Неизвестные/stale inputs не выдаются как свежие.

Match по умолчанию GAME, длительность **99000ms** согласно активному game_task2026:112. Запуск требует явного arm_match и уже разрешённого Controller. Извлечение known cord запускает таймер и, если engine свободен, открывает reserve side grip. Reinsertion, motor disable, потеря authority/inputs или timeout останавливают устройства. По timeout communication=false. Переполнение64-event queue фиксируется счётчиком и не отключает safety actions.

Config допускает явную историческую политику2025: debug/game mode, другой duration и `legacy_2025_motor_button_control=true`. В последнем случае только новое физическое нажатие после init и уже действующий arm/lease могут включить Maxon с нулевой скоростью. Начальный held level, repoll или последующее появление authority не выполняют отложенное включение.

## Reset и неопределённый результат

BOARD_RESET сначала останавливает механизмы. Pending reset запрещает новый arm. Reset выполняется после успешной полной передачи ACK на том UART, с которого пришла команда; ожидание не основано на произвольной задержке100ms.

Mac-клиент после подтверждённого reset ACK **не отправляет release/новые запросы** — сессия прекращается при reset. Если ACK потерян, результат неизвестен: нет retry, reconnect или повторного reset. Если получен явный NACK, клиент освобождает свою disarmed lease. Это не аппаратный reset test; полное отключение энергии проверяется отдельно.

## Wire0x24

Payload запроса — ровно22bytes, little endian:

| Offset | Поле |
|---:|---|
|0|operationu8|
|1|channelu8|
|2|argumentu32|
|6|secondaryu32|
|10,14,18|values[0…2] f32|

ACK type0xA4 совпадает по owner/sequence. Byte0 — Error; при ошибке нет достоверного success body. Остальные bytes — тело:

| Операция | Длина тела, bytes | Поля внутри тела |
|---|---:|---|
|0|16|reset/armed/estop/fault u8[4]; resetcause/uptime/owner u32[3]|
|3/5|13|known masku8 + ticks i32[3]|
|4|13|known masku8 + metres f32[3]|
|7|12|knownu8,positionf32,present_speedi16,loadi16,voltageu8,temperatureu8,movingu8|
|13/17/19|8|два u32 по смыслу соответствующей строки|
|21|24|mode/phase/strategy/flagsu8[4], known/pressed/switchknown/switchpressedu16[4], elapsed/droppedu32[2],error/stoperror/stopreason/reservedu8[4]|
|27|1…96|печатный diagnostic text|
|28|9|available/button/callback/action/stateu8[5],timestampu32|
|30|5/9/3/3/2/2|knownu8 + quantity поchannel0…5|
|34|2 либо4|statusu16 приargument0, иначеparameteru32|
|36|12|modelu16,firmwareu8,actualIDu8,CWu16,CCWu16,positionu16,torqueu8,status-returnu8|
|Остальные|0|Error0 подтверждает только результат соответствующей команды|

Командный register32 скорости AX/MX отличается от present-speed register38. Settled, referenced и measured также не взаимозаменяемы. Ни прочитанный software step counter, ни SPI ACK не подтверждают положение предмета или вакуум. Все аппаратные известности сохраняются отдельно.

Ошибки I/O после записывающей операции (timeout/output_failure/ошибка ответа) переводят Controller в fault и вызывают общий stop: частичная запись PCA не остаётся включённой без command deadline. Ошибка чтения или отказ валидации/authority этого не делает. При изменении Maxon ID занятость проверяется по текущим адресам всех трёх объектов, включая уже переименованные в этой сессии; повторное назначение одного нового ID двум колёсам отвергается. Изменённые постоянные адреса всё равно должны быть перенесены в deployment до следующей загрузки.
