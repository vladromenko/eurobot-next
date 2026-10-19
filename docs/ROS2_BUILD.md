# Сборка ROS 2 и проверка транспорта

Основная среда — Ubuntu 24.04 + ROS 2 Jazzy, GCC 13 и C++23. Актуальный релиз на 19.10.2026 — Lyrical Luth, Ubuntu 26.04 Tier 1; Jazzy остаётся поддерживаемой веткой. Дистрибутив задаётся явно. Основания: [матрица Lyrical](https://github.com/ros2/ros2_documentation/blob/rolling/source/Releases/lyrical/supported-platforms.rst), [официальное объявление](https://discourse.openrobotics.org/t/ros-2-lyrical-luth-released/55021), [официальные Docker-теги](https://raw.githubusercontent.com/docker-library/official-images/master/library/ros).

## Состояние проверки

19.10.2026 после окончательной интеграции исходников повторно выполнены полные Linux ARM64 Docker-сборки workspace, генерация msg/srv/action, линковка bridge и colcon tests:

| Среда | Build | Test result | Локальный журнал |
|---|---|---|---|
| Jazzy / Ubuntu24.04 | 3 packages finished, 13.8s | 3 tests, 0 errors, 0 failures, 0 skipped | artifacts/validation/ros2_jazzy.log |
| Lyrical / Ubuntu26.04 | 3 packages finished, 15.4s | 3 tests, 0 errors, 0 failures, 0 skipped | artifacts/validation/ros2_lyrical.log |

Это реальные container builds, выполненные родительским runtime после запуска Docker Desktop. Тесты используют Linux PTY и настоящий ROS bridge/graph, без платы. Число test results включает CTest/pytest wrapper results; это не число независимо реализованных сценариев. Таблица относится к финальному повтору с актуальным shared core, maintenance0..35, verified op5 readback, параллельными ресурсами и LED60. В этом повторе также проверены late-cancel race, future-skew MotionEpoch и terminal snapshot после disarm MCU. Экспорт обоих образов успешно завершён.

Актуальные native core/transport/PTY-тесты с maintenance0..35, actuator12..15 и verified reset readback успешно скомпилированы Apple Clang C++23 со строгими предупреждениями и выполнены с PASS, включая ASan/UBSan. Python source ROS graph test проходит syntax check. Ни UART реальной платы, ни flash не открывались.

## Полная сборка

Из корня EUROBOT_NEXT, при уже запущенном Docker daemon:

```sh
ros2_ws/tools/build_ros2.sh --docker --distro jazzy
ros2_ws/tools/build_ros2.sh --docker --distro lyrical
```

Runner проверяет доступность daemon и останавливается с объяснением, если его нет. Он не запускает приложения, не открывает UART и не выполняет прошивку. Dockerfile устанавливает зависимости из официального ROS-образа, генерирует interfaces, собирает bridge и запускает все ROS-тесты. Образы двух дистрибутивов имеют отдельные теги.

На соответствующей Ubuntu после установки ROS и colcon:

```sh
sudo apt-get update
# Для Lyrical заменить jazzy на lyrical и использовать поддерживаемую Ubuntu 26.04.
sudo apt-get install build-essential cmake ninja-build python3-colcon-common-extensions \
  python3-pytest ros-jazzy-ros-base ros-jazzy-diagnostic-msgs ros-jazzy-std-srvs \
  ros-jazzy-sensor-msgs ros-jazzy-rclcpp-action ros-jazzy-action-msgs \
  ros-jazzy-ament-cmake-pytest
ros2_ws/tools/build_ros2.sh --native --distro jazzy
source /opt/ros/jazzy/setup.bash
source ros2_ws/install/jazzy/setup.bash
ros2 launch eurobot_bringup robot.launch.xml config:=/absolute/path/bridge.yaml
```

Runner использует раздельные `ros2_ws/build/DISTRO`, `install/DISTRO`, `log/DISTRO`, включает BUILD_TESTING, после build запускает colcon test с возвратом ошибки и выводит test-result. Отсутствующие зависимости и упавшие тесты не скрываются. Для Lyrical использовать `--distro lyrical` и соответствующие setup-файлы.

Конфигурация bridge.yaml должна содержать реальный `/dev/serial/by-id/...` и согласованную скорость UART. Запуск bridge открывает канал и запрашивает status/sensors, но не включает выходы. ST-LINK для прошивки и UART для ROS — разные подключения. Аппаратный контейнер требует передачи конкретного serial device через `--device` и прав пользователя; privileged-режим не требуется. Для стенда предпочтителен native Linux. Docker Desktop на Mac используется прежде всего для сборки: прямой USB-UART не предполагается.

## Подготовленные тесты

`eurobot_hardware/test/transport_test.cpp` использует PTY и общий firmware parser. В нём проверяются конечный timeout, фрагментация, correlation owner/sequence/opcode, CRC, exclusivity, закрытие и повторное открытие; authority tokens и publisher GID; actuator validation; sensor reply/NaN/known masks. Добавлены проверки 22-байтного Gripper submit, единиц motion ops 6/8/9/12, границ timeout, отклонения NaN, отдельного reservation gate, 25-байтного job status, короткого NACK, evidence/counts и запрета считать settled измеренным захватом.

`test/test_gripper_action.py` запускает настоящий собранный bridge и ROS graph с отдельным доменом, подключённый к PTY peer. Он проверяет отказ неверного token и второго goal, совпадающие/независимые pump/DC ресурсы, параллельную Twist с PREPARE, эксклюзивную motion операцию, known-not-started submit NACK без отзыва lease, передачу submit22, отмену через wire 0x23 и подтверждённый CANCELLED, успешное завершение с evidence=settled без physically_verified, serial timeout с ABORTED/outcome_known=false и отсутствие автоматического replay. Peer является тестовой моделью и не подтверждает STM32 или физический захват. CMake регистрирует этот тест через ament_cmake_pytest; Docker и CI запускают его вместе с transport test.

Отдельный host-тест транспорта без ROS можно выполнить позднее из корня на macOS:

```sh
clang++ -std=c++23 -Wall -Wextra -Wpedantic -pthread \
  -Ifirmware/include -Iros2_ws/src/eurobot_hardware/include \
  firmware/src/core.cpp ros2_ws/src/eurobot_hardware/src/transport.cpp \
  ros2_ws/src/eurobot_hardware/test/transport_test.cpp -o /tmp/eurobot_transport_test
/tmp/eurobot_transport_test
```

На Linux добавить `-lutil`. Этот тест не заменяет colcon/ROS action test. После отдельных запусков фактические результаты фиксируются в TESTING.md с версией исходников и окружением.

## CI

`.github/workflows/ci.yml` готовит обязательные host GCC13 ASan/UBSan/CTest, clang-format19/static analysis, cross-build STM32 Arm GNU14.3 и ROS Jazzy/Lyrical. ROS job вызывает тот же runner, Docker build запускает rosidl/bridge/transport/action tests. Ошибки не пропускаются через continue-on-error. Локальная Docker проверка и удалённый запуск GitHub Actions учитываются отдельно; наличие workflow не доказывает remote CI run.

## Проверка платы и macOS

После отдельной сборки и разрешённой прошивки начать с `tools/board_test.py --help` и процедуры из FLASH_MACOS.md/FLASH_UBUNTU.md. Этот Mac-совместимый stdlib tool проверяет бинарные status/sensors и позволяет только явно выбранные упражнения выходов/Gripper; его запуск не заменяет ROS graph acceptance. Одновременно board_test и bridge UART не открывают. В этой работе команды реальной плате не отправлялись.

На стенде отдельно измерить остановку при исчезновении cmd_vel, истечении output deadman, потере UART/bridge, action cancel и timeout; проверить аппаратный estop, отсутствие auto-arm/replay после reconnect и невозможность смены владельца активной сессии. Для Gripper проверить каждый механизм профиля, реальный homing и обратную связь, stop_error, уровни evidence и отсутствие ложного физически подтверждённого захвата. Capture обозначает механический сценарий; камера проверяется отдельно в верхнем уровне 2026.

## micro-ROS

Текущий STM32 endpoint использует ограниченный бинарный UART, fixed payload ≤128, CRC, sequence/owner/lease и локальные тайм-ауты. micro-ROS потребовал бы отдельного подтверждения static pools, stack, XRCE/Agent UART и Worst Case Execution Time вместе с приводами. Такого измерения нет; XRCE runtime не добавлен. Это выбранная реализация, а не утверждение о невозможности micro-ROS на STM32F407.

## Дополнительная текущая проверка сервисного API

Maintenance.srv с ops0..35 добавлен в rosidl generation. В native transport test проверяются фиксированные22B offsets, finite values, Boolean/ID/calibration limits, разграничение read/mutation и точные response lengths. ROS graph test дополнительно проверяет read без token, отказ mutation без lease, claim-only maintenance session без armed, разрешённый CLEAR_RESET, запрет actuator outputs и неявного переключения режима, обязательный disarm перед обычной сессией и отказ сервисной mutation во время Gripper job. Этот mock не подтверждает физический reset, калибровку или EEPROM реальных устройств.

Локально актуальные core/transport/test исходники успешно скомпилированы Apple Clang C++23 со строгими предупреждениями; transport/PTY suite выполнен с PASS. Финальные maintenance helpers0..35 и дополнительные actuator kinds12..15 проверены с ASan/UBSan: PASS. Полная ROS проверка выполнена через родительский runtime; её журналы и результаты приведены выше. Native и ROS проверки дополняют друг друга и не подтверждают физические выходы платы.

## Install prefix при test

Runner передаёт одинаковые `--build-base build/DISTRO` и `--install-base install/DISTRO` в build и test. Поддержка test `--install-base` проверена по [исходникам colcon-core0.20.1](https://github.com/colcon/colcon-core/blob/0.20.1/colcon_core/verb/test.py). Финальные Jazzy/Lyrical журналы подтверждают успешный test с актуальным install prefix без прежнего предупреждения о якобы несобранных workspace packages; фильтр предупреждений не используется.

Идентификаторы успешно экспортированных локальных образов: Jazzy `sha256:27c4ff5cbb90115d399209334082ffd6763c365b1c74629afd67f7d9d2ed855e`; Lyrical `sha256:5d24032e28187c7fb4ffa1af71be335221091eba6bd8f5e76981492dc2472964`. Эти результаты подтверждают software build/ROS graph; физический UART, приводы и HIL/WCET остаются отдельными испытаниями.
