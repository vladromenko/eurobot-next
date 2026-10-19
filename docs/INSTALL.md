# Установка и подготовка EUROBOT NEXT

**Первое подключение: [WHAT_I_NEED_TO_DO.md](../WHAT_I_NEED_TO_DO.md).** По умолчанию комплект использует общий DIAGNOSTIC_MODE этой платы без назначения силовых нагрузок. Схемы2025/2026 ниже — варианты параметров/проводки; год ранее установленной прошивки не выбирает их автоматически. Новые восемь парPO1…PO16 требуют соответствующего реального жгута.


Для этой платы подготовлен заполненный source-профиль2026 и единый установщик: [ROBOT_2026_PROFILE.md](ROBOT_2026_PROFILE.md). Команда `python3 tools/install_robot.py flash` собирает его и выполняет backup/write/readback после подключения ST-LINK. Шаблон deployment вручную заполнять не требуется. Нижеприведённые команды общего `flash.sh` относятся к явно выбранному образу; без `--image` его default — диагностический Debug, а не рабочий source-профиль.

**Текущая программная редакция собрана и проверена.** Host, ASan/UBSan и host C++26: по81/81 CTest; STM32 debug/release/C++26 ELF/HEX/BIN и ROS2 Jazzy/Lyrical colcon build/test — PASS. Подробности и журналы: TESTING/VALIDATION_REPORT. Это не аппаратная приёмка и не разрешение вооружать default deployment. После изменения исходников, deployment, компилятора или пути проекта нужен свежий configure/build и согласованный manifest. Результаты34/34 и ранее сохранённые образы относятся к историческому срезу, а не автоматически к новой прошивке. Физическая загрузка владельцем — отдельный последующий шаг.

Финальная прошивка 2026 определяет захваты и команды. Версия 2025 служит базой/историей; она не заменяет поведение, присутствующее в 2026. См. [сравнение](COMPARISON_2025_2026_NEXT.md), [сценарии](GRIPPER_SCENARIOS.md).

## 1. Где находится проект

Рабочий проект: `/Users/vladromenko/Desktop/Eurobot/EUROBOT_NEXT`. Исходные `01_FIRMWARE_2025`, `02_FIRMWARE_2026`, `ORIGINAL_ARCHIVE` и папки исследования — только чтение. Не размещайте новые сборки внутри старого SOURCE/Core и не исправляйте оригиналы. Новый каталог можно перенести на Ubuntu; старые абсолютные CMake caches при этом не переносятся как результат конфигурации.

```sh
cd /Users/vladromenko/Desktop/Eurobot/EUROBOT_NEXT
```

Для Ubuntu далее используйте реальный путь своей копии проекта. Не запускайте CMake/colcon через sudo. Отдельно учитываются native host tests, STM32 cross-build, ROS2 colcon и HIL.

## 2. macOS Apple Silicon

Нужны Command Line Tools/Xcode с SDK, Git, Python3, CMake≥3.25, Ninja и GoogleTest. Homebrew устанавливается по [официальной инструкции](https://brew.sh/), если его ещё нет. Команды установки ниже изменяют только среду разработки, не контроллер:

```sh
xcode-select --install
brew install cmake ninja googletest python llvm@19
```

Если CLT уже установлены, повторная установка не нужна. [GoogleTest formula](https://formulae.brew.sh/formula/googletest) и [LLVM19 formula](https://formulae.brew.sh/formula/llvm@19) поддерживают установку Homebrew; LLVM19 нужен для согласованного clang-format/static-analysis. Для host C++23 можно использовать Apple Clang из установленного Xcode/CLT, как в историческом протоколе. Номер стандарта не заменяет проверку библиотеки; CMake проверяет используемые возможности. C++26 — отдельный эксперимент, не требование обычной установки.

Проверить архитектуру host и расположение инструментов, не компилируя проект:

```sh
uname -m
xcode-select -p
cmake --version
ninja --version
python3 --version
/usr/bin/clang++ --version
brew --prefix googletest
```

На Apple Silicon ожидается arm64. Не смешивайте x86_64 GoogleTest через Rosetta с native arm64 compiler. Путь `/opt/homebrew` типичен, но команды должны брать его из `brew --prefix`, не предполагать жёстко.

### ARM GNU Toolchain

Установите официальный **arm-none-eabi** комплект для bare-metal Cortex-M; это не aarch64 Linux compiler. Воспроизводимый исторический cross-build использовал Arm GNU14.3.Rel1. Выберите архив/installer под свою host-архитектуру на [официальной странице Arm](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads), проверьте опубликованный SHA256 до распаковки. [Arm installation guide](https://learn.arm.com/install-guides/gcc/arm-gnu/) описывает host packages и PATH. Более новый компилятор требует отдельной проверки; его установка не повышает статус проекта.

После установки укажите **фактический** bin-каталог, например в текущем терминале:

```sh
export ARM_TOOLCHAIN_BIN="/absolute/path/to/arm-gnu-toolchain/bin"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
arm-none-eabi-g++ --version
arm-none-eabi-objcopy --version
```

Не копируйте примерный путь буквально. `firmware/cmake/arm-none-eabi.cmake` читает ARM_TOOLCHAIN_BIN; на macOS также умеет искать установленный комплект STM32CubeIDE. Keil не нужен. CubeIDE может содержать x86_64 tooling; совместимость с Apple Silicon проверяется отдельно. CubeProgrammer нужен для будущей ручной загрузки, но не для host/cross-build.

## 3. Ubuntu 24.04

Основная описанная ROS-среда — Ubuntu24.04 Noble + ROS2 Jazzy, amd64 или arm64. Это выбранная поддерживаемая версия проекта, а не утверждение, что Jazzy — новейший релиз. Другие ОС/ROS требуют отдельной проверки. Native host toolchain и cross-toolchain — разные инструменты.

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  build-essential g++-13 cmake ninja-build git python3 curl xz-utils \
  libgtest-dev clang-19 clang-tidy-19 clang-format-19
```

Arm GNU14.3.Rel1 установите из официального архива под host x86_64 либо aarch64, target arm-none-eabi. Проверьте SHA256 из того же релиза. Системный пакет gcc-arm-none-eabi не следует считать тем же 14.3.Rel1: проверьте фактическую версию. Задайте ARM_TOOLCHAIN_BIN как выше. Все .o/.a для STM32 должны использовать Cortex-M4F hard-float ABI; библиотеки host для ARM не подходят.

### ROS 2

Добавление подписанного ROS apt repository, locale/Universe и установка выполняются по [официальной инструкции ROS2 Jazzy Ubuntu debs](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html). Не используйте случайные сторонние install scripts. После подключения репозитория нужны:

```sh
sudo apt-get install ros-jazzy-ros-base ros-dev-tools
source /opt/ros/jazzy/setup.bash
```

`ros-dev-tools` включает инструменты разработки, rosdep/colcon. Инициализацию rosdep выполняйте только если она ещё не сделана; обновление metadata — от обычного пользователя. Из ROS workspace позднее:

```sh
cd ros2_ws
rosdep update
rosdep install --from-paths src --ignore-src --rosdistro jazzy -y
```

macOS native firmware build не требует ROS. Docker на Mac — средство последующей Linux сборки; наличие Docker CLI не означает работающий daemon. USB в Docker Desktop и Linux `/dev/serial/by-id` не взаимозаменяемы. Фактически выполненный ROS build и возможные ограничения среды фиксируются в VALIDATION_REPORT; инструкция: [ROS2_BUILD.md](ROS2_BUILD.md).

## 4. Сторонние зависимости проекта

После завершения исходников подготовьте закреплённые upstream dependencies:

```sh
tools/dependencies.sh
```

Скрипт клонирует только CMSIS/FreeRTOS, проверяет commit и отсутствие модификаций tracked files. Он не собирает приложение и не открывает hardware.

| Зависимость | Tag | Проверяемый commit |
|---|---|---|
| FreeRTOS-Kernel |V11.1.0|dbf70559b27d39c1fdb68dfb9a32140b6a6777a0|
| CMSIS_5 |5.9.0|2b7495b8535bdcb306dac29b9ded4cfb679d7e5c|
| cmsis_device_f4 |v2.6.10|5f41fb29d22773896c780052bf61e47fc924d524|

Сохраняются notices зависимостей. Права на recovered application/производный новый материал требуют решения владельца: [LICENSE-STATUS.md](../LICENSE-STATUS.md), [PUBLICATION.md](PUBLICATION.md). Установка инструментов не разрешает публикацию.

## 5. Профиль робота до сборки

Начните с `examples/deployment.hpp`, создайте собственный header вне оригиналов, например `config/deployment_local.hpp`. Header предоставляет `make_deployment()` и возвращает `eurobot::board::Deployment`. Пример намеренно не commissioned; простое изменение флага не завершает настройку. Точные поля сверяются с `firmware/include/eurobot/profile.hpp` и gripper profile из исходников новой редакции.

| Группа | Что заполнить и проверить |
|---|---|
| Разрешение |commissioned только после согласования всех используемых привязок/предела мощности; параметры сценариев 2026 разрешаются отдельно|
| Насосы/клапаны |channels8…32,max_active,pump_outputs,valve_outputs,max_pump_on_ms;0=неизвестный pin; никакой вымышленной разводки 5…8|
| Геометрия |chassis.geometry=legacy2026 baseline плюс проверенные размеры/sign/calibration; motor_ids по реальным платам|
| Шасси limits |max_linear_mps,max_angular_radps,max_accel_mps2,max_angular_accel_radps2,max_wheel_tpm,max_encoder_delta,command_timeout_ms,max_tick_ms|
| AX/MX |ax_ids,mx_ids allowlists,servo_min_deg/servo_max_deg; проверенный model/id; ID254 не индивидуальный адрес|
| PCA |pca_enabled,pca_channels,pca_calibration: frequency,min/max pulse,min/max angle; каналы PCA имеют индексы 0…15; legacy CH1=1, поэтому не вычитать 1 из значений профиля|
| STM32 PWM |pwm_channels,pwm_calibration; ресурсная группа TIM4 или TIM5 не смешивает servo50Hz/DCfrequency|
| DC |dc_channels,dc_modes явное legacy_high_pair либо coast_pwm после схемы,dc_frequency_hz для timerbank; unconfigured запрещает drive|
| Stepper |stepper_enabled,steppers min/max steps,max steps/s,max tick; current_tval,ocd_threshold,mode,driver_config по двигателю/схеме|
| Homing |home_input,home_active_high,home_forward плюс maxtravel/deadline/скорость и политика отъезда сценария;255=не назначено|
| ESTOP |estop_input/polarity,hardware независимое снятие энергии; input index0…12;255=нет назначенной линии|
| Дополнительные функции |battery_enabled/battery_id,leds_enabled; неподключённый BMS не объявляется свежим измерением|
| Захваты 2026 |main/side/cursor/pinion IDs,AXspeeds/angles,PCAblock/shift routes,pump/valvelogicalroutes,mm→steps/mm→deg,feedbacktolerance/deadlines; общий ресурс и отключение/удержание груза|
| Составной сценарий |Явно выбранный порядок фаз,механические позиции и timedmove speed/duration; неизвестные величины блокируют сценарий|

### Baseline из финального 2026 и неизвестные величины

Подтверждено **кодом**: STM32F407VG; HSE16MHz; targetSYSCLK168MHz; SPI24bits/20poweroutputs; USART1 новойпрошивки 115200; Maxon IDs0x22,0x21,0x23; L0.1217m,r0.0295m,gear28,ticks4096. Только 4 различных pumps1/3/5/7 и valves2/4/6/8, оба legacy gripper разделяютих. Cursor98,pinion13; primarymain112, reservemain1/2. Старый слот254 неактивен; все main0-рецепты реализованы с feedback112. Подробные angles/channels: [GRIPPER_SCENARIOS.md](GRIPPER_SCENARIOS.md).

Неизвестны реальные motor current/PSUbudget/valvepolarity, исполнение новой разводки PO1…PO16 и физическая связь USART3/DIO, соответствие установленных servoIDs исходной карте, физические limits/ход/знаки, homingpolarity/travel, payload удержание при disable, worst-case latency/WCET. Ни одно поле нельзя заполнять случайным тестовым числом. При текущем 20outputbackend32logicalchannels не означают 32 физических насоса с 32 клапанами; понадобится подтверждённое расширение электроники.

## 6. Последующая ручная сборка и тесты

**Ниже команды для самостоятельного воспроизведения. Фактический текущий результат проверяется по VALIDATION_REPORT.** Для воспроизводимой ручной проверки текущего проекта из корня проекта:

```sh
# macOS host, native Apple Clang; GoogleTest из соответствующего Homebrew.
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake --preset host \
  -DCMAKE_PREFIX_PATH="$(brew --prefix googletest)"
cmake --build --preset host
ctest --preset host --output-on-failure

# Ubuntu host вместо macOS configure:
CC=gcc-13 CXX=g++-13 cmake --preset host
cmake --build --preset host
ctest --preset host --output-on-failure
```

Sanitizers и cross-build — отдельные последующие проверки:

```sh
cmake --preset host-sanitize
cmake --build --preset host-sanitize
ctest --preset host-sanitize --output-on-failure
cmake --preset stm32-debug -DEUROBOT_BOARD_CONFIG=/absolute/path/deployment_local.hpp
cmake --build --preset stm32-debug
cmake --preset stm32-release -DEUROBOT_BOARD_CONFIG=/absolute/path/deployment_local.hpp
cmake --build --preset stm32-release
```

Не запускайте оба host configure подряд на одной платформе с разными компиляторами: выбирается свой блок; для смены compiler нужен отдельный чистый buildtree. C++26 — дополнительно `cxx26-probe`/`stm32-cxx26-probe`, не substitute C++23 checks. Артефакты: `build/<preset>/firmware/eurobot_next.{elf,hex,bin,map}` и manifest. Размер,hash,vector/ABIcheck должны относиться к **новым** исходникам/профилю. Старые файлы не обновляются от редактирования C++. Новый manifest фиксирует source fingerprint и выбранный deployment; изменение или отсутствие исходного deployment header блокирует запись. Перенос на другую машину требует нового configure/build с доступным profile path, а не ручного исправления manifest.

ROS позднее на Ubuntu:

```sh
source /opt/ros/jazzy/setup.bash
cd ros2_ws
colcon build --cmake-args -DBUILD_TESTING=ON
source install/setup.bash
colcon test --return-code-on-test-failure
colcon test-result --verbose
```

Запуск bridge открывает serial и запрашивает status; это уже доступ к устройству, не часть установки. Параметры serial/baud115200 проверяются перед отдельным разрешённым запуском. Никакого auto-arm/replay после reconnect.

Новый профиль дополнительно содержит `grippers[2]` типа `gripper::Config`. Factory `legacy_2026_reference(index)` заполняет только подтверждённые source facts и оставляет `reviewed=false` и pump/valve mapping не назначенным. Primary main выбирает112 маской2; бывший слот254 неактивен. Настраиваются main/side/cursor/pinion bindings, main_active_mask и limits, block/shift PCA indices, logical PumpBank indexes, raw speeds, angle tolerance, lift stepper/steps-per-mm/speed/current/bounds, phase/operation timeouts, `disable_lift_after_move`, `drive_reference` и composite selection. Для `Composite::base_strategy_2026` обязательны `capture_axis`, явные forward/backward speeds и durations: устаревший HL payload их не даёт. `reviewed=true` относится к выбранному механическому профилю; `commissioned=true` — к общей плате.

## 7. Загрузка — отдельный этап владельца

В этой работе оборудование не использовалось. Для будущей записи выберите свежий образ и соответствующий manifest. До запуска проверьте питание и соединения, адрес приложения 0x08000000 и отсутствие собственного загрузчика в этой области. Скрипт сделает полную резервную копию внутренней Flash перед записью. Снятие RDP, изменение option bytes и полное стирание для обхода ошибки чтения не выполняются.

Подключения SWD и UART, команды записи и ручной запуск после `halt` описаны в [CONNECT_AND_FLASH.md](CONNECT_AND_FLASH.md). Дополнительные сведения: [FLASH_MACOS.md](FLASH_MACOS.md), [FLASH_UBUNTU.md](FLASH_UBUNTU.md). Без разрешающих флагов `tools/verify.sh` проверяет только локальный образ. `--allow-connect` разрешает чтение по SWD; `--allow-flash` используется отдельной командой записи. В CI доступ к оборудованию не нужен.

После самостоятельной загрузки начните с чтения состояния через `tools/board_test.py`, как описано в [MACOS_BOARD_TEST.md](MACOS_BOARD_TEST.md). Эта программа не запускается установкой автоматически. Для включения приводов нужен полностью настроенный профиль и отдельное упражнение с явно выбранными параметрами.

## 8. Частые проблемы

- GTest не найден: проверить архитектуру Homebrew/libgtest и CMAKE_PREFIX_PATH, не скачивать случайный binary.
- Armcompiler не найден: ARM_TOOLCHAIN_BIN должен указывать bin с arm-none-eabi-g++,не hostclang и не aarch64-linux-gnu.
- CMake cache содержит /private/tmp илидругуюмашину: создатьновуюконфигурациюв buildtree проекта;не переносить чужой cache.
- dependencies.sh сообщает revision mismatch: не менятьожидаемый hash ради успешного выхода; проверить origin/tag/локальныеправки.
- Defaultprofile не arm: ожидаемо при commissioned=false,нулевых limits илинезаполненных routes базового профиля. Исправление—подтверждённая конфигурация,неудаление validation.
- ROScolcon/Docker не установлен/daemon недоступен: отметить BLOCKED или NOTEXECUTED;неподменятьего hostPTYtest.

## Приложение, сервисные функции и второй UART

Помимо gripper jobs реализовано локальное приложение кнопок/матча2026,99s timer, init/state/click/hold events, strategy cycle, reset latch и конечные причины stop. Таблица известных операций обслуживания: [MAINTENANCE.md](MAINTENANCE.md). Mac клиент поддерживает `--maintenance board_info/application_state/wheel_ticks/wheel_path/...`; чтение не создаёт сессию и не arm. Изменения disarmed выполняются через новый owner/lease с `--allow-maintenance`, без ARM. `communication --arg1` позволяет восстановить приём команд после окончания матча, но не включает устройства.

`Deployment.application` задаёт временные параметры входов, GAME/DEBUG mode, duration и явную2025 motor-button compatibility. Период Control10ms ограничивает разрешение временного debounce; это не заявление об измеренных восьми1ms samples. `Deployment.debug_uart_enabled=false` по умолчанию. Если схема подтверждена, UART5 PC12TX/PD2RX115200 даёт тот же versioned protocol и общий owner с USART1; старый raw debug protocol намеренно заменён. Внешние клиенты не получают независимое владение механизмами через debug.

Дополнительно появились generic PWM allowlists/frequency по банкам, `dc_brake_configured/dc_brake_high`, source-style stepper ramp/profile и bounded homing profile. Числа из legacy не заменяют реальные пределы. Пример [legacy_2026_reference.hpp](../examples/legacy_2026_reference.hpp) содержит подтверждённые source facts и именованные TODO_UNKNOWN; commissioned/reviewed остаются false. Он не заменяет существующий `examples/deployment.hpp` и не вооружается. В нём явно показаны четыре общих pump routes, geometry2026, motor IDs и GAME99000, без вымышленных значений для неизвестной механики.

## Присланная карта платы и новая разводка8+8

[BOARD_WIRING.md](BOARD_WIRING.md) показывает подключения ST-LINK, USB–UART, питания, всех13 входов, PWM, шаговых приводов и нагрузок. Для новой разводки насосы/клапаны1…8 назначены парами PO1/2,…,PO15/16, PO17…20 свободны. Используйте [board_8_pumps.hpp](../examples/board_8_pumps.hpp) как основу своего deployment: в нём уже заданы физические routes и две независимые группы захватов. Электрические/механические limits и IDs по-прежнему заполняются только из оборудования. USART2 на рисунке не является используемым USART3 Dynamixel; его внешний DIO нужно подтвердить по цепи.

## DIAGNOSTIC_MODE и явный рабочий профиль

`Deployment.mode` по умолчанию DIAGNOSTIC_MODE. Для проверенного пользовательского профиля кроме остальных обязательных полей явно выбирается `config.mode = eurobot::board::DeploymentMode::OPERATIONAL_MODE;`. Значения `commissioned=true` и gripper `reviewed=true` сами по себе режим не меняют. В generic-образе эти флаги ложны, адреса/силовые routes не объявляются обнаруженной комплектацией. Переключения режима командойUART/ROS нет.

Профили2025/2026 на одной и той же плате могут отличаться назначением устройств; прежняя установленная программа не определяет выбранный профиль. Все новые образы по-прежнему не arm при boot. Проверенный профиль устанавливается только явным `install_robot.py --profile custom --config ...`; старый local2026 не подхватывается общей диагностикой автоматически. Подробная последовательность: [WHAT_I_NEED_TO_DO.md](../WHAT_I_NEED_TO_DO.md).
