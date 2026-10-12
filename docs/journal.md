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

## Стадия 2f (FTP): хвост-окно + «загрузить раньше»
- FTP-загрузка переделана на **range-чтение**: `_remote_size`, `_download_range` (`REST`/`transfercmd`), обратный постраничный догруз (`FTP_BACK_CHUNK`, `FTP_WINDOW_S`). При connect грузятся **последние ~30 мин**, кнопка «← загрузить раньше» догружает предыдущее окно (до начала файла). Границы строк обрабатываются (`_lines_in_range`: обрезка частичной первой строки, хвостовой leftover). Сессия хранит `start`/`end`.
- **Найдено/исправлено при сквозном тесте:** `SIZE` требует бинарного режима — `_ftp_open` теперь шлёт `TYPE I` (иначе `ftp.size` → «550 SIZE not allowed in ASCII mode» и хвост-окно не включалось).
- Верификация: 114 python-тестов + сквозной тест против реального FTP (pyftpdlib): connect грузит только окно (344 из 3000 записей), `load-earlier` префиксует старее **без дыр/дублей** (проверка непрерывности `t`), refresh добавляет только новое. Фронт: кнопка + `ftp.reachedStart`.

## Стадия 3 (modnet): атрибуция сетевого трафика по `.so`
- **Контекст.** Чужие in-process модули (AMXX-модули `easy_http`/`sockets`) генерируют сетевой трафик внутри серверного процесса; его нужно отделить от движкового и серверного. Дизайн: `docs/design.md` §19 (rev 3); план: `docs/plan.md`, раздел «Стадия 3 — `modnet`» (фазы M-P0..M-P8).
- **Гейт-трейл.** Дизайн (software-architect) → ревью `oracle` = PASS-WITH-REVISIONS (Critical C1 ленивый биндинг; H1 трафик glibc-резолвера; H2 гонка потоков; H3 blast radius; H4/H5 допущения; H6 проверяемость) → правки → re-review PASS (остатки R1..R6) → остатки учтены → вопросы владельцу закрыты → план → ревью `momus` = PASS-WITH-REVISIONS (MN-1..MN-8) → учтены → гейт реализации открыт.
- **Решения владельца.** Пустой `hldsmon_modnet_targets` ⇒ встроенный список `{easy_http_amxx_i386.so, sockets_amxx_i386.so}`; охват — только чужие модули (сам объект движка не атрибутируется); проверка — 32-битный синтетический `.so`-стенд в CI.
- **Механизм.** Интерпозиция GOT/PLT у загруженного целевого `.so` (ELF32: `dl_iterate_phdr`, релоки `R_386_JMP_SLOT`/`R_386_GLOB_DAT`); атрибуция по **вызывающему** `.so` через адрес возврата + кэш диапазонов (lock-free публикация). Оригинал берётся из `dlsym(RTLD_DEFAULT, name)`, **никогда** из GOT-слота (фикс C1). Политика BIND_NOW (b): перепроверка при rescan + пере-патч; fail-closed при отказе `mprotect`. Восстановление — compare-then-store (R2). Постоянная таблица сохранённых оригиналов.
- **Метрики.** Байты: `{send,recv,sendto,recvfrom,sendmsg,recvmsg}`; соединения: `{connect,accept}`; счётчик DNS: `getaddrinfo`. R1: `EINPROGRESS`/`EALREADY` = pending; `conns` = успешные блокирующие `connect()`+`accept()`; `conn_fail` = `connect()` с ошибкой; `connect_attempts` = все `connect()`; `dns_calls`. Новый тип JSONL `modnet` (Приложение P1, расширение).
- **Конфиг/эмиссия.** `hldsmon_modnet` (default 0, мастер-выключатель), `hldsmon_modnet_targets` (default `""`, пусто ⇒ встроенная пара), `hldsmon_modnet_rescan_sec` (default 5) + rescan по `SV_Spawn_f`. Эмиссия: одна запись `modnet` на каждый включённый целевой объект с 1 Гц, только для объектов с активностью.
- **Инварианты.** INV-1 расширен (прозрачность, вкл. `errno`); INV-2 расширен (обёртки без аллокаций); **INV-3 изменён** (обёртки исполняются в потоках вызывающего, вкл. пул воркеров EasyHTTP; счётчики атомарные; эмиссия JSONL остаётся в серверном потоке); новые INV-M1..M5.
- **Артефакты.** Создано: `src/modnet.h`, `src/modnet.cpp`, `src/modnet_got.h`, `src/modnet_got.cpp`, `tests/test_modnet_core.cpp`, `tests/test_modnet_serialization.cpp`, `tests/test_modnet_concurrency.cpp`, `tests/modnet_harness/{target.c,harness.cpp,README.md}`, `tools/analyzer/tests/fixtures/modnet.jsonl`. Изменено: `Makefile` (`CORE_SRCS += src/modnet.cpp`, `-pthread`, цель `test-modnet-harness`), `.github/workflows/ci.yml` (шаг стенда внутри `manylinux2014_i686`), `src/{bootstrap,sampler,control,events,hooks,plugin.h,metrics.h,serialization.*}`, `tests/test_no_alloc.cpp`, `tools/analyzer/hldsmon.py` (+разбор/series/summary/correlation/digest modnet), `tools/analyzer/README.md`.
- **Верификация (эта сессия, репо `/mnt/c`).** `make` OK (32-битный `.so`, TU modnet включены); `make test` = **138 тестов OK** (вкл. `test_modnet_core`/`_serialization`/`_concurrency` и 24 новых Python-теста modnet); `make verify-exports` OK (экспорты без изменений); `make test-modnet-harness` PASS — 72 утверждения: атрибуция, lazy/C1 (≥2 вызова), восстановление при disable, rescan (цель `dlopen` уже после enable), fail-closed проба, невмешательство (возврат + `errno` совпадают on/off). Цели стенда: `target_lazy.so` (без BIND_NOW) и `target_now.so` (`-Wl,-z,now`), обе с релоками `R_386_JUMP_SLOT` на `send/recv/connect/accept/getaddrinfo`.
- **Отклонения (честно).** Тест-файл назван `test_modnet_core.cpp` вместо планового `test_modnet.cpp` (плюс плановые `_serialization`/`_concurrency`); `ModnetState` определён в `modnet_got.h` (не в `plugin.h`), чтобы стенд линковал адаптер (`plugin.h` его включает, `PluginContext` держит поле); порог всплеска анализатора `MODNET_BURST_KBPS=50.0` (в плане числа не было); `webui.py` не менялся.
- **Открытые нити.** N6 (итого по интерфейсам / остаток) отложено; N7 (реестр read/write fd) отложено; N10 (байты DNS glibc-резолвера) вне охвата; live-пункты не сделаны — проверить статическую линковку curl/c-ares/OpenSSL/zlib (`ldd`/`nm -D` не должны показывать динамический `libcurl.so.4`) и зафиксировать идентичность задеплоенного модуля, затем включить `hldsmon_modnet` на тестовом сервере и наблюдать записи `modnet`.
- **Дальше.** Деплой на тестовый сервер, живые проверки H4/H5, включить modnet на `easy_http_amxx_i386.so`+`sockets_amxx_i386.so`, убедиться, что появляются раздельные строки трафика по модулям. CI прогоняет стенд на push.
- **Коммит.** Не выполнен — по профилю требуется явная просьба владельца.

## Стадия 3b (retention): автоудаление ротационных логов
- **Контекст.** Дневные файлы копятся без ограничения (O2). Дизайн: `docs/design.md` §20 (rev 4); план: `docs/plan.md`, раздел «Стадия 3b: `retention`» (фазы R-P0..R-P4).
- **Гейт-трейл.** Дизайн (software-architect) → ревью `oracle` = PASS-WITH-REVISIONS (R-1 рантайм-перечитывание cvar; R-2 явный fail-safe `bool retention_select(..., size_t* out_n)` вместо `SIZE_MAX`; R-3 ловушка Makefile `CORE_SRCS`; R-4..R-8 Low) → правки → вопросы владельцу закрыты → план → ревью `momus` = PASS-WITH-REVISIONS (RET-1..RET-4, учтены при реализации) → гейт реализации открыт.
- **Решения владельца.** `hldsmon_retention_days <= 0` ⇒ retention выключен (keep-all); удаление **не** запускается по `hldsmon_flush_log` (только open + суточная ротация); отдельного флага паузы нет: владелец выставляет cvar, он действует со следующей ротации благодаря R-1.
- **Семантика.** Cvar `hldsmon_retention_days` (default **3**): максимум файлов `hldsmon-YYYY-MM-DD.jsonl` **включая текущий**; держим `T - d <= N-1`, удаляем `d < T-(N-1)`; N=3 ⇒ сегодня + 2 предыдущих. Удаляются только точные дневные имена в `hldsmon_dir`; маркер краша (`hldsmon-crash.marker`) и не-дневные имена сохраняются всегда; текущий файл защищён; будущие даты сохраняются.
- **Механизм.** `prune()` в конце успешного `open_day()` (стартовый `open()` и `maybe_rotate`); чистое ядро `src/retention.{h,cpp}` (`retention_select`, `parse_log_date` с leap-aware проверкой дня, `days_from_civil`, без ФС/аллокаций); ФС-часть в `log_writer`; R-1: sampler перечитывает cvar каждый 1 Гц-тик и вызывает `set_retention_days(...)` перед `maybe_rotate()`.
- **Артефакты.** Создано: `src/retention.{h,cpp}`, `tests/test_retention.cpp`. Изменено: `src/log_writer.{h,cpp}` (`set_retention_days`, `prune`), `src/bootstrap.cpp` (+cvar, кэш по H2), `src/plugin.h` (`CvarRefs::retention_days`), `src/sampler.cpp` (runtime re-read), `Makefile` (`CORE_SRCS += src/retention.cpp`), `tests/test_log_writer.cpp` (набор prune: N=3, ротация N=1, symlink, unlink-while-open, маркер, свежий каталог, недоступный каталог), `README.md`, `docs/design.md` §12.
- **Верификация (эта сессия).** `make` OK (32-битный `.so`, без предупреждений); `make test` = **138 python-тестов OK** + все host-тесты, включая `test_retention` и расширенный `test_log_writer` (prune-сценарии зелёные); `make verify-exports` OK (whitelist без изменений). `modnet` не затронут.
- **Открытые нити.** Нет: реализация локально завершена. `dist` пересобрать после стадии (включая retention).
- **Дальше.** Коммит стадии только по явной просьбе владельца.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 4 (ping-fix): коррекция единиц `client_t::latency`
- **Контекст.** Жалоба владельца: per-client `ping` всегда `0`. Дизайн: `docs/design.md` §21 (rev 5); план: `docs/plan.md`, раздел «Стадия 4 — `ping-fix`» (фазы PF-P0..PF-P4).
- **Причина (доказана).** `client_t::latency` — `float` **в секундах** (ReHLDS `PF_GetPlayerStats` = `client->latency * 1000.0`; `common/net_api.h` документирует «Latency, in seconds»). `src/sampler.cpp:101` делал `static_cast<uint16_t>(ct->latency)` → усечение `0.040 → 0` у всех. Тот же баг в `src/hooks.cpp` (`sum_ping += o->latency`) давал нулевой `drop.neighbors.avg_ping`.
- **Гейт-трейл.** Дизайн (§21) → ревью `oracle` = **PASS-WITH-REVISIONS** (PG-1 NaN-ветка; PG-2 clamp по округлённому; PG-3 формулировка INV-4; PG-4 Makefile не трогаем; PG-5 усиление QAS-P1; PG-6 `ping_ms`→`ping`; PG-7 уточнение альтернативы B) → правки сложены → **вопросов владельцу нет** → план → **plan-ревью `momus` не проводилось** (сокращённый багфикс-трек по рекомендации oracle, design §21.12: нет развилок/схемы/ABI) → гейт открыт.
- **Решение (fix A).** Минимальный host-testable конвертер `ping_ms_from_latency(float)` в `src/metrics.h` (секунды→округлённые мс; NaN/`<=0` → `0`; clamp `0xFFFF` по округлённому значению); применяется в `sampler` и `hooks`. Источник данных и ABI-путь (`engine_layout`/`client_by_id`, INV-4) не меняются. `packet_loss` (процент 0..100) — не трогали. `ping=0` трактуется как «не измерено» (боты/first-2s/высокий разброс — легитимно).
- **Артефакты.** Изменено: `src/metrics.h` (+`ping_ms_from_latency`), `src/sampler.cpp` (каст→конвертер), `src/hooks.cpp` (сумма соседей в мс), `docs/design.md` (§21 + §6.2 `ping_ms`→`ping` + статус), `docs/plan.md` (Стадия 4). Создано: `tests/test_ping.cpp`. Makefile/schema P1/экспорты — **без изменений**.
- **Верификация (эта сессия, репо `/mnt/c`).** `make` OK (32-бит `.so`, `-Wall -Wextra -Werror`); `make test` = все host-тесты OK (вкл. новый `test_ping`) + **138 Python-тестов OK**; `make verify-exports` OK (whitelist без изменений).
- **Открытые нити.** **NT-1:** вендоренный `include/cssdk/engine/rehlds_interfaces.h` не содержит `GetLatency()/GetPacketLoss()/GetChokeCount()/GetDeltaSequence()`, хотя `rehlds_api.h` заявляет API 3.15 (методы есть в upstream с 3.10) — латентный дефект вендоринга (INV-4-риск), вынесен в отдельную стадию «re-vendor ReHLDS interfaces».
- **Дальше.** Стадия 5 = F2 (контекст запуска и rate-снимок). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 5 (config-context): контекст запуска и rate-снимок
- **Контекст.** При триаже дропов нужны настройки сервера на момент инцидента (rate/timeout/tickrate); ранее при дропе фиксировался только `sv_timeout`. Запрос владельца — «Уровень 0». Дизайн: `docs/design.md` §22 (rev 6); план: `docs/plan.md` «Стадия 5 — `config-context`» (фазы CFG-P0..CFG-P6).
- **Гейт-трейл.** Дизайн (§22) → ревью `oracle` = PASS-WITH-REVISIONS (CFG-1..CFG-8: обязательный `KNOWN_TYPES`; эмиттер `null` + `present`-битмаска; дубль `sv_timeout`; гейт/baseline; `memset`; схлопывание <1 с; сверка имён cvar; общий суб-эмиттер) → folded → **повторное ревью = PASS** (design gate open) → план → ревью `momus` = PASS-WITH-REVISIONS (CM-1..CM-5: сохранение `netchan.sv_timeout`; состав `drop.config`; `test_no_alloc`; CFG-6/7 в README/журнал; инициатор boot) → folded → гейт открыт.
- **Решение.** Новый тип JSONL `config` (boot при старте + при изменении любого отслеживаемого cvar; поллинг 1 Гц + diff с кэшем `cvar_t*`), плюс вложенный `config` в `drop` (общий суб-эмиттер; авторитет; `netchan.sv_timeout` сохранён для P1, CM-1). Отслеживается: `sys_ticrate`, `host_framerate`, `sv_timeout`, `sv_max`/`min`rate, `sv_max`/`min`updaterate, `sv_max`/`min`cmdrate`, версии/map/maxclients. Отсутствующий cvar → `null` (новый эмиттер `null`; `append_fixed` non-finite→0 не годится). Первый включённый тик → baseline. Схлопывание изменений <1 с — задокументировано (CFG-6).
- **Артефакты.** Изменено: `src/metrics.h` (`ConfigSnapshot`=38, `ConfigRecord`=119, `config_changed`, `DropRecord`+`config`, asserts), `src/serialization.{h,cpp}` (`write_config_json`, `kv_null`/`kv_fixed_or_null`, `emit_config_numbers`, `drop.config`), `src/events.{h,cpp}` (`write_config`), `src/plugin.h` (+9 `cvar_t*` в `CvarRefs`, `last_config`/валидность), `src/bootstrap.cpp` (+`tracked_cvar`), `src/sampler.{h,cpp}` (+`build_config_snapshot`, baseline/change), `src/hooks.cpp` (`rec.config`, `sv_timeout` из кэша), `tools/analyzer/hldsmon.py` (`KNOWN_TYPES += "config"`, `summary`/`digest`), `tools/analyzer/README.md`, `README.md`, `docs/plan.md` (Приложение P1). Создано: `tests/test_config.cpp`; расширены `tests/test_serialization.cpp`, `tests/test_no_alloc.cpp`, `tests/test_serialization_events.cpp` (пин `DropRecord` 16413→16451), `tools/analyzer/tests/fixtures/types_ok.jsonl`, `tools/analyzer/tests/test_hldsmon.py`. `Makefile`/`exports.whitelist`/`version_script.lds` — без изменений.
- **Верификация (эта сессия).** `make` OK (32-бит, `-Wall -Wextra -Werror`, нуль предупреждений); `make test` = все host-тесты PASS (вкл. новый `test_config`) + **141 Python OK**; `make verify-exports` OK (whitelist без изменений); INV-5 runtime-проверен (`json.loads` на `config`/`drop.config` с `null`).
- **Отклонения.** UI-панель конфигурации не реализована (вне перечня работ исполнения; анализатор учитывает `config` через `KNOWN_TYPES`); smoke на реальном ReHLDS и сверка имён/наличия cvar (CFG-7) — вне CI/офлайн; build-номер не эмитится (геттера нет).
- **Дальше.** Стадия 6 = F3 (host-metrics). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 6 (host-metrics): хост/ядро-телеметрия (best-effort)
- **Контекст.** Игровой путь HLDS виден, хост — нет (переполнение UDP-буфера ядра, NIC-дропы, CPU steal на VPS, диск). Запрос владельца — «Уровень 1» (best-effort, т.к. хостинг может не отдавать корректно). Дизайн: `docs/design.md` §23 (rev 7); план: `docs/plan.md` «Стадия 6 — `host-metrics`» (HM-P0..HM-P6).
- **Гейт-трейл.** Дизайн (§23) → ревью `oracle` = PASS-WITH-REVISIONS (H1 детектор окружения вместо несостоятельного `self==PID1`; H2 Δ-семантика; M1..M5) → folded → **повторное ревью = PASS** → план → `momus` = PASS-WITH-REVISIONS (HM-1..HM-6) → folded → гейт открыт → реализация.
- **Решение.** Новый тип `host` (1 Гц) под cvar `hldsmon_host` (**default 0/OFF**, opt-in); engine-free `src/host_metrics.{h,cpp}` (POSIX): parse-by-name `/proc/net/snmp` (UDP `RcvbufErrors`/`InErrors`/`InCsumErrors`), `/proc/net/dev`, `/proc/stat` (steal %), `/proc/self/io`, `statvfs` по `hldsmon_dir`; чистая детекция окружения (контейнеризация: `cgroup`-маркеры `docker/lxc/kubepods`, `/.dockerenv`, lxcfs, `container=`; `EACCES`/unknown ⇒ caveated); Δ-семантика (первый сэмпл ⇒ `null`); маски `present` (по-полям) / `caveat` (по-источникам). Недоступное ⇒ `null`; caveated ⇒ значение присутствует + бит.
- **Артефакты.** Создано: `src/host_metrics.{h,cpp}`, `tests/test_host_metrics.cpp`, `tools/analyzer/tests/fixtures/host.jsonl`. Изменено: `src/metrics.h` (`HostRecord`), `src/serialization.{h,cpp}` (`write_host_json`, integer-null), `src/events.{h,cpp}` (`write_host`), `src/plugin.h`+`src/bootstrap.cpp` (cvar `hldsmon_host`=0, `probe_env`/`detect_env`, `host_caveat_mask`), `src/sampler.{h,cpp}` (1 Гц сбор/эмиссия), `Makefile` (`CORE_SRCS += src/host_metrics.cpp`), `tools/analyzer/hldsmon.py` (`KNOWN_TYPES += "host"`, `summary`/`digest`), `README.md`, фикстуры/тесты, `docs/plan.md` P1. `exports.whitelist` — без изменений.
- **Верификация (эта сессия).** `make` OK (32-бит, `-Werror`); `make test` = все host-тесты PASS (вкл. новый `test_host_metrics`) + **147 Python OK**; `make verify-exports` OK.
- **Отклонения (честно).** Детекция контейнера — по **маркерам** cgroup (`docker/lxc/kubepods`/`containerd`/`podman`), а не по «путь ≠ `/`» (иначе ложные срабатывания на systemd-хостах); добавлен бит `kCaveatTruncated` (обрезка чтения; L3) и эмиссия integer-`null`.
- **Дальше.** Стадия 7 = F4 (client-quality): design §24 свёрнут, но **plan-гейт на развилках владельца D1/D2** (§24.9). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 7 (client-quality): jitter per-client + скользящий loss
- **Контекст.** Ур.2: набор §6.2 не различает транзиентный спайк и устойчиво плохой линк (нет jitter/скользящего loss). Дизайн: `docs/design.md` §24 (rev 8); план: `docs/plan.md` «Стадия 7».
- **Гейт-трейл.** Дизайн → oracle PASS-WITH-REVISIONS (H1/H2/M1-M4 folded); развилки D1/D2 **решены автономно** (D1: `reliable_overflow` вынесен из scope; D2: окно по `loss`); план → momus PASS-WITH-REVISIONS (CQ-1..CQ-5 folded, gate open).
- **Решение.** `ClientSample` +3 jitter-поля (`jit_min/avg/max_ms`); inline host-testable аккумулятор (Δ межпакетных интервалов, `GetRealTime`, guard/clamp); IP+port матч в `PreprocessPacket` (≤32, integer-only, гейт `client_hz>0`); rolling loss-rate — в анализаторе по `loss` (без новых полей/`in_seq`).
- **Артефакты.** Изменено: `src/metrics.h` (jitter; `ClientSample` 247→**253**, `DropRecord` 16451→**16835**), `src/plugin.h` (`ClientRuntime`: packed ip/port, jitter-аккумуляторы, `reset_client_pkt_state`), `src/hooks.cpp` (`accumulate_packet_jitter`; reset connect/disconnect), `src/sampler.cpp`, `src/serialization.cpp`, `tools/analyzer/hldsmon.py` (`loss_rate_series`/`rolling_mean`), `README.md`, `docs/plan.md` P1, `docs/design.md` §24 (гигиена). Создано: `tests/test_client_quality.cpp`, fixtures `client_quality.jsonl`.
- **Верификация.** `make` OK; `make test` = host PASS (вкл. `test_client_quality`) + **150 Python OK**; `make verify-exports` OK. RED-proof: поломка `avg` → тест падает.
- **Отклонения.** `pkt_has_prev`; `LOSS_RATE_WINDOW=5`; `test_no_alloc` окно 50→64.
- **Дальше.** Стадия 8 = F5 (connect-timings). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 8 (connect-timings): тайминги коннекта + длительность загрузки карты
- **Контекст.** Медленный коннект/auth-стопор и долгий мапчейндж дают дропы; стадии/длительность карты не фиксировались. Дизайн: `docs/design.md` §25 (rev 9); план: `docs/plan.md` «Стадия 8».
- **Гейт-трейл.** Дизайн → oracle PASS-WITH-REVISIONS (CT-1..CT-9 folded: единый контекст коннекта вместо per-slot при без-аргументном `SV_ConnectClient`; delayed-emission `mapchange`; предикат готовности; `GetRealTime`+guard). Q1 решён автономно (готовность = первый tick с доступным миром/`current_map()`, окно 30). План → momus PASS-WITH-REVISIONS (CTP-1..CTP-6 folded: present-бит+`kv_u32_or_null`; конкретный предикат+хранение; синхронность; host-testable конвертер; `static_assert`; P1-схема).
- **Решение.** `ConnectCtx`/`MapCtx` в `PluginContext`; `RejectRecord` +`stage_ms`/`total_ms`; `ConnectRecord` +`auth_ms`/`steam_ms`; `MapChangeRecord` +`load_ms` (nullable через `timing_present`/`load_present`); `timing_delta_ms()` — host-testable конвертер (guard `<=0`/NaN/±inf, round, saturate). `SV_Spawn_f` — только метка + сохранение massdrop-reset и `strcmp`-гейта; `mapchange` эмитится на 1 Гц при готовности/окне 30 тиков.
- **Артефакты.** Изменено: `src/metrics.h` (поля+биты+конвертер+`static_assert`), `src/plugin.h` (`ConnectCtx`/`MapCtx`+reset), `src/hooks.cpp` (тайминги стадий, reject/connect, spawn-метка), `src/sampler.cpp` (`emit_pending_mapchange`), `src/serialization.cpp` (`kv_u32_or_null`+поля), `README.md`, `docs/plan.md` P1. Создано: `tests/test_connect_timings.cpp`. `DropRecord`/экспорты — без изменений.
- **Верификация.** `make` OK; `make test` = host PASS (вкл. `test_connect_timings`) + 150 Python OK; `make verify-exports` OK.
- **Отклонения.** `steam_enter` хранит измеренную длительность стадии (сек), а не timestamp (точность + `null` при async); дополнительный guard `t_spawn>0`; pending только при `plugin_enabled()`.
- **Не проверено (вне CI).** Эмпирическая синхронность пайплайна (CTP-3); smoke QAS-CT3.
- **Дальше.** Стадия 9 = F6 (event-enrich). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 9 (event-enrich): детектор-поле, top-N, деградация, edict churn
- **Контекст.** Ур.3: детекторы `rehlds_security` тонут в `drop.reason`; flood-профиль — один `top_ip`; деградация сбора невидима; нет скорости эдиктов. Дизайн: `docs/design.md` §26 (rev 10); план: `docs/plan.md` «Стадия 9».
- **Гейт-трейл.** Дизайн → oracle PASS-WITH-REVISIONS (EN-1..EN-7 folded; EN-7 решён: детектор — поле в `drop`, `top_ips` аддитивно, hard-failure=console). План → momus OK (EV-1..EV-5 folded).
- **Решение.** `DropRecord.detector[32]` (best-effort substring, `""` при промахе, не влияет на drop; тип `filter` **не** введён); `PacketStats.top_ips[3]` (+ сохранён `top_ip`); `ServerSample.edicts_churn` (`int32`+present, reset на `SV_Spawn_f`); новые типы `degraded`/`retention` (pull-модель из `LogWriter` на 1 Гц, только при `log->enabled()`); hard write-failure/кап — console (`pfnServerPrint`)/`hldsmon_status` (тип `write_error` **не** введён).
- **Артефакты.** Изменено: `src/metrics.h` (detector/top_ips/edicts_churn+present; `DegradedRecord`/`RetentionRecord`; host-testable `detector_name`/`classify_detector`/`select_top_ips`; `IpEntry` перенесён), `src/plugin.h` (baseline-поля), `src/hooks.cpp` (матч детектора; reset churn в `SV_Spawn_f`), `src/sampler.cpp` (top-3; churn Δ; `emit_log_health`), `src/log_writer.{h,cpp}` (`last_prune_count`), `src/serialization.{h,cpp}` (`kv_i64_or_null`, `top_ips`, `write_degraded_json`/`write_retention_json`), `src/events.{h,cpp}`, `src/bootstrap.cpp` (re-baseline), `src/control.cpp` (`pruned: N`), `tools/analyzer/hldsmon.py` (`KNOWN_TYPES += degraded/retention`), `README.md`, `tools/analyzer/README.md`, `docs/plan.md` P1. Создано: `tests/test_event_enrich.cpp`, fixtures `degraded.jsonl`/`retention.jsonl`.
- **Верификация.** `make` OK (0 предупреждений); `make test` = 23 host-бинаря PASS (вкл. `test_event_enrich`) + **153 Python OK**; `make verify-exports` OK.
- **Отклонения.** `IpEntry` перенесён в `metrics.h` (host-testability); `prev_log_enabled`/`prev_edicts`; re-baseline в `release_state`; `last_prune_count` кумулятивный; детектор — 4 подстроки (`move/string commands flooding`, `dlfile flooding`, `speedhack`); `top_ips` всегда 3 слота.
- **Дальше.** Стадия 10 = F7 (analyzer-ui). Коммит — только по явной просьбе.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 10 (analyzer-ui): host-overlay, incident bundle, config-diff
- **Контекст.** Ур.4: данные есть, но триаж ручной. Дизайн: `docs/design.md` §27 (rev 11); план: `docs/plan.md` «Стадия 10». **Только `tools/analyzer`** (плагин/схема не меняются).
- **Гейт-трейл.** Дизайн → oracle PASS-WITH-REVISIONS (AU-1..AU-9 folded; Q1 решён: JSON-конверт). План → momus OK (F1..F4 folded).
- **Решение.** `config_at`/`config_diff` (`{changed,added,removed,baseline}`; `set`/`cleared`; явный «нет baseline»); карта `field→caveat-source` рядом с `HOST_CAVEAT_BITS`; `incident_bundle` → JSON-конверт `{bundle_version,drop,window,config,host,records}` + `read_bundle` (ре-инжестится), дедуп `drop` vs `records`, граница размера; `/api/analyze` +`host_series`/`host_caveats`/`config`; `/api/bundle`; UI — host-overlay на «Все датасеты» (3 состояния, `null`≠0), config-diff панель, кнопка bundle.
- **Артефакты.** Изменено: `tools/analyzer/hldsmon.py`, `webui.py`, `webui/index.html`, `tests/test_hldsmon.py`, `tests/test_webui.py`, `README.md`, `tools/analyzer/README.md`. Плагин/`src/`/схема/`KNOWN_TYPES` — **не тронуты**.
- **Верификация.** `make test` = **209 Python OK** (было 153; +56); `make` OK; `make verify-exports` OK; UI-функции прогнаны в Node с DOM-шимом (null→разрыв, caveat→маркер, round-trip localStorage).
- **Отклонения.** Доп. ключ `host_caveats` (единый источник карты); дедуп `records` по умолчанию при заданном `drop`; `config`-снимки с `diff` к предыдущему; bundle скачивается для последнего drop периода; CLI-подкоманда bundle не добавлялась.
- **Не проверено (вне CI).** Визуальный рендер в реальном браузере.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Стадия 11 (toolchain): C++23 + единый oracle-тулчейн в CI

- **Контекст.** Запрос владельца: мигрировать на последний стандарт C++ и использовать строго oracle-образ в CI (локально — не навязывать). Прежнее состояние: `-std=c++17`; CI-плагин собирался в `manylinux2014_i686` (devtoolset-10 = **GCC 10.2.1**), деплой-путь документировал oracle-образ (**GCC 16.2.0**) — две разные цепочки.
- **Исследование (эмпирическое).** Полный пайплайн в oracle-образе на `-std=c++23`: `make`, `verify-exports`, `test-modnet-harness`, `make test` — зелёные; max GLIBC = 2.17, без динамического libstdc++/libgcc. GCC 10.2.1 в `manylinux2014` отвергает `-std=c++23` (умеет лишь до c++20) — единственный блокер C++23. Отдельная проба с тяжёлыми библиотечными фичами C++23 (`<filesystem>`, `std::jthread`, `<format>`, chrono-tz, `thread_local`-dtor) на статическом libstdc++ GCC 16/glibc 2.17 всё равно даёт max GLIBC 2.17.
- **Решение.** Стандарт → **C++23**; единственный тулчейн — oracle-образ, пинован по digest `sha256:4e3334c1…`; CI (`tests` + `plugin`) и документированная локальная сборка используют его. Код не менялся (фактически C++11-уровня; новых фич не вводилось).
- **Артефакты.** Изменено: `Makefile` (`-std=c++17`→`c++23` в `BASE_FLAGS`/`HARNESS_FLAGS`; `CC` — origin-guard `:= gcc`, т.к. в oracle-образе нет `cc`, а make предопределяет `CC=cc`, перекрывая `?=`), `.github/workflows/ci.yml` (`tests`+`plugin` в oracle-образе по digest; верхнеуровневый `env.BUILD_IMAGE`), `README.md` (раздел «Сборка» и CI-описание), `docs/design.md` §3/§13, `AGENTS.md`. Код `src/`/схема/`KNOWN_TYPES`/экспорты — **не тронуты**.
- **Верификация (эта сессия, репо `/mnt/c`, oracle-образ).** `make` OK (32-бит, C++23, `-Wall -Wextra -Werror`); max GLIBC `2.17`; без динамического libstdc++/libgcc; `make verify-exports` OK; `make test-modnet-harness` PASS (после фикса `CC`); `make test` = все host-тесты PASS + **209 Python OK**. Локально (g++ 11.4) host-тесты PASS на c++17/c++20/c++2b; `-std=c++23` принимается и даёт `__cplusplus=202100L`.
- **Отклонения.** `manylinux2014_i686` полностью выведен из CI ради одного тулчейна. `CC`-фикс сделан origin-guard (сохраняет override из env/командной строки), а не жёстким `CC=gcc`.
- **Риски/нити.** CI зависит от внешнего Docker Hub-образа → пин по digest (желательно зеркало в ghcr.io); образ 3.41 ГБ (дороже pull); glibc ≤ 2.17 держится лишь пока не используются фичи с новыми glibc-зависимостями — CI-проверку сохранять; `_GLIBCXX_USE_CXX11_ABI=0` не менять.
- **Коммит.** Не выполнен: по профилю требуется явная просьба владельца.

## Итог по всем стадиям (Ур.0–4)
Все 7 фич прошли полный пайплайн (design → design-review → owner-форки → plan → plan-review → реализация → верификация → журнал): **Стадии 4–10**. Стадии закоммичены в `main` (3 коммита), версия поднята **0.3.0 → 0.4.0**, выпущен релиз **`v0.4.0`** (по явной просьбе владельца).

## Открытые темы
- **O1 (частота/объём логов).** `hldsmon_client_hz`/`hldsmon_disk_hz` (default 1) — уточнить по фактическому объёму (~300–600 МБ/сутки при 32 клиентах) и стоимости FTP.
- **O2 (PII/retention).** IP/SteamID/userinfo хранятся без авто-удаления; владелец чистит файлы.
- **O3 (целевые сборки ReHLDS). РЕШЕНО:** целевой сервер — ReHLDS API **3.14**; минимум понижен до 3.14 (хукчейн-vtable совпадает с 3.15). Требование хостинга: glibc **≤ 2.17** — сборка через образ `hun1er/oracle-7-cxx-build-env-gnu` со статическим линком libgcc/libstdc++.
- **Интеграция (Фаза 6).** Требует реального ReHLDS-сервера: QAS-1 (overhead), QAS-3 (`kill -9`), QAS-6 (нет API), smoke с реальным дропом. Доступ к тестовому серверу предложен владельцем.
- **NT-1 (ре-вендоринг ReHLDS-интерфейсов).** Вендоренный `rehlds_interfaces.h` устарел относительно заявленного API 3.15 (нет `GetLatency()` и др. с 3.10) — отдельная стадия: сверить/обновить пару `rehlds_api.h`+`rehlds_interfaces.h`, зафиксировать коммит в `include/VENDORED.md`.
- **NT-2 (UI `config`).** Панель/маркер конфигурации в webui не реализована (Стадия 5); анализатор и API уже учитывают тип `config` — визуализация при следующем UI-проходе.
- **Фаза 2 (Windows)** и **Фаза 3 (внешний агент с egress-push)** — отдельными стадиями.

## Следующие шаги
1. Установить плагин на тестовый сервер (`addons/metamod/dlls/`, строка в `plugins.ini`), проверить `meta list`, `hldsmon_status`, появление `hldsmon-YYYY-MM-DD.jsonl`.
2. Прогнать сценарии QAS-1/3/6, зафиксировать результат здесь.
3. Скорректировать частоты (O1) по факту.
4. По явной команде владельца — один связный коммит стадии.

## Коммит
Выполнен по явной просьбе владельца. Стадии 4–10 — 3 коммита (`cd0db19`, `2a6e7f0`, `7735eb9`); релизный коммит — bump `0.4.0` (`src/bootstrap.cpp` + запись журнала). Тег **`v0.4.0`** → GitHub Actions: host/analyzer-тесты, 32-битный `.so` (glibc ≤ 2.17), webui-бинарники (linux/windows) и публикация GitHub Release.
