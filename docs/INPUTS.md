# Кнопки и цифровые входы

`firmware/include/eurobot/inputs.hpp` содержит DebouncedInput и InputBank<13> без heap/исключений. Набор каналов допускает 1–32 входа, основной банк соответствует 13 подтверждённым входам платы. Электрические уровни подаёт board adapter; драйвер фильтрации не выдумывает GPIO.

InputConfig задаёт active_high, debounce_ms (default8), hold_ms (default500,0 отключаетhold) и maximum_sample_gap_ms (default20). Интервалы меньше 2^31 мс, debounce и gap положительны. Смена полярности задаётся конфигурацией, а не инверсией смысла события.

Первое измерение неизвестно, пока кандидат не стабилен в течение debounce. Даже если вход уже нажат при запуске, после инициализации он не генерирует press/hold/click: нужен настоящий цикл release→press. Последующие устойчивые изменения дают pressed/released один раз. Hold выдаётся один раз после hold_ms от подтверждённого нажатия. Click выдаётся только при отпускании короткого настоящего нажатия; послеhold и при отпускании первоначально зажатого входа click отсутствует.

`sample(level, now)` возвращает snapshot, битовые Event и Error. `snapshot(now)` инвалидирует известность при устаревании даже без нового sample. Превышение максимального gap сбрасывает фильтр в неизвестное состояние и возвращает timeout; clock backjump — invalid_argument. Переполнение uint32 миллисекунд поддерживается при регулярном sampling. `invalidate()` требует повторной стабильной инициализации.

InputBank возвращает независимые masks known/pressed/pressed_edges/released_edges/held_edges/clicked_edges. Это результат текущего sample, не скрытая очередь событий. Потребитель обязан обработать edges; application::Application преобразует их в callback/event и match policy, сохраняя фильтр независимым от приводов. Биты raw mask за пределами банка отвергаются без изменения состояния.

## Реальное подключение приложения

В runtime статически создаётся `application::Application` с Deployment.application. Control task инициализирует его, раз в 10 мс считывает board::input0..12, передаёт sample_inputs(raw,now) и выполняет tick(now). Порядок2026: starting_cord, playground_side, strategy, motor_enable, switch1..switch9. Inputs0/3 являются критическими для match policy. Электрическая polarity/debounce/hold задаётся профилем. Механический смысл девяти switches сохраняет legacy inversion и публикуется отдельно от raw levels.

Application хранит input_known/input_pressed, switch_known/switch_pressed, side/strategy/motor request и callback events init_complete/state_changed/click/hold. Queue имеет64 элемента; overflow увеличивает dropped_events, не выделяет память. Стратегия переключается по click modulo strategy_count (default 3); startup нажатие не создаёт click. Устаревшие критические входы останавливают активный матч.

Матч требует существующего explicit armed owner, известного вставленного cord и motor request. MATCH_ARM переводит в waiting_for_release; отпускание cord начинает running и счёт времени. Game deadline по активному2026 коду —99000 мс; debug mode сохраняет остальные safety guards, но не этот game deadline. Возврат cord, снятие motor request, пропавшая authority, неизвестные входы, выключенная communication или deadline вызывают stop callback. В runtime он останавливает сессию через ESTOP. Начальное открытие reserve side выполняется только явно разрешённым config и при свободном Gripper engine; занятая очередь даёт start_action_skipped, а не скрытый повтор.

Read-only Maintenance21 возвращает application snapshot24, Maintenance28 извлекает event9. Сервисные MATCH_ARM/DISARM, APPLICATION_MODE, COMMUNICATION_ENABLED, reset/read-reset/clear-reset используют единую authority и typed maintenance API. Низкоуровневый sensors9 и ROS BoardInputs сохраняют raw levels/known mask; это отдельное представление и не отменяет реально работающую filtered match policy.

2025 motor-button behavior доступен только через legacy_2025_motor_button_control; событие без действующей authority не ставится в очередь до будущего arm. Аппаратный ESTOP и снятие энергии остаются независимыми от debounce и Application.

`tests/input_tests.cpp` содержит 11 GoogleTests для фильтра; application policy отдельно покрыта tests/application_tests.cpp и runtime harness. Итог текущего запуска см. VALIDATION_REPORT.md. Проверены bounce, initial-held suppression, click/hold, stale sampling, polarity, backwards time, wraparound,13 независимых входов и граница 32 бита. Физические переключатели не испытывались.
