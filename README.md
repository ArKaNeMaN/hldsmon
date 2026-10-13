# HldsMonitoring

Native Metamod-плагин для ReHLDS (Half-Life Dedicated Server, GoldSrc), собирающий телеметрию для диагностики отключений игроков, плюс офлайн-инструменты анализа.

- **v1:** Linux, 32-бит, строго пассивный (никогда не дропает пакеты). Windows и внешний агент — отдельные фазы.
- **Документы:** [`docs/design.md`](docs/design.md), [`docs/plan.md`](docs/plan.md), [`docs/journal.md`](docs/journal.md), профиль проекта — [`AGENTS.md`](AGENTS.md).

## Сборка

Стандарт — **C++23**. Канонический тулчейн (и для CI, и для деплой-артефакта) — образ
`hun1er/oracle-7-cxx-build-env-gnu` (**GCC 16.2.0**, glibc **2.17**), пинованный по digest:

```bash
IMG='hun1er/oracle-7-cxx-build-env-gnu@sha256:4e3334c189a05c51a40050b14e7db05dadd42af283c9d6e4bf9bc0b6c6ca2191'

# сборка плагина + проверки (ровно то, что делает CI):
docker run --rm -v "$PWD":/src -w /src "$IMG" \
  bash -lc 'make clean && make && make verify-exports && make test-modnet-harness'

# host-тесты ядра + Python-тесты анализатора:
docker run --rm -v "$PWD":/src -w /src "$IMG" bash -lc 'make test'
```

### Совместимость со старым хостингом (важно)
Целевой хостинг имеет старую glibc (≤ 2.17). Плагин **статически** линкует `libgcc`/`libstdc++` и не должен требовать glibc выше **2.17**. Собирайте деплой-бинарник указанным выше oracle-образом (GCC 16 держит glibc 2.17 — проверено).

Проверка (обе команды обязательны):
```bash
objdump -T HldsMonitoring_mm_i386.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1   # <= GLIBC_2.17
readelf -d HldsMonitoring_mm_i386.so | grep NEEDED                                  # без libstdc++/libgcc
```

Готовый разворачиваемый бинарник (GLIBC_2.17): `dist/HldsMonitoring_mm_i386.so`.
Локальная разработка: в дистрибутиве с GCC ≥ 11 и multilib (`gcc-multilib g++-multilib libc6-dev-i386`) обычный `make`/`make test` работает, но локальный `.so` требует более свежую glibc и годится только для разработки/тестов — для деплоя используйте oracle-образ выше.


## Установка

1. Скопировать `HldsMonitoring_mm_i386.so` в `<mod>/addons/metamod/dlls/`.
2. Добавить строку в `addons/metamod/plugins.ini`:
   ```
   linux addons/metamod/dlls/HldsMonitoring_mm_i386.so HldsMonitoring
   ```
3. Проверить: `meta list` (плагин загружен), затем в консоли `hldsmon_status`.
4. Логи: `<mod>/addons/hldsmon/logs/hldsmon-YYYY-MM-DD.jsonl` (UTC), суточная ротация.

## Что собирается

- **Серверные метрики (1 Гц):** кадровое время (min/avg/max), hitch-гистограмма, CPU%/RSS/page-faults, игроки (всего/реальные/прокси/fake), in/out KB/s, edicts + `edicts_churn` (знаковый Δ за интервал, `null` на первом интервале/после смены карты), карта, uptime, FPS.
- **Per-client (staggered):** ping, loss/Δloss, choke/Δchoke, delta_seq, sequence-номера, заполнение reliable-канала, rate/cleartime, `cl_updaterate`/`cl_cmdrate`/`rate` из userinfo, state-битмаска, flow in/out, флаги сообщения, **простой (`idle_ms`)**, **jitter межпакетных интервалов (`jit_min_ms`/`jit_avg_ms`/`jit_max_ms`, мс за интервал; `0` — нет данных/первый пакет)**. **Идентичность:** `name`, `authid` (Steam2), `steamid64`, `ip`+`port`, `userid`, `slot` (1..maxclients), `conn_s`, `fake`/`proxy` — эмитятся в `client` и `drop`.
- **События:** drop (причина+`crash`+`detector` — best-effort детектор `rehlds_security` (`movecmd`/`stringcmd`/`dlfile`/`speedhack`, промах — `""`)+срез client/netchan+окно+идентичность), reject (стадия валидатора + тайминги `stage_ms`/`total_ms`), connect (+`auth_ms`/`steam_ms`), challenge-флуд, netcmd-гистограмма, mass-drop, packet-статистика (`top_ip` + `top_ips` top-3), hitch, mapchange (+`load_ms` до готовности мира), voice, **shutdown** (graceful: `quit`/SIGTERM/SIGINT через `NEW_DLL_FUNCTIONS::pfnGameShutdown`), **crash** (аварийный сигнал: best-effort JSONL-строка из signal-обработчика + маркер), **degraded**/**retention** (деградация сбора: Δ ошибок записи / Δ удалённых retention-файлов; pull из 1 Гц sampler, только пока запись включена; hard-сбой/кап — console + `hldsmon_status`). Тайминги коннекта/карты — `uint32` мс, **nullable**: нет метки/стадии или невалидная дельта (`GetRealTime()` guard) ⇒ `null`; `mapchange` эмитится отложенно (первый 1 Гц server-tick с готовым миром, окно 30 тиков).
- **Контекст конфигурации (`config`):** boot-снимок и запись при изменении отслеживаемых cvar — `sys_ticrate`, `host_framerate`, `sv_timeout`, `sv_maxrate`/`sv_minrate`, `sv_maxupdaterate`/`sv_minupdaterate`, `sv_maxcmdrate`/`sv_mincmdrate` — плюс версии ReHLDS, `map`, `maxclients`; не зарегистрированный на сервере cvar эмитится как `null`. Каждая drop-запись несёт вложенный `drop.config` — rate/timeout-снимок на момент дропа (авторитет; `drop.netchan.sv_timeout` сохранён для совместимости схемы). Поллинг 1 Гц: изменение с возвратом быстрее ~1 с в лог не попадает, несколько изменений между тиками схлопываются в одну запись.
- **`modnet` (1 Гц, per target `.so`):** атрибуция сетевого трафика чужих in-process модулей (по умолчанию `easy_http_amxx_i386.so`/`sockets_amxx_i386.so`): байты `send/recv/sendto/recvfrom/sendmsg/recvmsg`, соединения `connect`/`accept` (`conns`/`conn_fail`/`connect_attempts`), счётчик DNS `getaddrinfo`; одна запись на каждый активный целевой объект (интерпозиция GOT/PLT, вызывающий модуль по адресу возврата).
- **`host` (1 Гц, при `hldsmon_host 1`; по умолчанию OFF):** best-effort хост/ядро-телеметрия (только чтение `/proc` + `statvfs`): переполнение UDP-приёмного буфера ядра (`udp_rcvbuf_errors`/`udp_in_errors`/`udp_in_csum_errors`), NIC-дропы `rx_dropped`/`rx_missed`/`rx_overruns`, CPU `steal_pct` (%), `io_read_bytes`/`io_write_bytes`, свободное место `disk_bavail_mb` (в каталоге логов). Все счётчики — **Δ за интервал** (первый сэмпл — `null`); недоступный источник — `null` (не ноль-как-здоровье). Запись несёт маски `present_mask` (по полям) и `caveat_mask` (по источникам): **caveated ≠ null** — значение присутствует, но не атрибутируется серверу (общий netns, драйверная зависимость NIC-счётчиков, lxcfs-`steal`, thin/quota-диск, обрезанное чтение). Детекция окружения (выделенный хост vs контейнер) — при старте; неопределённость помечается caveat, движок не трогается. Парсинг — по именам полей (устойчив к ядрам без `InCsumErrors`/`MemErrors`).

Схема JSONL — `docs/plan.md`, Приложение P1.

## Управление (RCON/консоль)

| Команда | Назначение |
|---|---|
| `hldsmon_status` | статус: путь лога, объём, счётчики, размеры колец, проба layout |
| `hldsmon_flush_log` | принудительный сброс буфера |

### Cvar'ы

Все cvar'ы регистрируются с флагом `FCVAR_ARCHIVE` (сохраняются движком). Значения читаются «на лету» там, где это отмечено.

| Cvar | Default | Диапазон / примечание | Назначение |
|---|---|---|---|
| `hldsmon_enable` | `1` | `0`/`1` (вкл при `>0.5`) | Мастер-выключатель сбора. При `0` серверные и клиентские замеры и запись в лог прекращаются; переключатель `modnet` синхронизируется независимо. |
| `hldsmon_client_hz` | `1` | `>0` Гц; `<=0` — выкл | Частота per-client замеров (staggered: интервал делится на число батчей `ceil(maxclients/8)`). |
| `hldsmon_disk_hz` | `1` | `>0` Гц; `<=0` — выкл | Частота записи per-client записей на диск (тоже staggered). |
| `hldsmon_flush_sec` | `1` | сек; `<=0` — без периодического flush | Период `fflush` лог-файла. |
| `hldsmon_client_ring` | `300` | clamp `1..65536` | Размер кольца последних замеров на клиента (в памяти). Задаёт глубину окна для Δloss/Δchoke/delta_seq и drop-среза. |
| `hldsmon_server_ring` | `3600` | clamp `1..262144` | Размер кольца серверных замеров (`3600` ≈ 1 ч при 1 Гц). |
| `hldsmon_drop_window` | `60` | clamp `0..64` (большее обрезается) | Сколько последних per-client замеров вкладывается в каждую drop-запись (поле `window`). `0` — без среза. |
| `hldsmon_dir` | `addons/hldsmon/logs` | путь | Каталог логов. Относительный путь разрешается от игровой директории (`pfnGetGameDir`), т.е. `<mod>/addons/hldsmon/logs`; путь с ведущим `/` — абсолютный. Недоступен ⇒ fallback `/tmp/hldsmon`. Создаётся рекурсивно, права `0700` (каталог) / `0600` (файл). |
| `hldsmon_max_mb` | `0` | МБ; `0` — без лимита (опциональный клапан) | Суточный лимит объёма текущего файла. По умолчанию `0` (unlimited) — запись не останавливается по размеру; при `>0` запись останавливается до следующей суточной ротации. Возможный disk-fill при выключенном клапане — осознанное решение владельца. |
| `hldsmon_ip_table` | `4096` | clamp `16..1048576`, округляется вверх до степени двойки | Размер хеш-таблицы уникальных IP (пакетная статистика). |
| `hldsmon_retention_days` | `3` | `N>=1`; `<=0` — keep-all | Максимум одновременно хранимых дневных файлов, включая текущий (`3` ⇒ сегодня + 2 предыдущих). Перечитывается каждый server-tick, применяется при ближайшем open/ротации. Удаляются только файлы точного шаблона `hldsmon-YYYY-MM-DD.jsonl` в `hldsmon_dir`; маркер краша и прочие файлы не трогаются. |
| `hldsmon_modnet` | `0` | `0`/`1` (вкл при `>0.5`) | Мастер-выключатель атрибуции трафика (`modnet`). |
| `hldsmon_modnet_targets` | `""` | пробел-разделённые basename `.so` | Целевые in-process модули. Пусто ⇒ встроенный список `easy_http_amxx_i386.so sockets_amxx_i386.so`. |
| `hldsmon_modnet_rescan_sec` | `5` | сек, минимум `1` | Период повторного сканирования GOT/PLT; дополнительно rescan инициируется по `SV_Spawn_f`. |
| `hldsmon_host` | `0` | `0`/`1` (вкл при `>0.5`) | Мастер-выключатель host/ядро-телеметрии (best-effort `/proc` + `statvfs`, 1 Гц). `0` ⇒ ни одного чтения и ни одной `host`-записи. Значения caveated (общий netns, драйверные NIC-счётчики) помечаются в `caveat_mask`, а не отбрасываются. |

## Анализ (офлайн)

Файлы забираются с сервера (например, по FTP) и обрабатываются:

```bash
python3 tools/analyzer/hldsmon.py summary  logs/hldsmon-2026-10-09.jsonl
python3 tools/analyzer/hldsmon.py drops    logs/hldsmon-2026-10-09.jsonl --reason overflow
python3 tools/analyzer/hldsmon.py window   logs/hldsmon-2026-10-09.jsonl --around <ts> --secs 30
python3 tools/analyzer/hldsmon.py clients  logs/hldsmon-2026-10-09.jsonl
python3 tools/analyzer/hldsmon.py anomalies logs/hldsmon-2026-10-09.jsonl
python3 tools/analyzer/hldsmon.py digest   logs/hldsmon-2026-10-09.jsonl
python3 tools/analyzer/hldsmon.py export-csv logs/hldsmon-2026-10-09.jsonl --type drop
```

Только stdlib; парсер устойчив к обрезанному хвосту файла. Эвристики аномалий **исключают** окна вокруг `mapchange`, `shutdown`/`crash` и стартового «прогрева» (`--no-exclude` отключает); `summary`/`digest` показывают `log_end` (graceful/crash/unknown) и число исключённого. Библиотечный API: `find_anomalies_detail`, `client_series`, `client_identities`, `loss_rate_series` (скользящее окно loss-rate по `loss`), `exclusion_windows`, `log_end_state`, `config_at`/`config_diff`/`config_series`, `incident_bundle` + `read_bundle` (JSON-конверт инцидента, ре-инжестится в `iter_records`).

## Веб-интерфейс анализа

Локальный офлайн-сервер с дашбордом и JSON-API (`GET /api/health`, `POST /api/analyze`, `POST /api/export`, `POST /api/bundle`). Только stdlib, слушает `127.0.0.1` по умолчанию. Запуск:

- **WSL/Linux:** `make web`
- **Windows:** `web.bat` (или `python tools\analyzer\webui.py --open`)

Браузер открывается **автоматически**; флаг `--no-open` отключает авто-открытие (тогда откройте напечатанный URL вручную). Порт по умолчанию — `8765` (`--host`/`--port` переопределяют).

Страница показывает:

- сводку по логу (период, число записей, разбивка по типам, игроки, top drop-причины) и бейдж «завершение» (`log_end`);
- **выбор периода по таймлайну** — перетаскиваемые границы по мини-шкале всего лога + пресеты «Весь лог / 5 мин / 30 мин / 1 ч» и режим **«Слежение»** (правая граница без ограничения — новые пакеты попадают в выборку, ус едет за максимумом); ручной ввод от/до тоже есть; фильтрует графики, таблицы и дропы;
- графики серверных метрик с **полосами событий** (mapchange/shutdown/crash/warmup) и итогом «аномалии: N · исключено событиями: M»;
- **общий график «Все датасеты»** — вкл/выкл любых серверных метрик (нормализация) + **оверлей выбранного игрока: его метрики** (ping/loss/choke/fill/idle/Δseq/Δloss) **и трафик** in/out — **по умолчанию выключены**, включаются по одной; ряды **`host`** (при наличии `host`-записей) накладываются туда же: **три состояния точки** — `null` = разрыв (никогда не рисуется нулём), установленный бит `caveat` = кольцо-маркер («значение есть, но не атрибутируется серверу»), иначе обычная точка;
- **панель «Конфигурация»** — снимки `config` и diff к предыдущему: `изменено` (было→стало), `установлено`/`снято` (cvar появился/исчез, `null` — не «изменение»); у первого снимка — явная пометка «нет baseline»;
- **incident bundle** — кнопка `bundle` у панели drop скачивает JSON-конверт инцидента (окно ±N мин): `drop`, границы окна, `config`, `host`, предупреждения; конверт читается библиотечно (`hldsmon.read_bundle`) и ре-инжестится как JSONL;
- **per-client графики с выбором клиента** (ping/loss/choke/rel_fill/idle/dloss/delta_seq/трафик);
- таблицу клиентов по **сессиям** (`userid` в пределах запуска; связь между запусками по SteamID) с идентичностью (слот, ник, SteamID, IP:порт, длительность, флаги);
- список drop-событий (время, id, причина, категория, crash, источник, идентичность, простой, «был в игре») с кнопкой **«→ клиент»** (фильтр по причине сохраняется, в т.ч. при пустом за период наборе);
- **пагинацию** таблиц (25/50/100/все) и **сворачивание панелей** (клик по заголовку; «Свернуть всё/Развернуть всё»; свёрнутость сохраняется в `localStorage`);
- загрузку JSONL-файлов (не изменяют исходники) и экспорт в CSV.

Дополнительно:
- **«Все датасеты» выводится первым** среди панелей; к каждой метрике есть примечание (подсказка `title` + раздел «Справка»).
- **Единый (глобальный) выбор клиента** — один селектор, **перенесён в sticky-блок «Период»** (виден при скролле), синхронизирован между общим графиком и per-client (выбор сессии/игрока в одном месте отражается в другом).
- **Панель «События»** — «зависания клиента» (долгий непрерывный рост `idle_ms`; из `focus_points`) со кнопкой **«фокус»**: скролл к общему графику, выбор игрока и период ±10 с вокруг события (механизм расширяемый по `kind`).
- **Настройки датасетов** (набор серверных и host-метрик, нормализация, полосы событий и **выбор клиентских метрик**) сохраняются в `localStorage`; старые сохранения совместимы (host-ключи — аддитивные); клиентские метрики — **единый набор** (не зависит от выбранного игрока), фокус на событие его не меняет.

**Источник FTP (живой хвост):** панель «Источник: FTP» — хост/порт/логин/пароль/путь (токен `{date}` подставляет текущую UTC-дату). Приложение сначала забирает **только последние ~30 минут** лога (range-чтение хвоста по offset; прогресс-бар + фаза **«Разбор данных…»**), затем дочитывает **только новые данные** (авто-обновление). Кнопка **«← загрузить раньше»** догружает предыдущее окно (~30 мин) по нажатию, пока не дойдёт до начала файла. Креды хранятся только в памяти локального сервера, не пишутся на диск и не возвращаются в браузер; только stdlib (`ftplib`). Чекбокс «сохранять креды» (по умолчанию вкл.) кладёт хост/логин/пароль/путь в `localStorage` **браузера**, чтобы не вводить их заново (в открытом виде — инструмент локальный). Авто-обновление не мешает вводу: такт пропускается, пока открыт список/поле формы (фокус после перерисовки восстанавливается).

### Веб-интерфейс как один `.exe` (Windows, опционально)
Веб-сервер (`tools/analyzer/webui.py`) на чистой stdlib, поэтому собирается в один самодостаточный `.exe` через **PyInstaller** (страница `index.html` включается внутрь; путь к ней в коде frozen-aware). **Собирать нужно на Windows** — PyInstaller не кросс-компилирует с Linux/WSL.

```bat
tools\analyzer\build_webui_exe.bat
```
Скрипт поставит PyInstaller и соберёт `tools\analyzer\dist\hldsmon-webui.exe`. Запуск: двойной клик — **браузер откроется автоматически** (`--no-open` отключает авто-открытие). На Linux/macOS — `tools/analyzer/build_webui_exe.sh` (даст нативный бинарник, не `.exe`).

## Инварианты (см. `docs/plan.md`)

INV-1 пассивность (`callNext`+propagate, `PreprocessPacket` никогда не `false`); INV-2 hot-path без аллокаций; INV-3 единый server thread; INV-4 ABI-проба с fail-closed; INV-5 валидный JSONL; INV-6 Linux-only, без БД/агента.

## CI и релизы

GitHub Actions (`.github/workflows/ci.yml`) — оба C++-job'а идут в пинованном oracle-образе (**GCC 16, C++23**, glibc 2.17):
- **tests** — `make test` (host C++ ядро + Python-тесты анализатора) в oracle-образе;
- **plugin** — сборка 32-бит `.so` в oracle-образе + проверки `verify-exports`, `GLIBC ≤ 2.17` и отсутствия динамических `libstdc++`/`libgcc`; артефакт `HldsMonitoring_mm_i386.so`;
- **analyzer-binaries** — одиночный бинарник веб-UI (PyInstaller) для Linux и Windows (`hldsmon-webui` / `hldsmon-webui.exe`);
- **release** — на тег `v*` прикрепляет плагин и бинарники к GitHub Release.

Готовые бинарники — в разделе **Releases** (или в артефактах прогона Actions).

## Лицензия

**GPL-3.0** — см. [`LICENSE`](LICENSE). Проект содержит и линкует вендоренные GPL-компоненты ReAPI/ReHLDS/Metamod-R (провенанс — [`include/VENDORED.md`](include/VENDORED.md)), поэтому распространяется под GPL-3.0.
