# Сборка CMake на macOS и Ubuntu

Прикладной код использует C++23. C++26 — отдельный probe используемого компилятора: отсутствие полной поддержки не блокирует основной C++23 проект. Все собственные embedded-модули имеют расширения `.cpp/.hpp`; C допустим только для CMSIS/FreeRTOS и другого необходимого vendor middleware.

## Зависимости

Нужны CMake≥3.25 с presets, Ninja, GoogleTest для host проверок, компилятор host с C++23 и `arm-none-eabi-gcc/g++`, `objcopy`, `size` для Cortex-M4F. Для host sanitizers нужен соответствующий runtime компилятора. В ходе разработки ядро проверялось LLVM21 и ARM GNU14.3; конкретная доступность `std::expected`/C++26 проверяется фактической сборкой, а не номером стандарта.

Зависимости CMSIS5.9.0, STM32F4 device2.6.10, FreeRTOS11.1.0 закрепляются по версиям и официальному происхождению. Необходимость сети относится к подготовке dependencies; тесты не должны скачивать приложение старых прошивок. Не используйте ARMCC include из Keil как стандартную библиотеку GCC.

## Повторяемые команды

Выполнять из корня EUROBOT_NEXT:

```sh
cmake --list-presets
cmake --preset host
cmake --build --preset host
ctest --preset host --output-on-failure
cmake --preset stm32-debug
cmake --build --preset stm32-debug
cmake --preset stm32-release
cmake --build --preset stm32-release
```

Путь к `arm-none-eabi-*` должен быть в PATH либо в переменной ARM_TOOLCHAIN_BIN. На macOS toolchain также ищет комплект ARM GNU внутри установленного STM32CubeIDE; Keil при этом не нужен. Зависимости готовит tools/dependencies.sh. Заголовок проверенного профиля передаётся -DEUROBOT_BOARD_CONFIG=/absolute/path/deployment.hpp; он предоставляет make_deployment(), как examples/deployment.hpp. Базовый пример не вооружается. Для предписанной новой разводки8+8 на PO1…PO16 есть examples/board_8_pumps.hpp; он также оставляет commissioning и неизвестные limits выключенными. Дополнительный probe:

```sh
cmake --preset cxx26-probe
cmake --build --preset cxx26-probe
cmake --preset stm32-cxx26-probe
cmake --build --preset stm32-cxx26-probe
```

Команды `tools/build.sh`, `tools/test.sh`, `tools/clean.sh` дают короткий интерфейс; их фактические аргументы смотрите через help/исходник. Clean должен касаться только build-каталогов нового проекта, а не оригиналов или исходников.

## Целевые параметры

Цель STM32F407VG использует `-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, отключённые exceptions/RTTI, startup с реальной vector table и linker script Flash1 МиБ/SRAM128 КиБ/CCM64 КиБ. Все objects и библиотеки должны иметь единый ABI. DMA buffers — только обычная SRAM; CCM отдельно. Startup обязан инициализировать `.data/.bss`, C++ constructors и FreeRTOS vectors SVC/PendSV/SysTick.

Успешная целевая сборка создаёт ELF и преобразует его в HEX/BIN; команда size/map проверяет пределы секций и stack. ELF machine=ARM и hard-float attributes проверяются отдельно. Probe C++26 не равнозначен проверке всех возможностей языка или библиотеки.

## Конфигурация робота

Сборка без подтверждённых лимитов и полной hardware binding должна оставаться безопасной и не вооружаться. Нужно отдельно настроить дополнительные насосы, polarity клапанов, реальные пределы шасси/encoder delta, привязку servo ID, homing и аппаратный ESTOP. Значения в unit tests не являются конфигурацией реального робота.

ROS2 workspace собирается `colcon` отдельно, см. ROS2_BUILD.md. Host CTest не подменяет colcon. Наличие CMake preset в JSON не считается успешной сборкой; выполненные цели и причины пропусков записаны в TESTING.md.

## Результаты текущего среза

Свежие Debug/Release и экспериментальные C++26 цели проверяются общим прогоном. Числа Flash/RAM, SHA256 и тестовые результаты приведены в [VALIDATION_REPORT.md](VALIDATION_REPORT.md), журналы находятся в artifacts/validation. Старые размеры77912/52184байта относятся к неполной реализации и больше не описывают текущие образы.

Для полного воспроизводимого прогона без платы:

```sh
python3 tools/validate.py --legacy-root .. --experimental-cxx26 --ros2-docker
python3 tools/write_validation_report.py
```

Нужны clang-format19, clang-tidy и запущенный Docker; при нестандартных путях задайте CLANG_FORMAT/CLANG_TIDY. Скрипт выполняет чистые сборки, host/sanitizers, ARM и ROS, но не обращается к программатору или UART робота.

Передача прошивки не запускается сборкой. `tools/verify.sh` без дополнительных разрешающих флагов проверяет локальный образ и manifest без доступа к программатору. `--allow-connect` разрешает SWD и readback; запись через отдельный flash требует `--allow-flash` и проверки условий из FLASH_MACOS/FLASH_UBUNTU. Desktop path проекта после перемещения требует повторной конфигурации CMake: build cache с абсолютными staging-путями не является переносимым результатом.
