# Сравнение 2025, финальной 2026 и EUROBOT NEXT

**2025 и2026 используют одну аппаратную платформу; год установленной программы не доказывает проводку.** Не требуется считать реальную плату именно2025 или автоматически применять профиль2026. В NEXT сохранены полезные возможности обоих годов. Общий первый запуск — generic BoardDiagnostic в DIAGNOSTIC_MODE, без предполагаемых устройств и автоматического движения. Source-профили2025/2026 выбираются явно только после сопоставления с реальным оборудованием.

Полное восстановление source2025 с таблицей «параметр2025/NEXT/совпадение/проверка»: [ROBOT_2025_HARDWARE_PROFILE.md](ROBOT_2025_HARDWARE_PROFILE.md). В этой таблице2025 — набор фактов исходников, а не утверждение о текущем жгуте. Будущая установка8+8 наPO1…PO16 также требует отдельной проверки.

Текущая редакция включает приложение матча, MCU сценарии захватов, конечные движения, полный полезный набор драйверов и типизированное обслуживание. Программная проверка включает host/ASan/UBSan/C++26, ARM Debug/Release и ROS Jazzy/Lyrical. Точные результаты и границы среза — в [VALIDATION_REPORT.md](VALIDATION_REPORT.md), [TESTING.md](TESTING.md). Сборка и проверки с подставным оборудованием и PTY не подтверждают работу механизмов: аппаратные испытания и прошивка устройства не выполнялись.

## Источники

Сравнивались канонические `01_FIRMWARE_2025/SOURCE/Core` и `02_FIRMWARE_2026/SOURCE/Core`. Их C-файлы побайтово совпадают с соответствующими PROJECT-копиями: всего 84 файла. Локальная папка 2025 содержит дополнительно PumpOn/PumpOff; это не функция канонической 2025. Реестр 603 определений и SHA256 приведён в [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md), [legacy_source_manifest.csv](legacy_source_manifest.csv). Неисправная ранняя распаковка 2026 с нулевыми файлами не считается самостоятельной версией.

Пути ниже отсчитываются от исходной папки Eurobot рядом с EUROBOT_NEXT. Старое приложение C не включается в новый прикладной source tree. CMSIS/FreeRTOS остаются отдельными официальными зависимостями. Права на recovered application и производный материал требуют отдельного решения владельца: [LICENSE-STATUS.md](../LICENSE-STATUS.md).

## Детальная аппаратная разница и класс изменения

Обозначения: `S25/`=`01_FIRMWARE_2025/SOURCE/Core/`, `S26/`=`02_FIRMWARE_2026/SOURCE/Core/`. Источники находятся рядом с EUROBOT_NEXT и остаются только для чтения. Классы: **ПО** — другой программный механизм при тех же цепях; **ПАР** — иной параметр/адрес; **ПРОВ** — потребуется изменение/подтверждение проводки; **ДОП** — новое устройство; **РИСК** — неверный выбор может управлять другим устройством/неправильно двигать механизм. Классы могут сочетаться. «ПРОВ» не означает требование перепаять уже рабочуюплату ради диагностического запуска.

| Компонент | Конфигурация2025 и источник | Конфигурация2026 и источник | EUROBOT_NEXT | Класс изменения |
|---|---|---|---|---|
| MCU / карта памяти |F407VGTx/Flash1MiB/SRAM128KiB/CCM64KiB;2025`.uvprojx:17`|То же в2026`.uvprojx:17`|Та же target-платформа; конкретный MCU проверяется|ПО; без вывода о физической замене |
| HSE/PLL/шины |HSE16MHz,M8/N168/P2,Q4;APB42/84;`S25/BSP/board_hw_config.c:77`|Те же;LSI enable добавлен;`S26/BSP/board_hw_config.c:78`|Те же SYSCLK, исправленQ7;IWDG|ПО/ПАР, измерениеclocks |
| GPIO naming |Полный `S25/BSP/Include/board_gpio_defines.h:20`…`:147`|Текст карты совпадает|Номера разъёмов можно сверять с плакатом владельца|Одинаковая карта не доказывает одинаковый жгут |
| USART1 host |PA9/10,9216008N1;DMA;`S25/BSP/board_hw_config.c:131`|Те же;`S26/BSP/board_hw_config.c:168`|115200/versioned framing/ROS typed bridge|ПО/ПАР, настроить хост; не менять hostpins |
| UART/DMA routing |USART1:DMA2S7/5CH4;USART3:DMA1S3/1CH4;UART4:DMA1S4/2CH4;UART5:DMA1S7/0CH4;USART6:DMA2S6/2CH5;`S25/BSP/board_hw_config.c:148`…`:389`|Те же routes|RX rings/IRQ дляhost/debug, bounded pollingdevicebus|ПО, перепайка от этого не требуется |
| IRQ/timers |USART IRQpriority3,TIM1_CC1,DMALED5,TIM14timeout3;`S25/BSP/board_hw_config.c:41`|Те же основные номера|Host/debug6,stepserviceTIM1UP4,TIM2deadlines/IWDG|ПО, новая RTOS ownership/priority policy |
| Maxon |USART6/2Mbit,ID0x21/0x22/0x23;`S25/App/bus_device.c:27`|ID0x22/0x21/0x23;`S26/App/bus_device.c:27`|Кандидат2025 хранит2025порядок;2026профиль отдельно|ПАР/РИСК: перепутанные wheel indices |
| Геометрия/редуктор |L0.1075,r0.03,gear21,ticks4096;`S25/App/kinematics.c:31`|L0.1217,r0.0295,gear28,ticks4096;`S26/App/kinematics.c:30`|Обе геометрии отдельно; явныйsource-кандидат2025|ПАР/РИСК: другаяdistance/TPM/heading |
| Матрицы/odometry corrections |2025omni rows +L,+L,+L;нет empirical coefficients;`S25/App/kinematics.c:42`|Другое расположение/−L иcorrections0.988/1.003/0.984/0.997;`S26/App/kinematics.c:36`|Явный layout2025/2026;не смешиватьcalibration|ПАР/РИСК |
| BMS |UART4/2Mbit ID0x11;`S25/App/bus_device.c:30`|То же;`S26/App/bus_device.c:30`|Неопределённая установка не опрашивается автоматически;typed read|ПО; фактическое устройство проверить |
| Dynamixel2025grip |USART3 ID103/102;0°,100→50→0;`S25/App/bus_device.c:31`;`OS_Tasks/gripper_task.c:52`|Другиеmain/side/cursor/pinion IDs и рецепты;`S26/OS_Tasks/gripper_task.c:138`|2025ops и2026ops выбираются profile;общийдиагностическийпрофильнеприсваиваетустройства2025/2026|ПАР/ДОП/ПРОВ/РИСК |
| Primary2026main |Нет этой привязки|Source254/112;254broadcast, реальный адрес112;`S26/OS_Tasks/gripper_task.c:141`|Явная active-mask выбирает112;второйslotтолькоявно;не применяется к2025AX103/102|ПАР/ДОП/РИСК |
| PCA/I2C2 |PB10/11 толькоdefinitions;в2025GPIO/maininitнетI2C2/PCA;`S25/App/main.c:64`|GPIOI2C2+initPCA0x40/50Hz;`S26/BSP/board_gpio_config.c:326`;`App/main.c:71`|Драйвересть;кандидат2025 PCAdisabled/unknown|ДОП/ПРОВ/ПАР; source2026I2Cclock исправлен |
| Общие4buttons |PC15/PE6/PE5/PE2,HIGH,PULL_DOWN;`S25/OS_Tasks/buttons_poll_task.c:62`;`App/robot_gpio_config.c:34`|Те же общие функции;4registrations сохранены|Известные2025inputs;sourcepullpolicy сохраняется отдельно|ПО; электрическаясовместимостьобязательна |
| Линейныеконцевики |9registration отсутствуют;PD4/3/7лишьусловныеBIG/GUIDOdefines;`S25/App/Include/robot_gpio_defines.h:27`|ДевятьPC14/PC13/PE4/PE3/PE0/PB7/PB4/PB3/PD3;`S26/OS_Tasks/buttons_poll_task.c:68`|Поддержкаесть;необъявлятьихподключёнными2025|ДОП/ПРОВ/РИСК: неверныйhome |
| Stepperpins |nCSPE7/PD10/PD11/PC9;STEPPE9/11/13/14;DIRPE8/10/12/15;`S25/BSP/l6474_drvr.c:38`|То же в2026|Сохранены;механизмканалаизpinmapневыводится|Одинаковыецепи,разнаямеханикавозможна |
| Steppercurrent/mode |Все800mA,1/16;`S25/BSP/stepper.c:64`|Fullstep;1100/1500/1100/1100mA;`S26/BSP/stepper.c:64`|Кандидат2025mode4/TVAL24;параметрынеактивируютвыходы|ПАР/РИСК: ток/шагизменяютход/нагрев |
| L6474ALARM/CONFIG |3/0x7c03/TOFF31 motor1;0x87/0x2883 motor2…4;`S25/BSP/stepper.c:53`|Те же регистры доmode/currentdiff|Явныеprofilefields,restoreafterreset|ПАР;OCD/hardwarecurrentотдельнонеугаданы |
| Lift |2025StepDemoприсваиваетsoftwarezero,неhome;`S25/OS_Tasks/demo_task.c:104`|Liftstepper1,25steps/mm,1100/320mA,Switch2home;`S26/OS_Tasks/gripper_task.c:83`;`OS_Tasks/stepper_task.c:61`|Общиеboundedstepper+homing,2026recipeпозволенприreviewedmechanics|ДОП/ПАР/ПРОВ/РИСК;25steps/mmне2025факт |
| Насосы/клапаны2025 |Нетcanonicalmapping;localPumpOn/PumpOffобаSETPO1/2:локальный`terminal_cmd_handlers.c:214`,`:226`|4общиепары:PO1/3/5/7 и2/4/6/8;`S26/OS_Tasks/gripper_task.c:147`|Кандидат2025routesunknown/off;функциональная8+поддержкаесть|ПАР/ПРОВ;невыводить2025жгутиз2026 |
| Будущие8+8 |Исходникинеподтверждают|Дваgripperпрофиляделяттеже4пары,не8независимых|ПредлагаетсяPO1…16толькоотдельнойустановкой/profile|ДОП/ПРОВ/ПАР;powerbudgetобязателен |
| PWM/DC |8PWM50Hz,2HbridgesPWM1/2,3/4;`S25/BSP/pwm.c:29`;`Lib/h_bridge.c:29`|Базовыедрайверытеже|Групповыеconflictsиelectricalmodeявные;allowlistsdefaultoff|ПО/ПАР;одниGPIOнеудваиваются |
| LED |PC8TIM8CH3DMA2S1CH7до60LED;`S25/BSP/ws2812b.c:38`|Тотжедрайвер|BoundedDMA/resetgap;непредполагатьфактическуюленту|ПО;питание/levelshiftтребуетпроверки |
| Bootactions |GameautoMaxon,GripperautoTorque,activeSTEPPER_DEMO;`S25/App/main.c:118`;`OS_Tasks/demo_task.c:48`|Другиеbootactions/StepperTask;Demoотключён;`S26/App/main.c:131`|Диагностикабезenable/motion;явныйпереходвoperational|ПО/РИСК;отключитьэнергиюпередSWDктекущейпрограмме |
| Матч/кнопкаmotors |DEBUGdefault/100sтолькоGAME;buttondirectenable;`S25/OS_Tasks/game_task.c:43`;`App/robot_behavior_routine.c:113`|GAME99s,motorbuttonflag;`S26/OS_Tasks/game_task.c:55`|Локальнаяpolicyявная;неautoarmотуровнявхода|ПО/ПАР |

## Аппаратная основа и движение

| Область | 2025 | Финальная 2026 | EUROBOT NEXT |
|---|---|---|---|
| MCU / инструменты | STM32F407VG, Keil/ARMCC | Та же платформа F407VG | CMake/Ninja, Arm GNU, C++23; C++26 отдельно |
| Память | Flash 1 MiB, SRAM 128 KiB, CCM 64 KiB | Те же параметры project device | Раздельные секции, резерв основного стека; Flash/RAM каждого фактического образа приведены в validation report; исходный код и ELF fingerprint связаны |
| Clock | HSE 16 MHz, SYSCLK 168 MHz | То же; PLLQ4 неверен для 48 MHz domain | Q7 для 48 MHz; реальные сигналы требуют стенда |
| UART1 к компьютеру | 921600 baud | 921600 baud | 115200 baud, versioned frame, CRC, owner, sequence |
| Maxon ID в порядке колёс | 0x21,0x22,0x23 | 0x22,0x21,0x23 | Оба порядка доступны явно; generic diagnostic не предполагает подключённые Maxon |
| L / r / gear / ticks | 0.1075 m / 0.03 m / 21 / 4096 | 0.1217 m / 0.0295 m / 28 / 4096 | Две явные геометрии; выбор по реальному шасси, а не году прошивки |
| Матрица кинематики | Собственные порядок и знаки | Отличается от 2025 | Нельзя получить 2025 только заменой радиуса в матрице 2026 |
| Одометрия | Приращение поворачивается конечным углом; диапазон ±2π | Midpoint φ+dφ/2; диапазон ±π | Подход 2026, known/freshness и проверка скачков encoder |
| Поправки 2026 | Нет этого набора | Общий 0.988, X 0.988, Y 1, angular 0.988, колёса [1.003,0.984,0.997] | Параметры сохраняются явно; не выдаются за новую калибровку |
| Конечное движение | Нет развитого набора 2026 | Timed X/Y/yaw, ramp, relative Y→X, poll status, wheel calibration | Отдельные конечные операции с deadline и cancel; проверены portable plans, dispatcher и runtime fake backend; физический путь не измерен |
| PID колёс | Внешние Maxon-платы | Внешние Maxon-платы | Переносимый PID не объявляется проверенным регулятором внешних плат |

MCU указан в `.uvprojx:17` обоих проектов; clocks — `Core/App/system_stm32f4xx.c:52`, `Core/BSP/board_hw_config.c:86`. Геометрия: `01_FIRMWARE_2025/SOURCE/Core/App/kinematics.c:31`, `02_FIRMWARE_2026/SOURCE/Core/App/kinematics.c:30`. Матрица 2026 — тот же файл `:49`, интегрирование — `Core/App/odometry.c:113`, Maxon IDs — `Core/App/bus_device.c:27`. Подробная математика: [CHASSIS.md](CHASSIS.md).

## Захваты и исполнительные устройства

| Функция | 2025 | Финальная 2026 | Новая реализация и граница подтверждения |
|---|---|---|---|
| Общая схема захвата | Top ID103 / bottom ID102; 0° либо цикл 100°→50°→0° | Два логических захвата, пары main/side, отдельные cursor и pinion | Оба набора доступны явно по подтверждённым bindings; generic diagnostic не запускает ни один |
| Main lift AX | Нет трёх позиций 2026 | Up 65°, down 149°, cursor 99°, raw speed 150 | Профиль ID/углов, типизированная операция, feedback и deadline |
| Side grips | Нет профиля 2026 | Closed 54°/155°; open 124°/86° у g1, 123°/86° у g2 | Парная операция; отказ одного привода не скрывается |
| Cursor | Нет этой команды | ID98, raw speed600, 198°/80° | Явное состояние и feedback; нет автоматического torque на boot |
| Pinion slide | Нет этой команды | ID13, 148°+distance×3.979°/mm, clamp 0…300 | Явные scale/range; выход за пределы не становится ложным успехом |
| Vertical lift | Общий API stepper | Stepper1, 25 steps/mm, 1100/320 mA, move/wait/disable | Ограниченный план, reference/homing; неизвестные параметры блокируют пуск |
| Homing | Упрощённый software zero | Switch2, отрицательный поиск, ещё −50, zero, отъезд +1000; нет общего deadline | Только явная команда с подтверждёнными switch, direction, travel и deadline |
| Block rotate | Нет этого профиля | PCA blocks 1…4 / 7…10, 0°/180°, обратный порядок битов mask | Полный spread→rotate→close и отдельные shift/rotate |
| Pumps / valves | Нет mask API 2026; локальный PumpOff ошибочен | Четыре уникальных насоса; два gripper используют общие outputs 1/3/5/7 и valves 2/4/6/8 | До 32 логических каналов; дополнительные физические маршруты не выдумываются |
| Выходная транзакция | SPI shadow без полной синхронизации | All-on перед сбросом нулей mask | Итоговое слово до одного latch; владение общим ресурсом |
| PWM / DC | Драйверы PWM/H-bridge | Те же аппаратные группы | Конфликт целой группы TIM4/TIM5 проверяется; DC mode задаётся явно |
| BMS | Register protocol, копирование struct | 18-byte wire record | Явный decode и known flags; полярность тока не угадывается |
| LED | GRB / PWM DMA; HSV пуст | То же PC8/TIM8 DMA | 60GRB pixels, boundedDMA и настоящий HSV converter; старый HSV был пуст |

Рецепт 2025: `Core/OS_Tasks/gripper_task.c:39`. Профили 2026: `Core/OS_Tasks/gripper_task.c:138`; активные case — `:682`…`:1314`; homing — `Core/OS_Tasks/stepper_task.c:61`. Все углы, каналы, opcodes, фазы и ошибки разобраны в [GRIPPER_SCENARIOS.md](GRIPPER_SCENARIOS.md).

**Открытые аппаратные вопросы:** только в2026 доказаны четыре общие pump/valve пары; canonical2025 не задаёт такой карты жгута. PO1…PO16 — предложенная будущая установка8+8, не доказанная текущая конфигурация; неизвестны токи, бюджет питания, полярность клапанов, ход, реальные пределы и удержание груза при выключении stepper. Наличие сценария не отменяет эти блокировки профиля.

## Транспорт, безопасность и RTOS

| Область | 2025 / 2026 | EUROBOT NEXT |
|---|---|---|
| ACK / completion | Часто accepted либо всегда 1; результат notification timeout игнорируется | Job ID, отдельные accepted/running/result, причина ошибки и cancel |
| Параметры | Ряд handlers читает payload без длины и диапазона | Точная длина, finite числа, masks, IDs, allowlists профиля |
| Владение | Общие flags / gripper_cmd допускают перезапись | Один владелец control, очередь, busy и конфликт ресурсов |
| Boot | Включение torque/Maxon, движения и автоматический homing | Safe init, отдельные claim/arm, проверенный профиль |
| Потеря связи | Нет единой гарантированной политики | Lease, command deadline, локальный stop/disarm; reconnect без replay |
| RTOS | Много задач, tick 1 kHz, часть ожиданий без deadline | Статические Control/Comms/Watchdog, ограниченные транзакции; WCET не измерен |
| Watchdog | В2025 initializer не найден; в2026 вызов initializer отключён | IWDG зависит от progress критических задач; fault-сценарии требуют стенда |
| ROS | Найденный high-level2026 содержит строковый bridge/BT с расхождениями opcodes/payload; его наличие у установленной2025 не доказано | Типизированные interfaces/actions; сборка/тесты Jazzy и Lyrical; аппаратный serial путь требует стенда |
| Аварийная остановка | Старый StopAll неполон | StopSink и локальные выходы; CPU fault не гарантирует немедленный UART stop внешних плат |

## Как выбран high-level источник

В финальном дереве 2026 `bt_package/launch/bt_launch.py:12` задаёт `base_strategy.xml` по умолчанию; аргумент tree_name может выбрать другой файл. Это доказательство default source selection. Имя test/demo/orange само по себе не доказывает, что сценарий был последним на роботе.

Порядок default load/unload записан в GRIPPER_SCENARIOS. Камера и навигация остаются high-level. Найдены расхождения: PickNuts/PlaceNuts отправляют opcodes26/27, отсутствующие в финальной STM32 table; SimpleMoveOn/Off отправляют один byte, а final handler ожидает четыре. Отсутствующие speed/time нельзя восстановить предположением. Сохраняется назначение операций 2026, а ошибочные payload и ложный ACK исправляются.

## Как читать полноту переноса

Реестр 603 определений включает FIFO/IRQ/task glue, пустые routines и условные ветви. Это не 603 независимых требования робота. Каждая строка отдельно описывает перенесённое поведение, заменённый внутренний API, пустую функцию, отсутствующее поведение либо частичную реализацию.

Обновлённый [реестр603 определений](FUNCTIONAL_COVERAGE.md) различает полезное поведение, заменённые внутренние API, пустые тела, demo и неиспользуемые external-memory варианты. Для каждой строки записаны реальные source calls, замена, отклонения, названия тестов и отдельный HIL blocker. Прежние49 MISSING_BEHAVIOR перепроверены по телам и текущим реализациям, а не автоматически повышены из-за успешной сборки.

Уникальных невосстановленных полезных драйверных функций2025 не найдено. После нормализации переводов строк MX/H-bridge/RC/BMS/PWM/L6474 одинаковы с2026; AX2026 добавляет getter32, stepper отличается электрической политикой (2025:1/16/800mA,2026:full-step/1100 или1500mA). Полезное поведение2026 сохраняется в библиотеке возможностей; аппаратный профиль определяется проверенными устройствами, а generic diagnostic не выбираетгод автоматически. Подготовка2025 включала только torque двух AX и теперь требует явной команды; отдельный опасный boot wrapper не нужен.

Различия временной модели указаны явно: runtime odometry20Hz вместо legacy100Hz; time-based debounce8ms с дискретностью Control10ms; pulse service100µs ограничивает реальную частоту шагов. Активные2026 lift/home working divisions10/16 покрываются;19.2kHz workdiv1 из benchmark не объявляется достижимым на новом100µs сервисе. Автоматический2025 demo с фиктивным zero и бесконечными движениями четырёх моторов заменён явными finite упражнениями, а не объявлен перенесённым автономным матчем.

Typed maintenance0x24 охватывает reset/cause, encoder ticks/path/reset, pose, AX/MX speed/LED/torque limit/mode/ID, Maxon range/ID, BMS ID/calibration/независимые чтения, L6474 registers/reset, PWM frequency, PCA controls, GPIO и приложение матча. Disarmed/owned/armed access не позволяет обходить safety gate. Полная таблица — [MAINTENANCE.md](MAINTENANCE.md).

Установка: [INSTALL.md](INSTALL.md). История проверок: [TESTING.md](TESTING.md), [VALIDATION_REPORT.md](VALIDATION_REPORT.md). Образы должны соответствовать source/deployment fingerprint; автоматическая запись на устройство не выполнялась.

## Присланная карта разъёмов

| Функция | 2025 | 2026 | EUROBOT_NEXT | Проверка |
|---|---|---|---|---|
| GPIO/PWM/Stepper | Аппаратные макросы | Карта совпадает с изображениями владельца | Единый board_connectors.hpp используется STM32 adapter | BoardConnectors tests: все28GPIO,13input aliases, PWM4→1 и stepper1 снизу |
|8 насосов +8 клапанов|Нельзя восстановить8 независимых из исходных маршрутов|Два gripper разделяют4 пары|Новая предписанная разводка PO1…PO16, отдельные logical0…3/4…7|BoardInstallation tests, ARM preset stm32-board8-plan; реальный жгут/токи ещё не проверены|
| Сохранение NRST | Биты общего24-bit регистра | StopAll может сбрасывать лишние линии | Partition20PO+4NRST, power updates сохраняют NRST | Unit tests и linked firmware audit |
| Разъём USART3 | Драйвер используетPD8/PD9 | То же; на присланном изображении USART3 не подписан | Драйвер сохранён; USART2 не переименован вUSART3 | Физическую связь DIO сUSART3 нужно установить по цепи |

[BOARD_WIRING.md](BOARD_WIRING.md) содержит подключение каждого подтверждённого разъёма, [board_8_pumps.hpp](../examples/board_8_pumps.hpp) — заготовку нового deployment. Схема не задаёт допустимый ток, питание логики, модели/ID и механические пределы; они не подменяются тестовыми значениями.

## Рабочая конфигурация и обнаружение устройств

Новый [robot_2026](ROBOT_2026_PROFILE.md) автоматически переносит существующие настройки в отдельный собираемый профиль; явные новые области программных команд отмечены отдельно от source-фактов. Он требует live lease/arm и не включает движения при boot. Все primarymain-рецепты выполняются через112 с проверкой его положения; второй слот активируется только при явно заданном дополнительном индивидуальном адресе. К36 полезным операциям обслуживания добавлена новая37-я (`servo_probe`,номер36) для read-only инвентаризации без commissioned deployment. Отдельно проверены actualL6474SPIbytes,homezero0 вместо ошибочной трактовки−100000,derivedwheelbounds и отказ ошибочных активных привязок и реальные байты SyncWrite112/feedback112, включая timeout/ESTOP.
