# Журнал стадии — HldsMonitoring (телеметрия ReHLDS)

**Дата:** 2026-10-09
**Стадия:** v1 — Linux-плагин сбора телеметрии + офлайн-анализ.
**Артефакты:** `docs/design.md` (rev 2, reviewed), `docs/plan.md` (rev 2, reviewed), код в `src/`, `tools/analyzer/`, `AGENTS.md`, `include/` (вендоренные заголовки).

## Профиль (bindings)
- Design-review: `oracle`; Plan-review: `momus`; язык: русский; коммит — только по явной просьбе.
- Верификация: `make`, `make verify-exports`, `make test`, `make test-host-core`.
- Решения владельца (design §16): Linux-first; файлы JSONL с суточной ротацией (FTP); строго пассивный v1; внешний агент — не в v1; внутренний инструмент.

## Что сделано

| Фаза | Результат | Доказательство |
|---|---|---|
| 0 | Скелет, Makefile (`-m32 -Werror`), `version_script.lds`, `exports.whitelist`, вендоринг SDK (ReAPI@95089cb: cssdk+metamod+com_*.h; 193 заголовка) | `make` → `.so` 32-bit; `make verify-exports` OK |
| 1 | Host-core: `metrics.h` (11 POD-записей), `ring_buffer.h`, `delta.h`, `serialization.{h,cpp}` (no-alloc JSONL) | `test_ring/test_delta/test_serialization/test_no_alloc` — PASS |
| 2 | `log_writer.{h,cpp}`: JSONL, суточная UTC-ротация, 0600/0700, size-cap, fallback-сигнал | `test_log_writer` — PASS |
| 3 | Движковая обвязка: `bootstrap`, `hook_manager`, `hooks`, `sampler`, `events`, `engine_layout`, `platform_linux` (+ краш-маркер), `plugin.h`, `runtime_ring.h` | `make` `-Werror` чистый; `test_engine_layout`, `test_hookchains`, `test_runtime_ring` — PASS |
| 4 | `control.{h,cpp}`: `hldsmon_status`, `hldsmon_flush_log` | `make` OK |
| 5 | `tools/analyzer/hldsmon.py` + фикстуры (все 11 типов P1) + 23 теста | `python3 -m unittest` — OK |

Итог: `make && make verify-exports && make test` — зелёные. 8 C++ host-тестов + 23 Python-теста.

## Доп. аудит после реализации
Скептическая перепроверка кода (то, что не ловят компилятор и host-тесты) выявила и закрыла два пробела против плана:
1. **Отсутствовал краш-обработчик (plan Фаза 3 / design §9).** Добавлен `platform_install_crash_handler`: async-signal-safe маркер в пред-открытый `<dir>/hldsmon-crash.marker` для `SIGSEGV/SIGABRT/SIGBUS/SIGFPE` (`SA_RESETHAND` + `write` + `kill`); вызывается из `open_log()` с эффективной (в т.ч. fallback) директорией.
2. **`runtime_ring.h` не имел unit-теста** (при том что заявлен host-testable). Добавлен `tests/test_runtime_ring.cpp`: проверяет единственную аллокацию на `init`, отсутствие аллокаций на `push`, порядок oldest-first, перезапись при переполнении, клампинг `last()`, `clear`, ре-инициализацию.
3. Добавлен `README.md` (сборка/установка/cvar/анализ) — закрывает документарную часть Фазы 6.

## Ключевые решения и почему
- **Native Metamod (C++), не AMXX/ReAPI.** Только native даёт полный `client_t`/`netchan_t` и хуки `PreprocessPacket`/`SV_Frame` (G1/G2). Вендоринг ReAPI SDK даёт проверенные структуры без сборки движка.
- **Файловое логирование вместо mmap.** По решению владельца (FTP-выгрузка). Упростило crash-модель и сняло M4/M5 из review.
- **INV-1 (пассивность).** Каждый из 15 хуков вызывает `callNext` и возвращает его результат; `PreprocessPacket` **никогда** не возвращает `false` (иначе сервер сломается — B1 из design review).
- **INV-2 (no-alloc).** Сериализация пишет в вызывающий буфер; `test_no_alloc` доказывает 0 аллокаций на drop-пути.
- **INV-4 (ABI).** `client_t*` только через `IRehldsServerStatic::GetClient_t(GetId())`; на `Meta_Attach` — чистая host-testable проба (`probe_ok`), при несовпадении — fail-closed.
- **Приоритеты.** `SV_DropClient` — `HC_PRIORITY_UNINTERRUPTABLE` (голова цепочки); остальные — default.

## Как учтены ревью
- **Design-review (oracle): PASS-WITH-REVISIONS.** B1→контракт `callNext`+propagate; B2→runtime-проба + `GetClient_t`; M1→приоритет+fallback-детект; M2→`gen`-тег и сброс кольца на `ClientConnected`; M3/M4/M5→устранены сменой модели хранения; M6→QAS-метод+non-interference; M7→парсинг userinfo (`rate/cl_updaterate/cl_cmdrate/name`)+`rel_fill`; M8→0600/0700+size-cap; M9→`GetMaxClients()` в рантайме; M10→взвешенная матрица.
- **Plan-review (momus): PASS-WITH-REVISIONS.** H1→no-alloc сериализация+`test_no_alloc`; H2→регистрация всех cvar в bootstrap; H3→таблица QAS+`test_hookchains`; M1→поля `hldsmon_status`; M2→Приложение P1; M3→записи netcmd/massdrop/voice; M4→whitelist-diff; M5→деградация/`enable=0`/диск; M6→host-testable предикат.

## Верификация (снимок)
- `make` — 32-битный `.so`, `-Wall -Wextra -Werror`, без предупреждений.
- `make verify-exports` — определённые динамические символы ровно `exports.whitelist`.
- `make test` — 7 C++ host-тестов + 23 Python-теста, все PASS.
- `ldd` — зависимости: libstdc++/libm/libgcc_s/libc (движковых неразрешённых символов нет).

## Интеграция (Фаза 6.6, тестовый сервер)
Сервер: CS 1.6 ReHLDS (`engine_i486.so`, API **3.14**) + Metamod-R 1.3.0 + ReUnion/AMXX/ReChecker/…, запущен через lgsm в Docker (glibc контейнера 2.31). Целевой хостинг — старая glibc (≤ 2.17).

Результат: плагин загружается **RUN**, runtime-проба layout пройдена (`layout_checked=32`), серверные метрики пишутся 1 Гц, JSONL валиден, анализатор проверен на реальном логе (56 записей: 53 server/1 packet/2 hitch), `hldsmon_status` работает, права 0700/0600, суточная ротация. QAS-3 (`kill -9`) — хвост лога остаётся валидным JSON (потери ≤1 с). QAS-6 (несовпадение API) — чистое `fail load` без краха (fail-closed).

Исправлено в ходе интеграции:
1. **Путь логов.** CWD движка = `serverfiles`, а аддоны — под `cstrike`. Резолв относительного `hldsmon_dir` через `pfnGetGameDir` + рекурсивный `mkdir -p` в `LogWriter` (иначе падало в `/tmp`).
2. **Чтение cvar.** Раньше читался наш статический `cvar_t` (не обновлялся движком → `enabled: no`). Теперь указатели берутся через `pfnCVarGetPointer`.
3. **Сборка под старый хостинг.** `-static-libgcc -static-libstdc++` + сборка образом `hun1er/oracle-7-cxx-build-env-gnu` → максимум `GLIBC_2.17`, без зависимостей libstdc++/libgcc.
4. **Гейт версии API.** Сервер отдаёт API **3.14**; хукчейн-vtable идентичен 3.15, отличие лишь в трейлинг-функции `SetServerPause` (не используется) → минимум понижен до 3.14.

Ограничение: на тестовом сервере нет ботов/клиентов, поэтому per-client и drop-пути в рантайме не проверены (покрыты host-тестами и unit-тестом `build_drop_record`).

## Стадия 2 (v0.2.0): события shutdown/crash, идентичность клиентов, исключения аномалий, UI+FTP

**События остановки** (исследовано по исходникам ReHLDS/Metamod-R):
- Надёжный graceful-хук — `NEW_DLL_FUNCTIONS::pfnGameShutdown`. `Meta_Detach` на выделенном сервере **не** вызывается (Metamod-R пропускает `unload_all` при `g_dedicated_server`). Отдельного `SV_Shutdown` hookchain нет.
- `shutdown` (`reason:"graceful"`) пишется из `pfnGameShutdown` (покрывает `quit`/`exit`/SIGTERM/SIGINT). `crash` — best-effort JSONL-строка из signal-обработчика (async-signal-safe `write` в dup лог-fd) + маркер-файл.
- **Найдено на живом сервере:** после `quit` движок делает `abort()` (SIGABRT), из-за чего появлялся ложный `crash`. Исправлено: после записи graceful-`shutdown` флаг глушит последующий аварийный сигнал (teardown-шум, не краш).

**Идентичность клиента:** `name`/`authid` (Steam2, копируется сразу — ротирующийся буфер)/`steamid64`/`ip`+`port`/`userid`/`slot`/`conn_s`/`fake`/`proxy`; кэш на `ClientConnected`, живьём при сэмпле и дропе (в `SV_DropClient` идентичность ещё цела). Только геттеры (пассивно).

**Анализатор:** типы `shutdown`/`crash`; `exclusion_windows` (mapchange/shutdown/crash/warmup); `log_end_state`; `find_anomalies(exclude=True)` + `find_anomalies_detail`; `client_series`/`client_identities`; CLI `--no-exclude`; `digest` += `log_end`/`exclusions`/`anomalies_excluded`.

**UI:** выбор периода (от/до), per-client графики с выбором клиента, колонки идентичности, кнопка «→ клиент» из дропов, полосы событий на графиках, бейдж завершения. **FTP-live:** полная выборка → инкрементальное дочитывание по offset (stdlib `ftplib`; креды только в памяти сервера).

**Верификация (живой сервер, ReHLDS API 3.14):**
- `make` 0.2.0, `make verify-exports` OK, **84 теста OK** (C++ host + Python).
- `quit` → `{"type":"shutdown","reason":"graceful"}`; ложного `crash` нет.
- `kill -SEGV` → `{"type":"crash","signal":11}` + маркер `crash signal=11`.
- `kill -9` → новых терминальных записей нет.
- FTP (фейковый downloader): connect / append / частичная строка / rotation / close — корректно.
- UI: скриншот в headless Chrome (период, полосы событий, per-client, идентичность) — без ошибок.

## Стадия 2b (v0.2.1): диагностика «Timed out» дропов
- **Исследование ReHLDS:** таймаут — `SV_CheckTimeouts`, условие `realtime - netchan.last_received >= sv_timeout` (default 60), reason `"Timed out"`, crash=false; хук `SV_DropClient` срабатывает **до** очистки состояния (чит. до `chain->callNext`).
- **Добавлено (аддитивно, v2):** `client.idle_ms` (простой в мс на сэмпле — тренд «клиент замолчал»); `drop.netchan.now_real` (engine realtime на дропе), `drop.netchan.idle_ms`, `drop.netchan.sv_timeout`. Ранее уже был `drop.client.fully_connected` (играл vs не дозалогинился).
- **Анализатор/UI:** `client_series` += `idle_ms`; per-client график «Простой, мс»; в таблице дропов — колонки «простой» и «был в игре».
- **Верификация:** `make` (0.2.1), `verify-exports` OK, **87 тестов OK** (хост-тесты сериализации новых полей); UI-скриншот (график «Простой, мс», колонки дропов). Пересобрано oracle-образом (glibc 2.17), задеплоено, `meta list` → v0.2.1 RUN.

## Стадия 2c (веб-морда): сессии, трафик, период-таймлайн, таблицы
- **Сессии:** клиенты переведены на ключ `(run, userid)` со связью между запусками по SteamID (анализатор + UI; плагин `userid` уже писал). Разбиение на запуски по `shutdown`/`crash`.
- **Трафик:** в `client_series` добавлены `flow_in_kbps`/`flow_out_kbps`; per-client график «Трафик, KB/s»; в «Все датасеты» — выбор игрока и источника трафика (overlay).
- **Период:** таймлайн-браш (перетаскивание границ) + пресеты + режим **«Слежение»** (верхняя граница без ограничения → новые пакеты автоматически в выборке, правый ус идёт за максимумом). Тултип графиков не уезжает за края и не создаёт скролл.
- **«Все датасеты»:** при выборе игрока его клиентские метрики (ping/loss/choke/fill/idle/Δseq/Δloss/трафик in/out) доступны отдельными переключателями, **по умолчанию все выключены** — включаются по выбору.
- **Таблицы:** пагинация (25/50/100/все) для клиентов и дропов; панели сворачиваются (клик по заголовку, «Свернуть всё/Развернуть всё»); «Отключения (drops)»/«Аномалии» свёрнуты по умолчанию.
- **Верификация:** `node --check`, 102 python-теста; проверки в headless Chromium (тултип у левого/правого края — внутри карточки, `plotHScroll=0`; drag таймлайна фильтрует период). Плагин не менялся.

## Стадия 2d (веб-морда, продолжение)
- Клиенты агрегируются по **SteamID**: `view.players` с объединённым по всем сессиям рядом; в «Все датасеты» — выбор игрока (объединённый оверлей) + список его сессий; per-client — «весь игрок» либо конкретная сессия; таблица/списки отсортированы по `userid`.
- **Sticky-таймлайн** (панель «Период» закреплена под шапкой, `--topbar-h`).
- Поля периода показывают **дату** (fmtIso), а не epoch; оверлей на общем графике **обрывается** на последнем сэмпле клиента (нет плоского «хвоста» после дисконнекта).
- **Перерисовка не мешает вводу:** авто-обновление пропускает такт, пока открыт список/поле (`isUiInteracting`), фокус восстанавливается после `render`. **Свёрнутость панелей** сохраняется в `localStorage` (`hldsmon.ui.collapsed.v1`).
- Верификация: `node --check`, 102 python-теста; в headless Chromium — merged-оверлей игрока, список сессий, sticky под шапкой, даты в полях, обрыв линии, сортировка по userid, сохранение свёрнутости после reload, guard фокуса. Плагин не менялся.
- Фильтры (причина дропа, клиент, игрок) **не сбрасываются при пустом за период наборе** — выбранное сохраняется и показывается как опция.

## Стадия 2e (веб + анализатор): зависания клиента, примечания, глобальный выбор
- **Анализатор:** `focus_points(records)` — расширяемый (по `kind`) список моментов; детектор `idle_hang` ловит непрерывный рост `idle_ms` (≥ 5 с и прирост ≥ 2 с), отмечает `drop_after`; в payload (`focus_points`) и `digest` (`focus_point_count`). Плагин не менялся (`idle_ms` уже пишется).
- **Веб:** «Все датасеты» первым; примечания к метрикам (`title` + «Справка»); **единый глобальный выбор клиента** (один селектор `#clientPicker` в sticky-блоке «Период»); панель **«События»** с кнопкой «фокус» (период ±10 с + выбор игрока + скролл к общему графику); настройки датасетов (в т.ч. **единый набор клиентских метрик** — не per-player; фокус его не трогает) в `localStorage` (`hldsmon.ui.datasets.v1`).
- **Верификация:** `node --check`, 111 python-тестов; Chromium — порядок панелей, примечания, синхронизация выбора, кнопка фокуса, восстановление настроек после reload.
- Прогресс FTP получил фазу **«Разбор данных…»** (`phase: download|parse`): после скачивания бар не «висит» на 100%, а показывает разбор (индeterminate, amber).

## Открытые темы
- **O1 (частота/объём логов).** `hldsmon_client_hz`/`hldsmon_disk_hz` (default 1) — уточнить по фактическому объёму (~300–600 МБ/сутки при 32 клиентах) и стоимости FTP.
- **O2 (PII/retention).** IP/SteamID/userinfo хранятся без авто-удаления; владелец чистит файлы.
- **O3 (целевые сборки ReHLDS). РЕШЕНО:** целевой сервер — ReHLDS API **3.14**; минимум понижен до 3.14 (хукчейн-vtable совпадает с 3.15). Требование хостинга: glibc **≤ 2.17** — сборка через образ `hun1er/oracle-7-cxx-build-env-gnu` со статическим линком libgcc/libstdc++.
- **Интеграция (Фаза 6).** Требует реального ReHLDS-сервера: QAS-1 (overhead), QAS-3 (`kill -9`), QAS-6 (нет API), smoke с реальным дропом. Доступ к тестовому серверу предложен владельцем.
- **Фаза 2 (Windows)** и **Фаза 3 (внешний агент с egress-push)** — отдельными стадиями.

## Следующие шаги
1. Установить плагин на тестовый сервер (`addons/metamod/dlls/`, строка в `plugins.ini`), проверить `meta list`, `hldsmon_status`, появление `hldsmon-YYYY-MM-DD.jsonl`.
2. Прогнать сценарии QAS-1/3/6, зафиксировать результат здесь.
3. Скорректировать частоты (O1) по факту.
4. По явной команде владельца — один связный коммит стадии.

## Коммит
Не выполнен — согласно профилю требуется явная просьба владельца.
