# Проверки текущего комплекта

Проверка завершена 2026-10-19T11:37:42.955528+03:00. Source fingerprint: `95ffe46275663a478cb0afac5fe7a1992810f9e79679b8e5c30da635719591c9`.

Результаты относятся к одним и тем же неизменившимся исходникам. Программная проверка не является аппаратным испытанием. Основной образ — общий DIAGNOSTIC_MODE, независимый от года прежней прошивки.

| Проверка | Результат | Журнал |
|---|---|---|
|host|87/87 CTest PASS|[log](../artifacts/validation/test_host.log)|
|host-sanitize|87/87 CTest PASS|[log](../artifacts/validation/test_host-sanitize.log)|
|cxx26-probe|87/87 CTest PASS|[log](../artifacts/validation/test_cxx26-probe.log)|

В87 CTest входят75 GoogleTests и12 отдельных harness/инструментов. Число проверок внутри программ: {'core': 1433, 'device': 7931, 'gripper': 287, 'application': 2672, 'runtime': 784}. Mac board client29, actuator helper10, installation16, preparation4, flash26 — только локальные mocks/PTY; реальная плата не открывалась.

| Образ | BIN байт | SRAM с резервомMSP | SHA256 |
|---|---:|---:|---|
|stm32-debug|145268|53272|`04d7ca84c638f7cd97c70b9b5921f54af519b357f260cf86ed4602d7bc5578f9`|
|stm32-release|108056|53272|`4c09110c83ad26c50d070ebbd7e11e926c36f096aad3d8563186dc7710eb1feb`|
|stm32-board8-plan|106384|53280|`d5c23207f4c1ccc7352ea0b830f37a8b3d5e785b545d716eca1028be9640bf65`|
|stm32-robot2026|107344|53280|`0632b6d4fd779cb93d2527fb2dede412b3a02f2304bee021ec8dcf85972f0513`|
|stm32-robot2025-debug|144072|53280|`f834963448da409e05aed8256b83bfb24a166d17bc3b449dfa77fbc9cf00d54b`|
|stm32-robot2025|105152|53280|`6cccfbd418a1d5f936fee7ed5e0bc16c242a4cec376ebbdf0cf8492221b554ea`|
|stm32-diagnostic-debug|142776|53280|`812812a6c80963489934f0180b796d4583c34b0330c03f4320df6094882f9ae2`|
|stm32-diagnostic|104880|53280|`354b715d288edc206874d20d1cff9ac129fb3e1034f09f2b56a9a1ae48859548`|
|stm32-cxx26-probe|108056|53272|`4c09110c83ad26c50d070ebbd7e11e926c36f096aad3d8563186dc7710eb1feb`|

Все9 ELF/HEX/BIN/MAP/manifest прошли startup/vector, Cortex-M4F hard-float, memoryregions/noRWX, отсутствие linkedheap/exceptions и требуемые реализации. Статическая SRAM включает4096B резерваMSP; это не измерение worst-case stack/WCET. Восемь основных ELF дополнительно прошли полный ELF/MAP-аудит групп модулей.

Clang static analyzer:20 translation units PASS; format19/sourcepolicy/pinneddependencies PASS.164/164 исходных справочных SHA256 совпали, оригиналы не изменены.

ROS2 Jazzy и Lyrical проверены последовательно: полный LinuxDocker build трёх пакетов и colcon tests, по3tests/0errors/0failures/0skipped. Оба RUN выполнены заново для текущих источников. Сначала Dockerdaemon был выключен; отказ доступности сохранён как prior_attempts в results.json и ros2_jazzy_daemon_unavailable.log. После запускаDocker повторён этот этап и оставшиеся проверки с тем же source fingerprint. Предыдущие успешные host/ARM/static результаты не подменялись.

Диагностические регрессии проверяют отсутствие всех внешних записей/autoопроса при старте и stop, незаполненный профиль без ложного startupfault, запрет прямого Controller.rearm и всех команд движения/изменяющего maintenance,7байт deployment_info и explicitreadonly discovery. Даже commissioned+полностью заполненные backend-настройки не обходятDIAGNOSTIC_MODE.

Первый основной захват2026: только112 во внутренних целях SyncWrite/feedback, внешнийFE допустим; все3 положения, составные main-рецепты, reserve1/2, replay/timeout/ESTOP проверены. Неактивный старый слот254 не используется. Подробности: [PRIMARY_GRIPPER_254](PRIMARY_GRIPPER_254.md).

Все50 шагов и журналы: [VALIDATION_REPORT](VALIDATION_REPORT.md), [results.json](../artifacts/validation/results.json). Итог упаковки и распаковки: [FINAL_READINESS_REPORT](FINAL_READINESS_REPORT.md); artifacts/package-result.json и RELEASE_PACKAGE/validation/PACKAGE_VERIFICATION.json.

## Что остаётся проверить на оборудовании

MCU/clock/SWD/Flash/защиту/backup/readback, питание и safe boot/reset, UART, внешние устройства/адреса, GPIOподтяжки/полярности, силовыеPO/токи/клапаны, направления/энкодеры, реальныеservo/stepperlimits/home/удержаниегруза, потерюсвязи/остановку/watchdog/stacks/WCET, затемROSсроботом. Эти пункты имеют HARDWARE_TEST_REQUIRED. Критерии по каждому устройству: [HARDWARE_VALIDATION_PLAN](HARDWARE_VALIDATION_PLAN.md).

В ходе этой подготовки реальный ST-LINK не перечислялся, MCU не подключался, Flash/UART не читались и не записывались, механизмы не запускались. Единственный вызов установленного CubeProgrammer был offline `-h` для проверки синтаксиса инструкций. Более ранняя попытка другого среза, завершившаяся0ST-LINK, не относится к текущему образу.
