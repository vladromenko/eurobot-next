# Первый запуск: поиск причины без опасных повторов

Для первого включения используется **общий DIAGNOSTIC_PROFILE в DIAGNOSTIC_MODE**, выбранный в [WHAT_I_NEED_TO_DO.md](../WHAT_I_NEED_TO_DO.md). Год установленной старой прошивки не определяет фактическую проводку одной и той же синей платы. Профили2025/2026 — источники-кандидаты для сравнения; ни один не назначается реальному роботу по одному названию года. До индивидуальной приёмки движение — BLOCKED. Процедуры устройств: [HARDWARE_VALIDATION_PLAN.md](HARDWARE_VALIDATION_PLAN.md).

Неисправность устраняют от питания/проводки/идентификации к протоколу и профилю. Не заменять неизвестный параметр случайным, не увеличивать ток/ход/timeout без измерения, не повторять job после потерянного ACK. При нежелательном движении сначала снять энергию аппаратным stop. Программное подтверждение остановки не доказывает физического удержания лифта или снятия давления.

## Что сохранить до изменения настроек

- Полный текст команды и вывода, её exit code, время события; лог до перезапуска/сброса.
- BIN hash/manifest/source fingerprint и ответ `deployment_info`: mode/profile/commissioned/valid. `valid=true` — согласованность software, не доказанная механика.
- Фото маркировки устройства и его фактического разъёма, питания и жгута; выводы платы определять по подписям карты. Не присылать пароли/токены других систем.
- `status`, GPIO/known masks, `board_info/reset_cause`, device feedback, job/error/stop_error, если они доступны безопасным чтением. Unknown/null сохранять как unknown, не заменять нулём.
- Какие силовые устройства были физически обесточены, что происходило при stop; отдельное видео с неподвижной камерой и понятным направлением механизма.

Не стирать Flash/option bytes, не выполнять factory reset сервопривода, ID assignment, reset counters или EEPROM-калибровку до сохранения исходных данных. `backup` — реальная копия только после успешного полного чтения; отказ чтения не создаёт резервную копию.

## Короткий read-only набор на Mac

Только для уже установленного NEXT, не для произвольной legacy-прошивки. Из корня проекта:

```sh
cd /Users/vladromenko/Desktop/Eurobot/EUROBOT_NEXT
python3 tools/board_test.py --help
python3 tools/actuator_test.py --help
ls /dev/cu.*
```

Выбрать фактический USB-UART вручную, закрыть все другие владельцы порта, задать `PORT`. Далее при отключённой силовой части:

```sh
python3 tools/board_test.py --port "${PORT:?Задайте_проверенный_путь_адаптера}" --maintenance deployment_info
python3 tools/board_test.py --port "$PORT" --maintenance board_info
python3 tools/board_test.py --port "$PORT" --maintenance diagnostic_text
python3 tools/board_test.py --port "$PORT" --maintenance gpio
```

Default `board_test` читает status/sensors, не claim/arm. Дополнительный named read-only запрос также не включает механизмы. Электропитание внешнего устройства и риск включения от старого сохранённого target проверяют **до** его подключения/опроса; выключенная нагрузка может законно не отвечать. `actuator_test --help` ничего не открывает; сам тест требует всех обязательных аргументов и `--allow-motion` и откажет в diagnostic/uncommissioned/invalid до claim.

## Таблица неисправностей

| Симптом | Возможная причина | Безопасная проверка | Действие / граница допуска |
|---|---|---|---|
| STM32 не обнаруживается | нет питания логики; неверный SWD; MCU в reset; иная модель/повреждение | силовая часть выключена; фото MCU/SWD; измерить **подтверждённую** логическую точку/GND; проверить маркировку кабеля/NRST/общую землю | сначала установить схему питания. Не подавать на SWD/MCU соседние+5/+6/+12V. VTref — вход измерения программатора, не обещанный источник платы |
| ST-LINK не виден Mac | кабель только зарядный; USB-переходник/питание; CLI/драйвер; другой debugger держит probe | `system_profiler SPUSBDataType`; по установленному CLI `"$STM32_PROGRAMMER_CLI" -l st-link-only` | проверить USB-кабель/порт, закрыть debugger. Перечисление probe не доказывает связь SWD с target. Если их несколько — выбрать серийный номер, не первый попавшийся |
| ST-LINK виден, target нет / wrong ID / wrong Flash size | перепутаны SWDIO/SWCLK/NRST; нет VTref; другая модель/размер; неправильная схема загрузки | отдельная инструкция [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md), фото подписей; ID семейства0x413 и Flash-size из read-only SWD должны совпасть с manifest | не менять expected ID/size/адрес в manifest для обхода. При неизвестном bootloader остановиться. Не подключать clone-power pin по предположению |
| Readout protected / backup failed | RDP; питание/связь; неполное чтение | сохранить вывод CLI и фактическую длину backup; проверить настройки защиты только чтением по главной инструкции | не выполнять read-unprotect/option-byte write: они могут стереть старую Flash. Запись запрещена, пока существующая процедура не может сделать требуемую копию |
| Offline verify отказал | старый BIN; изменён профиль/исходники; неправильный manifest/vector/hash | `python3 tools/flash.py verify --image "${IMAGE:?Текущий_BIN_из_главной_инструкции}"` без разрешающих флагов; это только локальная проверка | новая чистая сборка выбранного диагностического профиля. Не редактировать hash вручную и не использовать старый HEX/BIN из архива |
| Запись Flash/independent readback ошибка | питание/кабель/target layout/защита; чужой probe; устаревший образ | сохранить полный flash log, выбранный serial, backup JSON; аппаратно изолировать нагрузки | исход неизвестен; не делать автоматический повтор, full erase или protection changes. Проверить причину отдельно. Реальную запись в этой работе не выполняют |
| После записи UART молчит | образ оставлен halted; logic power/reset/clock; другой adapter; RX/TX/земля; старая прошивка | убедиться по отдельной инструкции, что MCU явно запущен и силовая часть off; USART1 TXPA9→adapterRX и RXPA10←adapterTX;1152008N1 для NEXT | не путать ST-LINK/SWD с UART. Новая утилита не декодирует legacy921600. Не перебирать старые управляющие пакеты и не подавать5V TTL/RS232 на RX |
| UART busy / exclusive open denied | ROS bridge, IDE monitor, другая панель/клиент | закрыть известный процесс-владелец, проверить окно/терминал, затем повторить **read-only** подключение | не снимать TIOCEXCL/lock и не запускать два serial path. После ROS остановить bridge до Mac-клиента |
| CRC/неправильный opcode/короткий success/deadline | скорость/шум/земля/конфликт UART; старая версия протокола; устаревший owner/sequence | один короткий status с конечным timeout, без силовой нагрузки; проверить совпадение образа/клиента; сохранить bytes/log | не принимать malformed success за исправность. Тайм-аут команды движения — unknown outcome; не повторять её. Сначала stop/осмотр |
| `mode=diagnostic`; arm/outputs/jobs отвергаются | ожидаемая защита первого запуска | безопасно прочитать `deployment_info`; сверить intended generic diagnostic образ | **нормальный BLOCKED**, а не ошибка драйвера. Для движения нужен отдельно проверенный operational deployment, новая сборка/установка и явный arm; runtime обхода нет |
| OPERATIONAL, но `commissioned/valid=false` / invalid_config | незаполнены реальные limits/mapping; conflicting pins; активирован неподтверждённый backend | прочитать deployment info/diagnostics, проверить выбранный header/manifest и параметры одного устройства | не ставить только флагtrue и не подменять неизвестные значения нулями. Сначала закрыть список сведений для этой функции и пройти проверку профиля |
| `not_armed` / `estop` / lease expired | diagnostic; явный stop; закончилась lease; fault; потерян поток | state + deployment mode + original error fields, последняя команда/время | после проверки физической остановки нужен новый явный arm/nonce; не auto-arm. Heartbeat не заменяет свежий pump/DC/velocity request |
| lease_conflict / старый token / второй publisher | занят чужой session; overlapping job resource; stale token/GID | state.owner/lease, token только из текущего Arm response, ROS publisher count/GID; журнал активного job | не забирать чужого владельца. Обычный конфликт ресурса не означает transport failure; не повторять задачу вслепую. Один bridge, один текущий оператор, один cmd_vel publisher |
| Двигатель не отвечает | фактический ID/RS485-порт/baud/питание/DE не совпал; source-кандидат другого жгута | после проверки питания безопасный `device_ping --channel0 --arg WHEEL_INDEX`; `motor_range`; фото контроллера/разъёма | сначала адресация/проводка. Не менять ID/baud/max range широковещательно и не подавать PWM вместо RS485. Не считать unanswered мотор отсутствующим по одному timeout |
| Колесо движется не туда / другое колесо | переставлены индексы, направление мотора, ориентация координат | все колёса подняты, один принятый малый wheel test, raw ticks/video | аппаратный stop, затем сверить каждую физическую связь. Не компенсировать ошибку ID глобальным знаком Twist. Не запускать полную калибровку400000TPM первым тестом |
| Энкодер frozen/скачет/неверный знак или масштаб | питание/кабель/ID; разрядность/overflow; неверный gear ratio/направление | raw `wheel_ticks` до/после одного измеренного допустимого малого поворота, known_mask, отдельный канал | unknown не ноль. Не reset counters до сохранения evidence. Проверить реальный encoder/вал/редуктор, затем только калибровочные коэффициенты |
| Dynamixel не отвечает | ID/model/Protocol1/baud/питание/half-duplex DE; неверный выбранный bus | один `servo_probe --channel SERVO_ID`, mode safe/idle; individual ID1..253; readback actual_id/model | не пробовать254 как реального servo и не запускать ID-assignment на общей шине. Дополнительный найденный ID не назначает механическую роль автоматически;2026 inventory labels — справка, не проводка |
| Dynamixel дёрнулся / угол неверный / чужой привод | градусы перепутаны с радианами; неверная модель/raw scale; mapping; torque включил сохранённый target | аппаратный stop; фото модели, probe/feedback, команда с единицами, current position/limits | AX/MX в actuator_test — radians; PWM/PCA — degrees. Не включать torque неподдержанного механизма. Source112/main98/cursor не применять к неизвестному жгуту автоматически |
| PWM/PCA servo не работает / не тот ход | нет питания; неверный zero-based channel; group frequency; pulse calibration; PCA address | выходы off; `pwm_info`, осциллограмма без servo если возможно, проверка питания/канала | частота/ACK не измеряют угол. Не использовать ALL_PULSE, не менять общий timer при подключённых других нагрузках. Сначала модель/pulse limits |
| Stepper не движется / шумит / пропуски | пары обмоток, ток/режим, enable/reset, направление/скорость, reference absent | напряжение/обмотки проверять обесточенно; `stepper_parameter --arg0`, driver faults, mapped GPIO | снять энергию при перегреве/упоре. Не переставлять обмотки под током; не повышать ток и не обнулять software position вместо home |
| Концевик не срабатывает | неверный input bit/полярность; обрыв; нет logic reference/pull; неисправен switch | read-only `gpio` в двух состояниях + `application_state` после debounce; проверить конкретный connector и допустимый электрический уровень | не коротить неизвестный GPIO и не отключать home gate. NO/NC и fail-open поведение должны быть подтверждены до home/движения |
| Home выполнен, следующий relative/lift отвергается | ESTOP/disarm/release инвалидировал reference | журнал cleanup прошлого процесса, firmware reference policy | это ожидаемая защита. Home и последующий move требуют verified reference в одной owned session; отдельный завершённый actuator_test home не калибрует следующий процесс. До такого пути испытание BLOCKED |
| Лифт движется не туда / падает после stop | направление/масштаб/ноль; ошибочная механика; неподтверждённый torque-off/hold режим | независимая опора уже установлена, аппаратный stop, raw limit state, измеренный ход/ток | не держать падающую механику программным бесконечным heartbeat. Подтвердить удержание/тормоз/опору и реальный безопасный ход до нового разрешения |
| Насос не работает / не тот насос | logical bit перепутан с PO; нет силовой шины; low-side key wiring; unsupported route; rating | один подтверждённый короткий output test только после допуска; измеренные напряжение/ток и commanded/known | исходные четыре пары и будущие8пар — разные разводки. Не подключать PO как+12V и не повышать max_active до8 по наличию8каналов API |
| Клапан переключается наоборот / давление остаётся | NO/NC/бистабильность, трубки, обратный клапан, source mapping | насосы выключены, давление безопасно сброшено; одно короткое воздействие с измерением фактической функции | off не гарантирует сброс давления. Уточнить пневмосхему/номиналы/защиту; не менять mapping вслепую |
| Захват timeout/не завершает сценарий | неподтверждённый recipe, sensor/reference, IDs, ошибочные units, фактическое препятствие | phase/job_id/step/evidence/job_error/stop_error; видео; проверить каждый prerequisite отдельно | unknown outcome не повторять. В diagnostic jobs намеренно запрещены.2026 capture не применяется к жгуту по умолчанию и не включает camera/VLM |
| Cancel пришёл, результат SUCCEEDED | job уже подтвердил COMPLETED до отмены | сопоставить job_id/owner/terminal snapshot | допустимая гонка, не «неработающая отмена»; результат нельзя переименовывать в CANCELLED или запускать снова. Cancel всей сессии всё равно оценивают по фактической остановке |
| ROS2 не видит node/service/topic | не source environment, разные ROS_DOMAIN_ID/RMW/сеть, bridge не запущен/упал | `ros2 node list`, `ros2 service list`, `ros2 topic list`; process log; тот же distro/workspace | сначала единая software среда; не менять MCU/Flash из-за DDS. Mac Docker build не означает Mac USB доступен Linux-контейнеру |
| ROS bridge connected=false | неверный Linux device path/права/serial занят; baud/образ | настоящий Linux `/dev/serial/by-id/...`, параметрыport115200 и bridge log; закрыть других владельцев | передать конкретный device/права, не privileged-всё подряд. На Mac сначала native board_test, ROS hardware — поддерживаемый Linux |
| ROS connected, но команды движения игнорируются | diagnostic/uncommissioned; нетarm/token; stale/future stamp; frame неbase_link; publisher conflict | deployment_info37 + state/diagnostics; один publisher; header.frame_id/source stamp/ROSclock | после arm/cancel новая stamp должна быть позже stop+50ms, возраст≤200ms; источник постоянно выдаёт новые stamped сообщения. Статический stamp и raw Twist вместо TwistStamped не подходят |
| Watchdog/reset повторяется | просадка питания; hard fault; превышение времени/стека; reset line; другой firmware/clock | силовая часть off, board_info/reset_cause/uptime до и после, log по фазам включения нагрузок, питание осциллографом при наличии | не отключать watchdog для «лечения». Выделить воспроизводимый безопасный минимальный случай; приложить ELF/map/manifest/log, причину reset не выводить только из мигания LED |

## Как различать отрицательные результаты

`unsupported` — функция/маршрут отсутствует в выбранном профиле или backend; это не повод перебирать каналы под нагрузкой. `invalid_argument` — неверные поля/единицы/диапазон; сначала разобрать команду. `invalid_config` — deployment не обеспечивает безопасную реализацию. `not_armed` в diagnostic ожидаем. `output_failure`/`timeout` после начатого действия могут означать unknown физический исход: только измерение/осмотр закрывают вопрос. Transport timeout и корректный device-level NACK — разные события.

Для ROS normal service NACK ресурса5 не должен автоматически запускать action/release/retry; конфликтующий ресурс остаётся запрещён. У действующего job после MCU fault мост может получить один terminal query-only snapshot, не выдавая actuation authority обратно. Сохранить **original** job/stop errors; успешный более поздний status их не опровергает.

## Пакет данных для разбора

Прислать одну папку конкретного инцидента: rawJSON/текст без обрезания, exit code, manifest/hash/profile, фото актуальной проводки и модели неисправного устройства, видео минимального случая, условия питания, какие нагрузки были отключены и как подтверждена остановка. Не обязательно повторно фотографировать уже подтверждённую MCU распиновку из исходников; нужны отличия фактического жгута/номиналы/неизвестные точки питания и симптом.

Статус после разбора выбирается отдельно: SOFTWARE_READY, READY_FOR_DIAGNOSTIC_FLASH, HARDWARE_CONFIGURATION_REQUIRED, HARDWARE_TEST_REQUIRED либо BLOCKED. Неисправленное физическое поведение не становится PASS из-за повторной успешной компиляции.
