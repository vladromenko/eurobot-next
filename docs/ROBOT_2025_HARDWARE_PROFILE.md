# ROBOT_2025_HARDWARE_PROFILE — кандидат аппаратной конфигурации2025

Плата2025/2026 — одна аппаратная платформа STM32F407. Год установленной программы **не определяет** жгут, комплект устройств, адреса, механику или пригодность профиля. Этот документ восстанавливает именно настройки исходников2025 как **отдельный source-кандидат**, а не утверждает, какая прошивка сейчас установлена и какая конфигурация обязательна для реального робота.

Общий первый диагностический образ **BoardDiagnostic** не выбирает2025 или2026 автоматически и не предполагает подключённые устройства. Полезные возможности обоих годов сохраняются в NEXT и включаются по подтверждённой фактической конфигурации. Этот2025-кандидат можно выбрать только явно, если проверка установленных устройств подтверждает его параметры.

Различаются четыре сущности: возможности firmware; восстановленные настройки исходников2025; фактически установленное оборудование (не установлено по году); будущие расширения. Предложенная ранее разводка8+8 на PO1…PO16 — **будущее расширение**, не доказанная текущая проводка и не автоматический профиль первого пуска.

## Источники и статус доказательств

Здесь `S25/` означает `01_FIRMWARE_2025/SOURCE/Core/`, `S26/` — `02_FIRMWARE_2026/SOURCE/Core/`; оба дерева находятся рядом с EUROBOT_NEXT и остаются только для чтения. `NEXT/` означает собственное дерево EUROBOT_NEXT. Ссылки вида `S25/BSP/board_hw_config.c:131` задают файл и строку оригинала. Приведённые числа восстановлены чтением, не аппаратным измерением. Сборки, USB, SWD и чтение устройства в рамках этого аудита не выполнялись.

`S25/BSP/Include/board_gpio_defines.h` полностью совпадает с2026 после нормализации перевода строк; также совпадают `BSP/spi_gpo.c`, `BSP/Include/spi_gpo_defines.h`, `BSP/pwm.c`, `BSP/ws2812b.c`, `App/stm32f4xx_it.c`. Однако **определение контакта, его GPIO-инициализация и активная прикладная функция — разные доказательства**. Например, I2C2 определён в обоих заголовках, но включается в main/GPIO только2026; девять линейных switches опрашиваются только2026.

Карта разъёмов владельца применима как карта показанной платы: [общая](images/stm32-board-connectors.jpg), [деталь](images/stm32-board-detail.jpg). Красная обводка смысла не имеет. Подробности контактов: [PINOUT.md](PINOUT.md), [BOARD_WIRING.md](BOARD_WIRING.md). Карта не доказывает, какие устройства сейчас подключены. Сеть USART3↔правый DIO на карте явно не обозначена.

## Параметры2025 и соответствие NEXT

В колонке NEXT различаются общая реализация и **кандидат2025**. Указание значения кандидата не является разрешением включения: диагностический профиль не вооружается. Подтверждённые исходниками параметры не нужно получать повторно от владельца; физически сверяются только их соответствие установленной плате/жгуту и неизвестные пределы.

| Параметр | Прошивка2025: факт и источник | EUROBOT_NEXT | Совпадает? | Требует проверки? |
|---|---|---|---|---|
| MCU |STM32F407VGTx, Keil `01_FIRMWARE_2025/PROJECT/eurobot_2025_main_board/MDK-ARM/Main_Board.uvprojx:17`|F407VG, Cortex-M4F hard-float|Да по проекту|Маркировка конкретного MCU и device/Flash-size |
| Память |Flash1MiB0x08000000; SRAM128KiB0x20000000; CCM64KiB0x10000000; тот же `.uvprojx:21`|Та же карта; DMA буферы внеCCM|Да|Read protection/custom bootloader и доступность backup |
| HSE |16MHz в `S25/App/system_stm32f4xx.c:52`; runtime PLL рассчитан на16 в `S25/BSP/board_hw_config.c:77`|HSE16MHz|Да по runtime|Реальный кварц; Keil metadata CLOCK25000000 в uvprojx:21 противоречит runtime и не заменяет измерение |
| SYSCLK / шины |PLLM8/N168/P2; SYSCLK/AHB168MHz; APB1/2=42/84MHz (`S25/BSP/board_hw_config.c:78`)|Те же SYSCLK/шины; timer clocks84/168MHz|Да|Clock-output/скорости на стенде |
| PLLQ |Q4: домен48M фактически84MHz (`S25/BSP/board_hw_config.c:79`)|Q7,48MHz; USB не становится активным от одного PLL|Намеренно исправлено|Не использовать legacy clock как доказательство работающего USB |
| Host UART |USART1 PA9TX/PA10RX AF7 (`S25/BSP/Include/board_gpio_defines.h:20`),921600/8N1/oversampling8 (`S25/BSP/board_hw_config.c:131`)|Те же пины, **115200/8N1**, новый framed protocol|Пины да; baud/protocol изменены|Настройки USB-UART/ROS; старый клиент несовместим |
| Servo UART |USART3 PD8 data/PD9 DE (`S25/BSP/Include/board_gpio_defines.h:26`);half-duplex1Mbit/s (`S25/BSP/board_hw_config.c:182`)|Та же шина/Dynamixel1|Да|Физический разъём, модели, питание и реальные IDs |
| Maxon UART |USART6 PC6 data/PC7 DE (`S25/BSP/Include/board_gpio_defines.h:35`);2Mbit/s (`S25/BSP/board_hw_config.c:357`)|Та же шина; карта:RS485 USART6|Да|A/B/reference/терминация и питание внешних контроллеров |
| Maxon IDs по индексам |**0x21,0x22,0x23** (`S25/App/bus_device.c:27`)|Кандидат2025 должен использовать этот порядок; default2026 —0x22,0x21,0x23|Требуется профиль2025|Не переносить2026swap без проверки колёс |
| Геометрия |L0.1075m,r0.03m,gear21,ticks4096 (`S25/App/kinematics.c:31`)|`legacy_2025_geometry()`; отдельно поддерживается2026|Да при выборе2025|Фактические размеры/редукторы/знаки и единицы внешнего encoder |
| Матрица шасси |Ряды(+cos30,+0.5,+L),(-cos30,+0.5,+L),(0,-1,+L), `S25/App/kinematics.c:42`|Отдельный layout2025 без empirical2026coefficients|Да при выборе2025|Соответствие физических колёс индексам |
| BMS |UART4 PC10 data/PA15 DE,2Mbit/s (`S25/BSP/Include/board_gpio_defines.h:29`;`BSP/board_hw_config.c:249`);ID0x11 (`S25/App/bus_device.c:30`)|Протокол/пины поддерживаются; опрос только явно настроенного устройства|Да по source|Есть ли BMS, её питание/число ячеек/знак тока |
| UART5 |PC12TX/PD2RX,115200 (`S25/BSP/board_hw_config.c:304`), исходная инициализация толькоRX (`:309`)|Опционально новый framed TX/RX, default выключен; общий owner|Пины/baud да; режим иной|Нужен ли интерфейс и подключён ли он |
| USART2 |PD5 data/PD6DE объявлены (`S25/BSP/Include/board_gpio_defines.h:23`), нет активного USART2 init в main55…85|Не активный actuator bus|Да|Не подменять имUSART3/Dynamixel |
| SPI1 |PA5SCK/PA6MISO/PA7MOSI, PA4STCP (`S25/BSP/Include/board_gpio_defines.h:59`);mode3,prescaler2 (`S25/BSP/board_hw_config.c:414`)|Те же сигналы/режим; общий Control owner|Да|Реальный latch/byte order и электрические уровни |
| L6474 SPI |Та жеSPI1, prescaler32 на transaction (`S25/BSP/l6474_drvr.c:49`)|Те же nCS/шина, bounded transactions|Да|Драйвер/обмотки/токи и timing |
| Силовые выходы |20PO во внешнем24-bit регистре; PO1…20→bits20,21,18,19,22,23,10,11,14,15,12,13,6,7,4,5,2,3,1,0 (`S25/BSP/Include/spi_gpo_defines.h:18`)|Тот же перевод; это **доступные выходы**, не список подключённых насосов|Да|Назначение каждого фактического провода, нагрузки, токи |
| NRST L6474 |В регистре bits8,9,17,16 (`S25/BSP/Include/spi_gpo_defines.h:39`)|Сохраняются при PO mask update|Да|Физическая реализация reset и безопасное снятие holding |
| Stepper nCS1…4 |PE7,PD10,PD11,PC9 (`S25/BSP/l6474_drvr.c:39`)|Те же nCS|Да|Назначение Stepper Output каждому механизму |
| Stepper STEP/DIR |STEP PE9/11/13/14;DIR PE8/10/12/15 (`S25/BSP/Include/board_gpio_defines.h:39`)|Те же выводы; генерация GPIO из bounded TIM1 service|Пины да; backend иной|Пары обмоток, направления и travel |
| Режим шага / ток |Все4 —1/16 и800mA (`S25/BSP/stepper.c:64`,`:86`,`:108`,`:130`)|Кандидат2025: logical mode4,encoded0x8c,TVAL24; не включает driver автоматически|Source-настройки сохраняются отдельно|800mA — запрос source, не измеренный безопасный ток конкретного мотора |
| L6474 ALARM/CONFIG/TOFF |Motor1 ALARM3/CONFIG0x7c03/TOFF_MIN31;motor2…4 ALARM0x87/CONFIG0x2883/TOFF_MIN не записан (`S25/BSP/stepper.c:53`,`:78`,`:100`,`:122`)|Поля профиля и подготовка после reset; sentinel для незаданногоTOFF|Да по выбранному профилю|OCD threshold в этих init не задан; не выбирать произвольный предел |
| Current quantization |TVAL=floor((mA−31.25)/31.25),800→24 (`S25/BSP/l6474_drvr.c:199`)|Та же units/conversion, проверка диапазона|Да|Реальное соответствие sense hardware/току |
| TIM1 |База19200Hz, PWM CH1…4, TIM1_CC IRQ (`S25/BSP/board_hw_config.c:37`,`:425`,`:485`)|TIM1 update100µs, software planner с source-style ramp|Функционально заменён|Peak rate ограничен новой службой;19.2kHz изdemo не объявляется физически достижимой |
| PWM1…4 |PD15/14/13/12→TIM4 CH4/3/2/1 (`S25/BSP/pwm.c:29`);50Hz,tick1µs (`BSP/board_hw_config.c:540`)|Та же группа/пины; явные allowlists|Да|Частота/импульсы/питание каждого servo; конфликт DC |
| PWM5…8 |PA0/1/2/3→TIM5 CH1/2/3/4 (`S25/BSP/pwm.c:30`);50Hz (`BSP/board_hw_config.c:578`)|Та же группа|Да|Те же механические/силовые ограничения |
| H-bridge |ПарыPWM1/2 и3/4 (`S25/Lib/h_bridge.c:29`);positive=(duty,100),negative=(100,abs) (`:46`)|Явный `legacy_high_pair` либо другой reviewed electrical mode|Только при явном выборе|Фактическая схема моста; не заменить coast_pwm автоматически |
| I2C2/PCA |PB10/11 объявлены (`S25/BSP/Include/board_gpio_defines.h:90`), но GPIO/main не включают I2C2/PCA (`S25/App/main.c:64`)|PCA optional; source2025 не подтверждает присутствие|Новая возможность|Физически есть ли PCA, питание/OE/каналы |
| I2C1/SPI2/SPI3 |Разъёмы определены (`S25/BSP/Include/board_gpio_defines.h:70`); нет законченного активного приложения|Периферия не включается по одному заголовку|Да по отсутствию auto-активации|Подключённые устройства и конфликт aliases |
| LED |PC8 TIM8CH3, DMA2stream1channel7 (`S25/BSP/board_hw_config.c:490`);до60LED (`S25/BSP/ws2812b.c:38`)|Те же pin/DMA; GRB/reset gap; ARR209 вместо210|Пины да; waveform уточнён|Наличие/длина ленты, питание/level shifting |
| Buttons |4 регистрации:cordPC15,sidePE6,strategyPE5,motorPE2;HIGH polarity (`S25/OS_Tasks/buttons_poll_task.c:62`)|Эти4 известны; поддержка13 входов не доказывает9концевиков|Карта4 да|Состояния реальных контактов; не назначать home автоматически |
| Input pulls |Эти4 INPUT/PULL_DOWN (`S25/App/robot_gpio_config.c:34`…`:68`)|Явный source-кандидат2025 сохраняет PullDown0x0F; общий BoardDiagnostic этих pulls не предполагает|Нужна совместимая политика|Наличие внешних подтяжек и реальные уровни без движения |
| Условные входы |PD4/PD3/PD7 дляROBOT_GUIDO/ROBOT_BIG (`S25/App/Include/robot_gpio_defines.h:27`, `App/robot_gpio_config.c:74`)|Не объявляются активными9limits2026|Нет доказательства активной ветви|Действительный вариант и жгут; заголовок не равен registration |
| Насосы/клапаны |Каноническая2025 не задаёт соответствие pump↔PO/valve↔PO; `S25/OS_Tasks/gripper_task.c:39` и `App/terminal_cmd_handlers.c:187`|Неактивные неизвестные routes; поддержка8+ доступна после явной привязки|Неизвестно|Установлены ли нагрузки и куда подключены;4пары2026 не копировать |
| Lift/home |2025 demo присваивает software zero четырёммоторам (`S25/OS_Tasks/demo_task.c:104`); измеренного home нет|Ограниченный homing только по reviewed input/travel; никаких boot moves|Намеренно изменено|Реальный lift, концевики/направление/steps-per-mm/границы |
| Boot |Активны GameTask/DemoTask/GripperTask (`S25/App/main.c:118`);автоenable Maxon/AX/stepper|DIAGNOSTIC_MODE не повторяет unsafe boot|Намеренно изменено|Отдельное снятие энергии **до SWD к текущей программе**, которая может запуститься |

## DMA, IRQ и таймеры: аппаратные ресурсы2025

Все номера ниже взяты непосредственно из `S25/BSP/board_hw_config.c`, а не перенесены из2026. TX/RX обозначают направление STM32. Совпадение register map не требует сохранения старой архитектуры DMA в NEXT.

| Ресурс2025 | DMA / IRQ / настройки | Источник2025 | NEXT |
|---|---|---|---|
| USART1 TX/RX |DMA2 S7/S5,CH4;USART1 IRQ priority3|`S25/BSP/board_hw_config.c:148`,`:160`,`:168`|RX IRQ ring и bounded TX polling;USART1 priority6|
| USART3 TX/RX |DMA1 S3/S1,CH4;USART3 IRQ priority3|`S25/BSP/board_hw_config.c:203`,`:215`,`:232`|Bounded half-duplex polling;DE снимается послеTC|
| Дополнительный DMA M2M |DMA2 S0,CH1|`S25/BSP/board_hw_config.c:223`|Legacy transport internal glue заменён |
| UART4 TX/RX |DMA1 S4/S2,CH4;UART4 IRQ priority3|`S25/BSP/board_hw_config.c:267`,`:279`,`:288`|Bounded polling |
| UART5 TX/RX |DMA1 S7/S0,CH4;UART5 IRQ priority3;initRX|`S25/BSP/board_hw_config.c:321`,`:333`,`:341`|Опциональный RX IRQ ring/TX polling,priority6 |
| USART6 TX/RX |DMA2 S6/S2,CH5;USART6 IRQ priority3|`S25/BSP/board_hw_config.c:375`,`:387`,`:396`|Bounded polling |
| TIM1 |PWM CH1…4;IRQ TIM1_CC priority1|`S25/BSP/board_hw_config.c:425`,`:485`|TIM1_UP_TIM10 priority4,100µs software service |
| TIM4/TIM5 |PSC83,ARR19999,50Hz;общая частота внутри группы|`S25/BSP/board_hw_config.c:551`,`:556`,`:589`,`:594`|Сохранены группы;frequency/resource validation |
| TIM8 LED |PSC0,ARR210;CH3→DMA2 S1 CH7;DMA IRQ priority5|`S25/BSP/board_hw_config.c:500`,`:507`,`:530`,`:535`|Та же DMA route;ARR209;bounded completion polling |
| TIM14 |PSC0,ARR8400;TIM8_TRG_COM_TIM14 IRQ priority3|`S25/BSP/board_hw_config.c:625`,`:634`|Дедлайны отTIM2 вместо общего legacy timeout IRQ |
| TIM2 |Нет функцииinit в перечисленной2025конфигурации main|`S25/App/main.c:81`…`:85`|Новый1MHz monotonic timer для deadline/micros |
| NVIC grouping |`NVIC_SetPriorityGrouping(0)`|`S25/App/main.c:62`|Grouping3 согласован с FreeRTOS;не копировать старую группировку |
| Реальные IRQ wrappers |USART1/3,UART4/5,USART6,TIM1_CC,TIM14,DMA2S1|`S25/App/stm32f4xx_it.c:38`…`:79`|Перенесён результат функций,не прежний transport API |
| Watchdog |В2025 нетIWDG_Init/main enable|`S25/App/main.c:55`…`:126`|IWDG с progress Control/Comms |

Пример: изменение DMA transport→poll/ring — программная реализация при тех же пинах, не требование перепаять плату. Смена USART1 baud/protocol требует перенастройки хоста. Смена motor IDs/матрицы требует выбора соответствующего профиля. Добавление PCA/насосов/концевиков может потребовать устройств и проводки.

## Фактическое активное поведение2025

- `S25/App/main.c:120` создаёт DemoTask; `S25/OS_Tasks/demo_task.c:48` действительно определяет STEPPER_DEMO. Ветка `:93` инициализирует/включает все4 L6474, `:104` присваивает software zero. Цикл `:220` двигает к20000/8000/20000/20000 и обратно0. Это **не проверка физического home**, не допустимые механические пределы и не обязательное поведение безопасного NEXT.
- `S25/OS_Tasks/gripper_task.c:43` автоматически включает torque ID103/102; состояние0 задаёт0°, состояние1 выполняет100°→50°→0°, с паузами1000 ticks (`:52`…`:66`). Скорость/механические диапазоны там не подтверждены. NEXT сохраняет явно вызываемые2025операции и отдельное разрешение torque; не копирует auto-enable.
- `S25/OS_Tasks/game_task.c:43` выбирает DEBUG_MODE,`:53` автоматически включает Maxon.100000 ticks timeout в`:65` действует толькоGAME_MODE; это не активный99s policy2026. `PrepareRobot()` в`:46` закомментирован, хотя сама функция существует.
- `S25/App/robot_behavior_routine.c:113` включает/отключает Maxon по button state. В callback есть fall-through и проблемное self-increment strategy; NEXT сохраняет полезное назначение с явной authority и исправлением ошибок, а не их буквальное воспроизведение.
- В canonical2025 нет pump/valve mask API2026. Отдельная локальная папка `2025 г./eurobot_2025_main_board-master/Core/App/terminal_cmd_handlers.c:214` содержит PumpOn, а`:226` — PumpOff; **оба SET PO1|PO2**. Это изменённый локальный вариант, ошибка OFF и не доказательство назначенияPO1=насос,PO2=клапан или наличия четырёхпар.

## Что допускается в кандидате2025

Явно выбранный source-кандидат2025 сохраняет источник именованных фактов: MCU/clocks,2025motor-ID order,2025geometry,AX103/102,4commoninputs и source stepper parameters. Само наличие этих параметров не активирует оборудование. Для первого запуска используется общий DIAGNOSTIC_MODE без автоматического выбора2025/2026: нет arm, движения, pump/valve ON, torque enable, stepper enable/homing и gripper jobs; опросы только явно безопасного доступного интерфейса. В частности, неизвестные насосные routes, PCA,9концевиков и2026gripper IDs не включаются без подтверждения их наличия на реальном роботе.

Переход в OPERATIONAL_MODE выполняется только после проверки реального устройства, wiring, модели/ID, референсов и пределов. Неизвестные значения остаются неизвестными; нельзя ставить искусственный ток/скорость/home/порог мощности для прохождения validation. Полная аппаратная проверка каждой возможности изложена в главной инструкции поставки.

### Представление кандидата в C++

Предусмотрен собственный header `firmware/include/eurobot/robot_2025.hpp` и factory `eurobot::board::robot_2025()`, возвращающая Deployment. Идентификатор — `HardwareProfile::ROBOT_2025_HARDWARE_PROFILE`, начальный режим — `DIAGNOSTIC_MODE`. Это источник кандидата2025; ни он, ни пример2026 не выбираются по одному году прошивки. Общий BoardDiagnostic — отдельный generic/CUSTOM профиль без предположений об устройствах.

Кандидат хранит motor IDs0x21/0x22/0x23, `legacy_2025_geometry()`, известные AX103/102 и2025 gripper bindings. При этом все actuator allowlists/enables, PCA, BMS/LED polling и `grippers[].reviewed` выключены; routes насосов/клапанов остаются0, `max_active=0`, operational limits не заполнены, `home_input=255`. Значения800mA/TVAL24/mode4, CONFIG7c03/2883, ALARM3/87 иTOFF31/255 — неактивные source facts до явной проверки. OCD8 в кандидате обозначен как значение reset register по datasheet, **не как запись исходной2025 init и не как подтверждённый безопасный ток**.

Electrical policy четырёх common inputs — PullDown mask0x0F; это сохраняет `S25/App/robot_gpio_config.c:34`…`:68`. Остальные входы не превращаются в установленное оборудование от наличия raw GPIO bit. Режим/profile/commissioned/valid доступны отдельным read-only configuration snapshot (maintenance op37); порядок и имена CLI операций сверяются с актуальной [MAINTENANCE.md](MAINTENANCE.md).

Переход из DIAGNOSTIC_MODE не должен происходить от button level, появления ROS bridge или reboot. Для operational нужны явное разрешение режима, reviewed/commissioned configuration и штатные owner/lease/arm. Даже после изменения mode неизвестные limits/routes не обходятся. Итоговые образы и тесты этого кандидата фиксируются общей финальной сборкой отдельно; документация аппаратного аудита не является таким подтверждением.

### Неизвестные величины, которые source2025 не закрывает

| Уровень | Что не установлено | Зачем требуется |
|---|---|---|
|CRITICAL|Источник питания логики/3.3V VTref, текущее питание/защита, независимое отключение нагрузок|Подключение SWD/первый boot без воздействия текущей неизвестной программы |
|CRITICAL для движения|Фактические wheel-ID↔колесо, направления, реальные пределы скорости/ускорения/тока|Предотвращение неверного вращения/неограниченного воздействия |
|REQUIRED FOR FEATURE|Модели/реальныеID AX103/102, pulse/mechanical ranges, источник питанияservo|Наличиеsourceадреса не доказывает конкретный установленныйservo |
|REQUIRED FOR FEATURE|Каждый насос/клапан и егоPO/напряжение/ток/нормальное состояние|В2025 нет доказанной карты жгута; не выбирать8+8 автоматически |
|REQUIRED FOR FEATURE|Stepper Output↔механизм/парыобмоток, реальные ток/ход/home/steps-per-mm|Software zero и demo positions не физическая калибровка |
|REQUIRED FOR FEATURE|Концевики сверх4commonbuttons, PCA/BMS/DC/LED фактически установлены?|Не опрашивать/не включать вымышленные устройства |
|OPTIONAL|UART5,ADC/SPI/I2C расширения|Дальнейшее оборудование без конфликта aliases |

Часть сведений после диагностической загрузки читается программно: board_info/reset/config, raw GPIO, именованный `servo_probe` одного ID без write/arm. Но текущая неизвестная программа до загрузкиNEXT не обязана понимать новый протокол/115200. Нельзя использовать отсутствие ответа нового клиента как доказательство неисправной платы. Backup текущейFlash делается только если защита позволяет чтение, без read-unprotect/option-byte изменений; загрузку в ходе разработки не выполнять.
