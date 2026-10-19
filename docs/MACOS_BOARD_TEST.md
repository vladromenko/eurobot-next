# Проверка платы с Mac после самостоятельной прошивки

Для этой платы подготовлен заполненный source-профиль2026 и единый установщик: [ROBOT_2026_PROFILE.md](ROBOT_2026_PROFILE.md). Команда `python3 tools/install_robot.py flash` собирает его и выполняет backup/write/readback после подключения ST-LINK. Шаблон deployment вручную заполнять не требуется. Нижеприведённые команды общего `flash.sh` относятся к явно выбранному образу; без `--image` его default — диагностический Debug, а не рабочий source-профиль.

`tools/board_test.py` — отдельный Python3-клиент для новой прошивки EUROBOT NEXT. Он не устанавливает firmware, не вызывает CubeProgrammer и не выбирает устройство автоматически. Нужны только стандартные модули Python; pyserial не нужен.

**Текущая редакция C++ прошла итоговые программные проверки; протокол и журналы приведены в VALIDATION_REPORT.** Перед самостоятельной загрузкой образ должен соответствовать текущим исходникам и deployment; это проверяет source fingerprint. Старые или несогласованные образы отклоняются загрузчиком. См. [INSTALL.md](INSTALL.md), [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md).

В ходе разработки клиента выполнены только Python-тесты с локальными PTY и подставной платой: **28/28 PASS**. Это проверяет формат, serial API, исключительное открытие, тайм-ауты и cleanup. Никакой реальный `/dev/cu.*`, ST-LINK или робот этими тестами не открывался. Они не подтверждают работу STM32, новой прошивки или механизмов.

## Подключение для теста

Для загрузки используется SWD/ST-LINK; для этого клиента — **USART1 через USB-UART 3.3 V**. Это разные подключения.

| STM32 | USB-UART |
|---|---|
| PA9, USART1 TX | RX адаптера |
| PA10, USART1 RX | TX адаптера |
| GND | GND |

На присланной карте USART1 находится вверху платы и подписан GND/RX/TX. Соединять по этим подписям, не по порядку строк таблицы или внешнему виду другого адаптера. Полная карта: [BOARD_WIRING.md](BOARD_WIRING.md). Нужен UART с уровнями 3.3 V, не RS232 и не 5 V TTL. Не соединяйте питание адаптера с платой без подтверждённой схемы питания. Само подключение/открытие UART у некоторых адаптеров может менять DTR/RTS или вызывать reset через внешнюю схему; скрипт не отправляет reset, но не управляет такой проводкой.

Новый протокол: 115200 baud, 8N1, без flow control. Legacy baud921600 для этой прошивки не подходит. Перед первым тестом питание моторов, насосов и механики отключено независимо от MCU. Закройте ROS bridge, serial monitor, IDE и другие владельцы порта. Скрипт требует TIOCEXCL и advisory flock; это не разрешение отбирать активное управление.

Найдите имя адаптера в системном отчёте Mac или самостоятельно посмотрите список `/dev/cu.*`. Скрипт сам не перебирает порты. Пример ниже содержит placeholder; замените его реальным путём.

## Сначала только чтение

Из корня EUROBOT_NEXT:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER
```

Отправляются **только status0x08 и sensors0x09**, owner0, без claim/arm/движений. Результат — по одному JSON для status и sensors. Даже если контроллер принадлежит другому owner, скрипт ничего не захватывает и не останавливает. `--timeout 0.5` задаёт конечное ожидание ACK; допустимо 0.02…1s. Никаких повторов запроса и автоматического reconnect.

Проверить:

- `armed=false` до самостоятельного разрешённого испытания, неожиданный `owner`/активный lease объяснён;
- ошибки `controller_error`, `pump_error`, `chassis_error`, firmware error видны отдельно;
- нулевой `*_known` означает отсутствие подтверждения, а не исправность;
- `battery_known=false` допустим для не настроенного BMS; raw GPIO имеет независимую известность;
- ток публикуется как `raw_current_ma`, `current_sign=unknown`: направление не выводится по предположению;
- `pose_known=false` не превращается в мировую позу 0,0,0: JSON даёт `null`.

Старое firmware без read-only extension может вернуть однобайтный NACK sensors. Он отображается как недоступность; malformed success не принимается за датчики.

## Одна явно разрешённая операция захвата

Выполняется только после проверки нового firmware, commissioned deployment, всех ID/каналов/полярностей, механических пределов и аппаратного stop. В legacy main ID254 — broadcast; такой профиль не разрешает реальное движение. По исходникам подтверждены четыре общих насоса. Для новой разводки8+8 на PO1…PO16 имеется examples/board_8_pumps.hpp с независимыми группами захватов; существующий жгут нужно сверить с этим назначением.

Пример **потенциально двигает cursor**; выполнять только владельцу подготовленного стенда:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --exercise gripper --allow-motion \
  --gripper 1 --operation cursor --value 1 --job-timeout-ms 5000
```

Оба флага `--exercise` и `--allow-motion` обязательны. Путь без них остаётся только чтением. Перед упражнением клиент проверяет, что исходный status не armed и не занят другим owner. Затем создаёт новый ненулевой nonce, выполняет claim с lease2000ms и явный arm; отказ прекращает действие. Индекс CLI gripper1/2 переводится в wire0/1.

При принятом job клиент опрашивает его ID и обновляет lease heartbeat. Отправленная команда не повторяется, потерянный ACK не вызывает второй запуск. При timeout, неверном snapshot, disconnect или Ctrl-C нет переподключения. Если владение было подтверждено, в finally предпринимаются bounded cancel активного job, **ESTOP и release**. ESTOP остаётся защёлкнутым: это ожидаемое завершение упражнения. Последующее управление требует новой сессии и явного arm. Cancel уже завершённого job не отправляется.

Успех команды остановки не доказывает физическое снятие энергии. Если cleanup не подтверждён, используйте аппаратное отключение. При claim conflict клиент не посылает ESTOP чужому владельцу.

### Значения операций

| `--operation` | Номер | Поля |
|---|---:|---|
|cursor|0|value0/1: vertical/horizontal|
|side_grips|1|value0/1: open/closed|
|rotate_main|2|value0/1/2: up/down/cursor|
|pumps / valves|3/4|mask0…15, local gripper route|
|blocks_rotate|5|mask0…15, spread→rotate→close|
|timed_move|6|value signed velocity m/s (yaw rad/s), argument duration ms, mask axis0X/1Y/2yaw|
|lift_height|7|value height mm; требуется реальный reference|
|relative_move / relative_move_poll|8/12|value X metres, argument Y metres, speed m/s|
|wheel_calibration|9|mask wheel index0…2; параметры из deployment|
|blocks_shift|10|value0/1: close/spread|
|blocks_rotate_only|11|mask0…15; bit3 первый block|
|slide|13|value rack distance mm|
|legacy_2025_reset / legacy_2025_cycle|20/21|Явная историческая операция, не профиль 2026 по умолчанию|
|capture / release / prepare|22/23/24|Только выбранный и проверенный составной профиль 2026|

Наличие номера не означает, что профиль разрешает действие. Клиент проверяет finite float32 и uint32; окончательная семантика/limits проверяется firmware. Отказ нельзя обходить подбором ID, удалением validation или включением broadcast.

## Ограниченная проверка выходов

Пример выбирает только логический pump bit0 на 150ms. Это физическое действие, не обычная проверка связи:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --exercise output --allow-motion --select 0x1 --values 0x1 --duration-ms 150
```

`select/values` и `valve-select/valve-values` — пары uint32; values должны быть подмножеством select. По умолчанию все 0; нужен явный ненулевой select. Максимальная длительность упражнения 5000ms, потом ESTOP/release. Пока оно длится, команда обновляет только выбранные каналы каждые≤100ms; heartbeat отдельно продлевает lease. У других каналов собственный command deadline не продлевается.

После завершения gripper job автоматическое продление затронутых им выходов прекращается; обычный deadman250ms снова требует свежей команды. Этот клиент завершает упражнение через ESTOP, поэтому не оставляет pump ON «для осмотра». Запись SPI подтверждает commanded, не вакуум/ток.

## Точный wire-контракт

Кадр: `A5 5A`,version1,typeu8,sequenceu32,owneru32,payloadlengthu16,payload,CRCu16; little endian, float IEEE754 binary32. CRC16/CCITT-FALSE по bytes от version до payload, poly0x1021/init0xffff; test123456789→0x29b1. Payload≤128. Ответ должен совпасть по `type|0x80`, sequence и owner; неверные CRC/несвязанные ACK игнорируются только в пределах исходного deadline.

Status success/error snapshot — строго 84bytes, sensors — строго 25bytes; sensors unsupported допускает 1byte nonzero error. Job ACK —25bytes либо 1byte nonzero error. Однобайтный 0 не является валидным успехом.

Submit0x21 payload22bytes:

| Offset | Поле |
|---:|---|
|0|gripper indexu8 0/1|
|1|operationu8|
|2|valuef32|
|6|masku32|
|10|timeout_msu32,100…60000|
|14|argumentf32|
|18|speedf32|

Query0x22 и cancel0x23: payloadjob_idu32. Job snapshot:

| Offset | Поле |
|---:|---|
|0|Error результата запроса|
|1|phase: idle0/running1/completed2/cancelled3/faulted4|
|2,3|operation,gripper index|
|4,5|step,step count≤64|
|6|evidence unknown0/commanded1/settled2/referenced_steps3/measured4|
|7…10|число шагов каждой категории commanded/settled/referenced/measured|
|11,12|job error,stop error|
|13,17,21|job_idu32,owneru32,elapsed_msu32|

Категории evidence различаются по смыслу: settled — прошло время, referenced_steps — отсчитаны шаги от установленного reference, measured — получен подходящий реальный feedback. Ни одна категория автоматически не доказывает положение предмета или вакуум. Завершение PCA по времени не называется measured.

## Воспроизвести только mocks

Эти команды не открывают пользовательские serial-устройства и не компилируют C++:

```sh
python3 tests/board_test_test.py -v
python3 tools/board_test.py --help
```

Строго проверяется, что completed имеет непустой полный план и полное число evidence; ложный completed0/0 отвергается. Default smoke exit0 означает только успешную связь и разбор ответа: uncommissioned/fault могут быть видны в JSON и не становятся готовностью механизмов.

Проверяются CRC/fragments/noise, bounded parser, строгие размеры/known flags, payload offsets, отказ неправильных CLI flags до открытия порта, default read-only requests, отбрасывание ACK другого owner/sequence/opcode, timeout без replay, TIOCEXCL, heartbeat/cleanup, NACK и конфликт владельца. Для реального стенда нужен отдельный протокол с версией firmware/profile, условиями питания, оператором и измеренными результатами.

## Именованные сервисные команды

Полная таблица и форматы: [MAINTENANCE.md](MAINTENANCE.md). Без включения механизмов:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance board_info
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance application_state
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance diagnostic_text
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER --maintenance wheel_ticks
```

Read-only запросы сохраняют owner0 даже при уже занятой плате. Изменение конфигурации требует `--allow-maintenance` (alias `--maintenance-session`): claim, одна named operation и release **без ARM**. После99s окончания матча вернуть communication=true можно так:

```sh
python3 tools/board_test.py --port /dev/cu.usbserial-YOUR_ADAPTER \
  --maintenance communication --arg 1 --allow-maintenance
```

Это не rearm. match_arm/wheel_holding/pca_allpulse относятся к armed exercise и требуют дополнительно `--exercise maintenance --allow-motion`; срок удержания задаётся `--hold-ms` и ограничен120000ms. BOARD_RESET после ACK ожидает прекращение сессии без follow-up release; потерянный ACK не повторяется. Проверки26/26 относятся исключительно к local PTY/mocks: дополнены все36 типов maintenance, известности/длины/ошибки, claim-only, post-match recovery и reset/no-retry.

Опциональный второй порт UART5 PC12TX/PD2RX использует115200 и ту же блокировку owner/lease. Он не включается default profile и не создаёт обход управления через USART1. Фильтр приложения и raw GPIO — разные snapshots: `sensors`/`gpio` дают электрические уровни, `application_state` — фильтрованные состояния и матч.
