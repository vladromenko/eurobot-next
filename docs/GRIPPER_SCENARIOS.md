# Сценарии захватов: доказательства и перенос

Сценарии включены в текущие host/sanitizer и ARM сборки; фактические результаты общего прогона — в [VALIDATION_REPORT.md](VALIDATION_REPORT.md). Физическая загрузка и движения не выполнялись.

**Приоритет требований: финальная прошивка 2026 определяет захваты и доступное поведение. Версия 2025 — базовая предыстория; если операция отсутствует 2025, но есть 2026, переносится 2026. Рецепт 2025 не заменяет более поздний 2026.**

Пути ниже отсчитываются от исходной папки Eurobot рядом с EUROBOT_NEXT. Углы AX — градусы 0…300; скорость AX — сырые регистровые значения, а не градусы/с. Задержки vTaskDelay выражены в ticks (при 1kHz равны ms). Числа из legacy — baseline исходников, не измеренные безопасные пределы текущего робота.

## 2025: оба состояния

USART3, верхний AX ID103, нижний ID102 (`01_FIRMWARE_2025/SOURCE/Core/App/bus_device.c:31`). Задача после 50ticks включает torque обоих (`Core/OS_Tasks/gripper_task.c:39`) и далее suspend до команды. Handler `Core/App/terminal_cmd_handlers.c:187` читает первый байт состояния, resume задачи и сразу ACK без completion.

| State | Активный рецепт | Источник gripper_task.c |
|---|---|---|
| 0 | Оба 0° | :52 |
| 1 | Оба 100° →1000ticks→оба 50°→1000ticks→оба 0° | :56 |

Нет проверки длины, результата записи, углового feedback или общего deadline. Новая реализация сохраняет рецепт, но не автоматический torque на boot и не ложный success.

## 2026: аппаратные профили

Источник: `02_FIRMWARE_2026/SOURCE/Core/OS_Tasks/gripper_task.c:138`.

| Параметр | Захват 1 | Захват 2 |
|---|---|---|
| Main AX IDs | **254**,112 | 1,2 |
| Side AX IDs | 7,8 | 16,25 |
| Main start/end/cursor |149°/65°/99°|149°/65°/99°|
| Side closed |54°,155°|54°,155°|
| Side open |124°,86°|123°,86°|
| Profile main/side raw speed |300/900|300/900|
| Block PCA indices (0…15, CH1=1) |1,2,3,4|7,8,9,10|
| Shift PCA channels |5,6|11,12|
| Shift close/start |102°,15°|103°,18°|
| Shift spread/end |17°,112°|16°,112°|
| Pump outputs |1,3,5,7|**те же 1,3,5,7**|
| Valve outputs |2,4,6,8|**те же 2,4,6,8**|

ID254 — broadcast Dynamixel; индивидуальный реальный адрес не подтверждён. Нельзя подставлять вымышленный ID или разрешать движение broadcast. Два logical gripper используют четыре общих физических насоса: восемь независимых маршрутов не доказаны. Профиль должен явно описывать совместное использование либо подтверждённую независимую проводку.

Cursor ID98, speed600, horizontal198°, vertical80° (`gripper_task.c:187`). Pinion ID13, start148°, speed800,3.979°/мм (`:193`,`:82`). PCA address0x40. В legacy enum `Core/Lib/Include/pca9685.h:58` CH0=0 и CH1=1: значения профиля 1…12 уже являются индексами PCA, их нельзя дополнительно уменьшать на 1. Lift stepper1:25steps/мм, normal1100mA, down320mA (`:79`).

## Все активные микросценарии 2026

Op — старый terminal opcode, case — внутренний gripper_cmd. Новые versioned wire opcodes описываются отдельно.

| Op/case | Вход | Порядок активных фаз | Legacy completion |
|---|---|---|---|
|0x1E/0 cursor|state1 horizontal, иначе vertical|AX98 speed600→torque→198°/80°→50ms|WaitAngle1500ms; result ignored; notify|
|0x1F/1 side|gripper1/2,state1 closed, иначе open|Оба side speed900→torque→SyncWrite целей→100ms|Только первый servo,1500ms; result ignored; notify|
|0x20/2 main lift|gripper,state0 up65°,1 down149°,2 cursor99°|Оба main **speed150**→torque→SyncWrite одинаковых целей→200ms|Только первый servo,1500ms; result ignored; notify|
|0x21/3 pumps|gripper,mask4|Pump mask→50ms|Нет вакуумного/токового feedback; notify|
|0x22/4 valves|gripper,mask4|Valve mask→50ms|Нет feedback; notify|
|0x1C/5 fullrotate|gripper,mask4|Spread pair→200ms→block1…4 по mask с 20ms после каждого→500ms→close pair→200ms|PCA только запись/время; task не notify|
|0x2B/10 shift only|gripper,spread bool|Shift pair end/start→200ms|Notify, без механического feedback|
|0x2C/11 rotate only|gripper,mask4|block1…4 по mask с 20ms после каждого→500ms|Notify, без механического feedback|
|0x25/6 timedbase|axis/gripper1…4,dir0/1,speedByte,timeByte|v=speedByte×0.01;T=timeByte×500ms;1→X,2→Y с ramp;3→X,4→yaw без ramp; затем 0|ACK немедленно, не completion; нет pose feedback|
|0x27/7 vertical|heightByte мм|target=height×25;если ниже current320mA,иначе 1100mA→enable→10ms→move(64,10,200)→wait≤4000ms→disable→1100mA→10ms|Software pos strict error<75steps; timeout ignored; notify|
|0x29/8 relative|dx/dy signedLE32 мм,speedByte см/с|Maxon enable→20ms→Y→при успехе X→stop→100ms|last_cmd_status; terminal ACK=accepted|
|0x2D/12 pollrelative|тот же payload|Как case8, итог move_status_flag|1=success;0 смешивает running/failure; read0x2E|
|0x2F/13 slide|signedLE32 мм|148+trunc(distance×3.979),clamp0…300→AX13 speed800/torque/target→50ms→wait1500ms|last_cmd_status используется handler; notify|
|0x28/9 wheeltask|wheel1…3|stopall→20ms→reset выбранных ticks→20ms→TPM400000→poll5ms до abs(ticks)≥114688−50 или 100000ms→TPM0→20ms→readticks→restore disabled|Внутренний case; terminal handler имеет другой алгоритм|

Ссылки на gripper_task.c: cursor`:682`,side`:705`,main`:834`,pumps`:965`,valves`:974`,fullrotate`:983`,shift`:1013`,rotate-only`:1021`,timed`:1029`,vertical`:1124`,relative`:1157`,pollrelative`:1189`,slide`:1217`,wheel`:1247`.

Block index i0…3 использует bit(3−i), target0°/180°: mask1000 поворачивает первый block. Pump/valve mask использует bit i по порядку массива. Эти нумерации различаются. Новый интерфейс должен отвергать mask>15.

## Terminal handlers и ошибки

Handlers находятся в `02_FIRMWARE_2026/SOURCE/Core/App/terminal_cmd_handlers.c`: fullrotate`:538`,shift`:556`,rotate-only`:573`,cursor`:590`,side`:613`,main`:636`,pumps`:660`,valves`:682`,timed`:744`,relative`:765`,pollrelative`:810`,status`:834`,vertical`:859`,slide`:1014`. Opcode table: `Core/App/Include/terminal_cmd_table.h:60`.

WaitForGripperCompletion (`:130`) ждёт 5000ticks и игнорирует возврат. Cursor/side/lift/pump/valve/vertical возвращают success1 также при COMM_ENABLE0 или timeout. Большинство handlers не проверяют payload length и gripper range; active_hw может быть NULL. Relative принимает≥9bytes только idle; slide≥4bytes только idle. Fullrotate/shift/rotate-only/timed ACK немедленно. Новый jobID и accepted/running/succeeded/failed/canceled должны различать приём и фактическое завершение.

WaitDynamixelAngle (`gripper_task.c:463`) опрашивает 20ms, strict abs(error)<10°, default1500ms; ошибка GetAngle игнорируется, stale/нулевое значение может дать ложный успех. WaitStepper1Angle (`:493`) использует software counter выданных шагов, не encoder; strict error<75steps,20ms. Совпадение counter не доказывает механическое движение.

Pump/valve helpers (`:433`,`:446`) сначала включают все 4, потом гасят нули с 10ms задержками. Новая маска вычисляется до одного latch; этот переход не переносится. PCA не предоставляет позиционную обратную связь. Force-centering по нагрузке (`:732`) и lift impact guard (`:872`) **закомментированы**, не являются активными рецептами.

## Homing

`02_FIRMWARE_2026/SOURCE/Core/OS_Tasks/stepper_task.c:47` инициализирует stepper1/zero. Calibration (`:61`): enable→seek−100000 с(32,16,500) до linear_switch2 pressed→ещё current−50 с(32,10,500)→100ms→stop→zero→+1000 с(32,10,300). Общего deadline/path limit нет. При достижении цели драйвер выключается. Gripper boot автоматически ставит calibration (`gripper_task.c:611`).

Это дополнительный ход внутрь switch, не безопасный default. Новая homing-команда требует подтверждённых input/polarity/direction, travel limit/deadline и политики освобождения/отъезда. Неизвестные значения блокируют действие. Авто homing на boot не переносится. Software zero не заменяет физический reference.

## Relative axis и timed math

`gripper_task.c:292`: dxlocal=dXcosφ0+dYsinφ0; dylocal=−dXsinφ0+dYcosφ0. Axis (`:306`): tolerance3mm,minspeed0.002m/s,accel0.2m/s²,vprofile=min(vmax,max(vmin,sqrt(2a|error|))). Разгон прибавляет a×0.02 на цикл при delay10ms. Direction фиксируется target sign и не меняется после overshoot. Deadline=(|target|/speed)×3500+3000ms; stop+80ms в конце.

XheadingP2.2 clamp±0.4rad/s. Y:2×atan(clamp(−dx/max(|error|,0.05),±0.8)),clamp±0.4 вместо headinghold. Relative делает Y затем X; **каждая ось заново снимает стартовую pose**. Это отличается от комментария про единый command-start frame. Новая реализация должна явно определить frame, feedback freshness, finite limits, overshoot/stall, cancel и deadline.

Timed ramp (`:256`):20ms обновления,ramp=min(total,350ms),scale=elapsed/ramp; rampdown нет. Реальные byte units0.01m/s(для yaw0.01rad/s) и 500ms вопреки комментариям mm/s и ms.

Wheel calibration имеет два алгоритма: реальный terminal handler (`terminal_cmd_handlers.c:984`) вызывается таблицей напрямую и не устанавливает case9; constants`:102` дают 15s timeout и 1750ticks margin. Taskcase9 использует 100s/50ticks margin. Новая calibration требует выбранного профиля скорости/ticks/deadline/margin, а не молчаливого выбора старой ветки.

## Финальный источник и составные high-level рецепты

`03_HIGH_LEVEL_2026/eurobot2026-main/modules/bt/bt_package/launch/bt_launch.py:12` выбирает `base_strategy.xml` по умолчанию; launch argument tree_name позволяет другой файл. Это объективный приоритет источника перед именами test/demo. Факт последнего запуска на роботе из дерева не восстанавливается.

Default `trees/base_strategy.xml:69` Load: SideOff→300ms→GripOn→300→SideOn→300→SideOff→300→ValvesOn→300→PumpsOn→300→SimpleMoveOn→2500→SideOn→300→GripOff→300→SimpleMoveOff→2500→SendImage→300→SideOff→300. Unload`:102`: GripOn→300→PumpsOff→300→ValvesOff→300→ValvesOn→300→GripOff→300→RotateBlocks0000→4500→SendImage→300.

Однако SimpleMoveOn/Off nodes (`src/nodes/simple_move_on_node.cpp:16`, `simple_move_off_node.cpp:22`) отправляют только один payload byte0/1, тогда как финальный STM32 handler ожидает 4bytes[gripper,dir,speed,time]. Default BT и final firmware несовместимы в этой операции. Из BT нельзя придумывать отсутствующие speed/time. Порядок механических фаз документируется, численные параметры движения должны быть явными в deployment. Камера и навигация остаются high-level. Статус совместимости не повышается по имени XML.

Следующие файлы — исторические варианты внутри 2026; они не переопределяют final firmware и default launch без явного выбора:


Путь BT: `03_HIGH_LEVEL_2026/eurobot2026-main/modules/bt/bt_package`. Это версии матчевых сценариев, не универсальный безопасный capture/release.

- `trees/demo_tree_1.xml:127` Load: SideOff→300ms→GripOn→300→SideOn→300→SideOff→300→ValvesOn→300→PumpsOn→300→SimpleMoveOn→2500→GripOff→300→SimpleMoveOff→2500→SendImage→300.
- Тот жефайл`:155` Unload: GripOn→300→PumpsOff→300→ValvesOff→300→ValvesOn→300→GripOff→300→RotateBlocks0000→4500→SendImage→300.
- `trees/eurobot_simple_orange.xml:170` LoadG2: SideOff→GripOn→ValvesOn→PumpsOn→SideOn→SideOff→Move+Y7cm/s1s→1300ms→SideOn→GripOff→Move−Y7cm/s1s→1300ms→SideOff. G1`:136` без последнего SideOff, в ForceSuccess.
- Тот жефайл`:189`,`:204` UnloadG1/G2: GripOn→SideOff→PumpsOff→ValvesOff→ValvesOn→GripOff→RotateBlocks0000→500ms;G1 в ForceSuccess.
- `trees/test_blue2.xml:699` LoadNew: VerticalGrip0mm(timeout4s)→ValvesOn(g1)→PumpsOn(g1)→Move+X16cm/s500ms→700ms. Подъём 35mm закомментирован.

`src/nodes/pick_nuts_node.cpp:31` отправляет grab_on26/0x1A, `simple_place_nuts_node.cpp:64` opcode27/0x1B; в финальной STM32 table таких entries нет. Это не доказательство работающей составной операции. Навигация/камера/ForceSuccess не переносятся автоматически внутрь firmware. Выбранный именованный рецепт должен иметь типизированные фазы; ошибка не скрывается в safety path.

## Приёмка после разрешённой сборки

Проверить bounded phases, единицы/маски, общий deadline/cancel, отказ неизвестных IDs/channels и владение sharedpump. Новые файлы тестов получают UNIT_TESTED только после выполнения. Затем проверить каждую фазу с отказом транспорта/stalefeedback/timeout/cancel; отдельно ARMlink, ROScolcon и HIL. Стенд требует реальных IDs/polarity/travel, проверки удержания груза при disable и аппаратного отключения энергии.

## Что написано в новой редакции

`firmware/include/eurobot/gripper.hpp` и `firmware/src/gripper.cpp` задают ограниченный Engine, максимум64 фазы. `runtime.cpp` связывает его с настоящими драйверами и отдельными guard/validate/issue/feedback/stop callbacks. Все микросценарии case0…13 представлены; capture/release используют явно выбранный `Composite::base_strategy_2026`, prepare вызывается только командой. Исторические операции2025 доступны отдельно. Поведение2026 является основным.

Factory `legacy_2026_reference` сохраняет source angles, AX IDs, PCA indices и единицы. `reviewed=false`; main ID254 заменён на unassigned, а не на вымышленный ID. Логические pump/valve routes и неизвестные lift/motion limits не назначаются автоматически. В deployment нужны общие allowlists и два отдельных reviewed gripper config. PCA CH1=1, уже готовый индекс: вычитать1 нельзя.

Парные AX теперь проверяют оба feedback и moving/error, вместо первого привода с игнорируемой ошибкой. Lift использует точные referenced steps; software counter не объявляется измеренным положением. Удержание/disable задаётся проверенной политикой, поскольку отключение тока может потерять reference и уронить груз. Slide вычисляется с float precision и отвергает выход за reviewed bounds вместо молчаливого clamp. Timed движение использует явные SI speed/axis/duration; старые aliases и ошибки единиц не сохраняются. Relative движение исправляет знак после overshoot и использует фактический dt. Wheel calibration считает относительную encoder delta без разрушительного сброса внешних totals.

Принятый finite plan заранее определяет все явно упомянутые ресурсы. Их уже активные удерживающие состояния продлеваются только пока действуют owner/lease/job deadline; выключенный выход не включается от продления. Это позволяет выполнить OFF позднее в том же плане и не продлевает посторонние устройства. При завершении job автоматическое продление прекращается, действует обычный250ms deadline. Набор примитивов не удерживает ресурсы между job автоматически: для непрерывного удержания нужен проверенный composite. Программная остановка не заменяет аппаратное снятие энергии.

Wire0x21 submit имеет22-byte payload; query0x22/cancel0x23 используют job ID, ACK25 содержит отдельные phase/error/stop error/evidence. Точный контракт и Mac клиент: [MACOS_BOARD_TEST.md](MACOS_BOARD_TEST.md). ROS action `eurobot_interfaces/action/Gripper` связывается с тем же job, без отдельного скрытого пути управления.

Свежие C++/ARM/ROS результаты фиксируются общим прогоном. Mac PTY26/26 и flash mocks24/24 проверяют инструменты без физического оборудования. Профильные аппаратные блокировки сохраняются после успешной программной проверки.
