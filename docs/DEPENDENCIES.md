# Зависимости и воспроизводимость

Собственное приложение — C++23; CMSIS и ядроRTOS используют исходный vendorC/ASM. ПрикладныеC-модули старыхEurobot в сборку не включены. Keil/ARMCC не нужен. Получение: `bash tools/dependencies.sh`; скрипт проверяет exactcommit и отсутствие изменённых tracked-файлов.

| Библиотека | Версия / exactcommit | Источник | Лицензия / подключение |
|---|---|---|---|
| FreeRTOS-Kernel | V11.1.0 / dbf70559b27d39c1fdb68dfb9a32140b6a6777a0 | https://github.com/FreeRTOS/FreeRTOS-Kernel | MIT; tasks.c/queue.c/list.c +GCC ARM_CM4F port; только статическое выделение |
| CMSIS_5 |5.9.0 /2b7495b8535bdcb306dac29b9ded4cfb679d7e5c|https://github.com/ARM-software/CMSIS_5|Apache2.0; CMSIS/Core/Include |
| cmsis_device_f4 |v2.6.10 /5f41fb29d22773896c780052bf61e47fc924d524|https://github.com/STMicroelectronics/cmsis_device_f4|Apache2.0 в License.md; deviceheaders/startup_stm32f407xx.s |

Копии лицензий закреплённых версий сохранены в vendor_licenses/. При получении зависимостей их notices остаются и в upstream-деревьях. ZIP содержит собственные исходники и воспроизводимый скрипт получения; полные сторонние репозитории и их кэши не вложены. Первая сборка из ZIP требуетGit/сеть; запись готовогоBIN этого не требует.

Инструменты: Python≥3.10, CMake≥3.25, Ninja, ARM GNU C/C++/ASM Cortex-M4F hard-float. Проверки разработчика: clang-format19, clang-tidy; hostC++23+GoogleTest; C++26 отдельный эксперимент. ROS2Jazzy/Lyrical строится в LinuxDocker, зависимости описаны Dockerfile.ros2; контейнеры не требуются для записи сMac. Соответствие проверяется чистой сборкой, ELFABI/vector/linkedaudit, hosttests/sanitizers и colcon build/test. Реальная аппаратная совместимость требует стенда.

Лицензия собственной перенесённой логики/право публичного распространения исходного поведения отдельно не установлены: LICENSE-STATUS.md и PUBLICATION.md. Комплект подготовлен владельцу локально, публикации не было.
