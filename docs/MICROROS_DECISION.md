# ROS 2 transport и оценка micro-ROS

В этой реализации выбран собственный ограниченный бинарный UART transport и ROS 2 bridge. Это инженерное решение для текущего проверяемого проекта, а не утверждение, что STM32F407 не поддерживает micro-ROS.

Официальный перечень micro-ROS содержит Olimex STM32-E407 с Cortex-M4F и FreeRTOS/UART: https://github.com/micro-ROS/micro-ROS.github.io/blob/master/_docs/overview/hardware/index.md . STM32F407VG имеет 1 МиБ Flash и 192 КиБ SRAM, из которых 64 КиБ CCM доступны только ядру: https://www.st.com/en/microcontrollers-microprocessors/stm32f407vg.html . CCM нельзя суммировать с обычной SRAM как единый DMA buffer pool.

Текущий release занимает приблизительно52 КиБ загрузочного Flash и33 КиБ обычной SRAM с резервом4096 байт основного стека; точные числа в VALIDATION_REPORT.md. Стек задач статический, очереди8+8Frame, RX-ring1024 байта, max_payload128/frame144. В firmware нет heap allocator приложения. Измерение фактического worst-case stack и WCET на плате остаётся обязательным.

Это оставляет запас памяти, но не доказывает пригодность конкретной конфигурации micro-ROS: дополнительные rcl/rclc/XRCE streams, history, MTU, число entities и стеки требуют собственного link-map и измерений. Официальное описание метода разделяет static/stack/dynamic memory: https://www.eprosima.com/developer-resources/performance/micro-ros-client-memory-profiling . Результаты других MCU не выдаются за измерения этого робота.

Фиксированный transport здесь уже собирается вместе с контроллером, имеет проверяемые owner/sequence/lease и offline/PTY tests. При20Гц команд движения heartbeat+twist и их84-байтные ACK потребляют около4880 байт/с по обоим направлениям суммарно; sensors2Гц добавляют около114 байт/с. Это оценка wire bandwidth, не измерение latency.115200 8N1 допускает11520байт/с в каждом направлении; последовательные round-trip и RTOS нагрузка должны проверяться отдельно. Все bus waits имеют deadline; ROS executor не выполняет локальный цикл движения STM32.

micro-ROS можно оценить позже отдельной сборкой на том же core API, не создавая второй путь управления периферией. До такой проверки middleware не включается в real-time firmware и не называется реализованным. Для новой платы, которой требуется общий DDS API на MCU, решение можно пересмотреть по измерениям, сохранив единственного владельца ресурсов и локальную safety logic.
