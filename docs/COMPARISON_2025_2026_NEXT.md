# Сравнение 2025, финальной 2026 и EUROBOT NEXT

**Основой поведения служит финальная прошивка 2026.** Версия 2025 — предыстория и базовая платформа. Если функция отсутствует в 2025, но есть в 2026, переносится вариант 2026. Захваты и актуальные механические операции берутся из 2026. Историческая совместимость с 2025 остаётся явной и не меняет основной профиль.

Текущая редакция включает приложение матча, MCU сценарии захватов, конечные движения, полный полезный набор драйверов и типизированное обслуживание. Программная проверка включает host/ASan/UBSan/C++26, ARM Debug/Release и ROS Jazzy/Lyrical. Точные результаты и границы среза — в [VALIDATION_REPORT.md](VALIDATION_REPORT.md), [TESTING.md](TESTING.md). Сборка и проверки с подставным оборудованием и PTY не подтверждают работу механизмов: аппаратные испытания и прошивка устройства не выполнялись.

## Источники

Сравнивались канонические `01_FIRMWARE_2025/SOURCE/Core` и `02_FIRMWARE_2026/SOURCE/Core`. Их C-файлы побайтово совпадают с соответствующими PROJECT-копиями: всего 84 файла. Локальная папка 2025 содержит дополнительно PumpOn/PumpOff; это не функция канонической 2025. Реестр 603 определений и SHA256 приведён в [FUNCTIONAL_COVERAGE.md](FUNCTIONAL_COVERAGE.md), [legacy_source_manifest.csv](legacy_source_manifest.csv). Неисправная ранняя распаковка 2026 с нулевыми файлами не считается самостоятельной версией.

Пути ниже отсчитываются от исходной папки Eurobot рядом с EUROBOT_NEXT. Старое приложение C не включается в новый прикладной source tree. CMSIS/FreeRTOS остаются отдельными официальными зависимостями. Права на recovered application и производный материал требуют отдельного решения владельца: [LICENSE-STATUS.md](../LICENSE-STATUS.md).

## Аппаратная основа и движение

| Область | 2025 | Финальная 2026 | EUROBOT NEXT |
|---|---|---|---|
| MCU / инструменты | STM32F407VG, Keil/ARMCC | Та же платформа F407VG | CMake/Ninja, Arm GNU, C++23; C++26 отдельно |
| Память | Flash 1 MiB, SRAM 128 KiB, CCM 64 KiB | Те же параметры project device | Раздельные секции, резерв основного стека; Flash/RAM каждого фактического образа приведены в validation report; исходный код и ELF fingerprint связаны |
| Clock | HSE 16 MHz, SYSCLK 168 MHz | То же; PLLQ4 неверен для 48 MHz domain | Q7 для 48 MHz; реальные сигналы требуют стенда |
| UART1 к компьютеру | 921600 baud | 921600 baud | 115200 baud, versioned frame, CRC, owner, sequence |
| Maxon ID в порядке колёс | 0x21,0x22,0x23 | 0x22,0x21,0x23 | Основной порядок 2026; проверка ID и свежести feedback |
| L / r / gear / ticks | 0.1075 m / 0.03 m / 21 / 4096 | 0.1217 m / 0.0295 m / 28 / 4096 | Основной baseline 2026; 2025 доступен явно |
| Матрица кинематики | Собственные порядок и знаки | Отличается от 2025 | Нельзя получить 2025 только заменой радиуса в матрице 2026 |
| Одометрия | Приращение поворачивается конечным углом; диапазон ±2π | Midpoint φ+dφ/2; диапазон ±π | Подход 2026, known/freshness и проверка скачков encoder |
| Поправки 2026 | Нет этого набора | Общий 0.988, X 0.988, Y 1, angular 0.988, колёса [1.003,0.984,0.997] | Параметры сохраняются явно; не выдаются за новую калибровку |
| Конечное движение | Нет развитого набора 2026 | Timed X/Y/yaw, ramp, relative Y→X, poll status, wheel calibration | Отдельные конечные операции с deadline и cancel; проверены portable plans, dispatcher и runtime fake backend; физический путь не измерен |
| PID колёс | Внешние Maxon-платы | Внешние Maxon-платы | Переносимый PID не объявляется проверенным регулятором внешних плат |

MCU указан в `.uvprojx:17` обоих проектов; clocks — `Core/App/system_stm32f4xx.c:52`, `Core/BSP/board_hw_config.c:86`. Геометрия: `01_FIRMWARE_2025/SOURCE/Core/App/kinematics.c:31`, `02_FIRMWARE_2026/SOURCE/Core/App/kinematics.c:30`. Матрица 2026 — тот же файл `:49`, интегрирование — `Core/App/odometry.c:113`, Maxon IDs — `Core/App/bus_device.c:27`. Подробная математика: [CHASSIS.md](CHASSIS.md).

## Захваты и исполнительные устройства

| Функция | 2025 | Финальная 2026 | Новая реализация и граница подтверждения |
|---|---|---|---|
| Общая схема захвата | Top ID103 / bottom ID102; 0° либо цикл 100°→50°→0° | Два логических захвата, пары main/side, отдельные cursor и pinion | Основной набор 2026; цикл 2025 — явная историческая операция |
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

**Открытые аппаратные вопросы:** main AX ID254 — broadcast, индивидуальный адрес неизвестен; насосы общие, независимые каналы 5–8 не подтверждены; неизвестны токи, бюджет питания, полярность клапанов, ход, реальные пределы и удержание груза при выключении stepper. Наличие сценария не отменяет эти блокировки профиля.

## Транспорт, безопасность и RTOS

| Область | 2025 / 2026 | EUROBOT NEXT |
|---|---|---|
| ACK / completion | Часто accepted либо всегда 1; результат notification timeout игнорируется | Job ID, отдельные accepted/running/result, причина ошибки и cancel |
| Параметры | Ряд handlers читает payload без длины и диапазона | Точная длина, finite числа, masks, IDs, allowlists профиля |
| Владение | Общие flags / gripper_cmd допускают перезапись | Один владелец control, очередь, busy и конфликт ресурсов |
| Boot | Включение torque/Maxon, движения и автоматический homing | Safe init, отдельные claim/arm, проверенный профиль |
| Потеря связи | Нет единой гарантированной политики | Lease, command deadline, локальный stop/disarm; reconnect без replay |
| RTOS | Много задач, tick 1 kHz, часть ожиданий без deadline | Статические Control/Comms/Watchdog, ограниченные транзакции; WCET не измерен |
| Watchdog | Initializer есть, вызов отключён | IWDG зависит от progress критических задач; fault-сценарии требуют стенда |
| ROS | Строковый bridge/BT; несовпадения opcodes и payload | Типизированные interfaces/actions; сборка/тесты Jazzy и Lyrical; аппаратный serial путь требует стенда |
| Аварийная остановка | Старый StopAll неполон | StopSink и локальные выходы; CPU fault не гарантирует немедленный UART stop внешних плат |

## Как выбран high-level источник

В финальном дереве 2026 `bt_package/launch/bt_launch.py:12` задаёт `base_strategy.xml` по умолчанию; аргумент tree_name может выбрать другой файл. Это доказательство default source selection. Имя test/demo/orange само по себе не доказывает, что сценарий был последним на роботе.

Порядок default load/unload записан в GRIPPER_SCENARIOS. Камера и навигация остаются high-level. Найдены расхождения: PickNuts/PlaceNuts отправляют opcodes26/27, отсутствующие в финальной STM32 table; SimpleMoveOn/Off отправляют один byte, а final handler ожидает четыре. Отсутствующие speed/time нельзя восстановить предположением. Сохраняется назначение операций 2026, а ошибочные payload и ложный ACK исправляются.

## Как читать полноту переноса

Реестр 603 определений включает FIFO/IRQ/task glue, пустые routines и условные ветви. Это не 603 независимых требования робота. Каждая строка отдельно описывает перенесённое поведение, заменённый внутренний API, пустую функцию, отсутствующее поведение либо частичную реализацию.

Обновлённый [реестр603 определений](FUNCTIONAL_COVERAGE.md) различает полезное поведение, заменённые внутренние API, пустые тела, demo и неиспользуемые external-memory варианты. Для каждой строки записаны реальные source calls, замена, отклонения, названия тестов и отдельный HIL blocker. Прежние49 MISSING_BEHAVIOR перепроверены по телам и текущим реализациям, а не автоматически повышены из-за успешной сборки.

Уникальных невосстановленных полезных драйверных функций2025 не найдено. После нормализации переводов строк MX/H-bridge/RC/BMS/PWM/L6474 одинаковы с2026; AX2026 добавляет getter32, stepper отличается электрической политикой (2025:1/16/800mA,2026:full-step/1100 или1500mA). Приоритет2026 сохраняется. Подготовка2025 включала только torque двух AX и теперь требует явной команды; отдельный опасный boot wrapper не нужен.

Различия временной модели указаны явно: runtime odometry20Hz вместо legacy100Hz; time-based debounce8ms с дискретностью Control10ms; pulse service100µs ограничивает реальную частоту шагов. Активные2026 lift/home working divisions10/16 покрываются;19.2kHz workdiv1 из benchmark не объявляется достижимым на новом100µs сервисе. Автоматический2025 demo с фиктивным zero и бесконечными движениями четырёх моторов заменён явными finite упражнениями, а не объявлен перенесённым автономным матчем.

Typed maintenance0x24 охватывает reset/cause, encoder ticks/path/reset, pose, AX/MX speed/LED/torque limit/mode/ID, Maxon range/ID, BMS ID/calibration/независимые чтения, L6474 registers/reset, PWM frequency, PCA controls, GPIO и приложение матча. Disarmed/owned/armed access не позволяет обходить safety gate. Полная таблица — [MAINTENANCE.md](MAINTENANCE.md).

Установка: [INSTALL.md](INSTALL.md). История проверок: [TESTING.md](TESTING.md), [VALIDATION_REPORT.md](VALIDATION_REPORT.md). Образы должны соответствовать source/deployment fingerprint; автоматическая запись на устройство не выполнялась.
