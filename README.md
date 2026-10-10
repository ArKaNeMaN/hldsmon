# HldsMonitoring

Native Metamod-плагин для ReHLDS (Half-Life Dedicated Server, GoldSrc), собирающий телеметрию для диагностики отключений игроков, плюс офлайн-инструменты анализа.

- **v1:** Linux, 32-бит, строго пассивный (никогда не дропает пакеты). Windows и внешний агент — отдельные фазы.
- **Документы:** [`docs/design.md`](docs/design.md), [`docs/plan.md`](docs/plan.md), [`docs/journal.md`](docs/journal.md), профиль проекта — [`AGENTS.md`](AGENTS.md).

## Сборка

```bash
make            # -> HldsMonitoring_mm_i386.so (32-bit, -Wall -Wextra -Werror)
make verify-exports   # определённые символы ровно = exports.whitelist
make test       # C++ host-тесты ядра + Python-тесты анализатора
make test-modnet-harness   # 32-битный синтетический .so-стенд атрибуции modnet
make clean
```

Требуется `gcc/g++` с multilib (`gcc-multilib g++-multilib libc6-dev-i386`) и `python3`.

### Совместимость со старым хостингом (важно)
Целевой хостинг имеет старую glibc (≤ 2.17). Плагин **статически** линкует `libgcc`/`libstdc++` и не должен требовать glibc выше **2.17**. Собирайте совместимый бинарник образом `hun1er/oracle-7-cxx-build-env-gnu`:

```bash
docker run --rm -v "$PWD":/src -w /src hun1er/oracle-7-cxx-build-env-gnu make
```

Проверка (обе команды обязательны):
```bash
objdump -T HldsMonitoring_mm_i386.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1   # <= GLIBC_2.17
readelf -d HldsMonitoring_mm_i386.so | grep NEEDED                                  # без libstdc++/libgcc
```

Готовый разворачиваемый бинарник (GLIBC_2.17): `dist/HldsMonitoring_mm_i386.so`. Локальный `make` в новом дистрибутиве даёт `.so`, требующий более свежую glibc (годится только для разработки/тестов) — для деплоя используйте oracle-образ выше.

## Установка

1. Скопировать `HldsMonitoring_mm_i386.so` в `<mod>/addons/metamod/dlls/`.
2. Добавить строку в `addons/metamod/plugins.ini`:
   ```
   linux addons/metamod/dlls/HldsMonitoring_mm_i386.so HldsMonitoring
   ```
3. Проверить: `meta list` (плагин загружен), затем в консоли `hldsmon_status`.
4. Логи: `<mod>/addons/metamod/hldsmon/logs/hldsmon-YYYY-MM-DD.jsonl` (UTC), суточная ротация.

## Что собирается

- **Серверные метрики (1 Гц):** кадровое время (min/avg/max), hitch-гистограмма, CPU%/RSS/page-faults, игроки (всего/реальные/прокси/fake), in/out KB/s, edicts, карта, uptime, FPS.
- **Per-client (staggered):** ping, loss/Δloss, choke/Δchoke, delta_seq, sequence-номера, заполнение reliable-канала, rate/cleartime, `cl_updaterate`/`cl_cmdrate`/`rate` из userinfo, state-битмаска, flow in/out, флаги сообщения, **простой (`idle_ms`)**. **Идентичность:** `name`, `authid` (Steam2), `steamid64`, `ip`+`port`, `userid`, `slot` (1..maxclients), `conn_s`, `fake`/`proxy` — эмитятся в `client` и `drop`.
- **События:** drop (причина+`crash`+срез client/netchan+окно+идентичность), reject (стадия валидатора), connect, challenge-флуд, netcmd-гистограмма, mass-drop, packet-статистика, hitch, mapchange, voice, **shutdown** (graceful: `quit`/SIGTERM/SIGINT через `NEW_DLL_FUNCTIONS::pfnGameShutdown`), **crash** (аварийный сигнал: best-effort JSONL-строка из signal-обработчика + маркер).
- **`modnet` (1 Гц, per target `.so`):** атрибуция сетевого трафика чужих in-process модулей (по умолчанию `easy_http_amxx_i386.so`/`sockets_amxx_i386.so`): байты `send/recv/sendto/recvfrom/sendmsg/recvmsg`, соединения `connect`/`accept` (`conns`/`conn_fail`/`connect_attempts`), счётчик DNS `getaddrinfo`; одна запись на каждый активный целевой объект (интерпозиция GOT/PLT, вызывающий модуль по адресу возврата).

Схема JSONL — `docs/plan.md`, Приложение P1.

## Управление (RCON/консоль)

| Команда | Назначение |
|---|---|
| `hldsmon_status` | статус: путь лога, объём, счётчики, размеры колец, проба layout |
| `hldsmon_flush_log` | принудительный сброс буфера |

Cvar'ы: `hldsmon_enable`, `hldsmon_client_hz`, `hldsmon_disk_hz`, `hldsmon_flush_sec`, `hldsmon_client_ring`, `hldsmon_server_ring`, `hldsmon_drop_window`, `hldsmon_dir`, `hldsmon_max_mb`, `hldsmon_ip_table`, `hldsmon_retention_days`.

**Retention (автоудаление):** `hldsmon_retention_days` (default `3`) задаёт максимум одновременно существующих дневных файлов, включая текущий. При открытии лога и на суточной ротации старые `hldsmon-YYYY-MM-DD.jsonl` (дата старше `сегодня − (N−1)`) удаляются; `N=1` оставляет только текущий файл, `N<=0` отключает удаление (keep-all). Значение перечитывается каждый server-tick, поэтому изменение вступает в силу на ближайшей ротации. Удаляются только файлы точного шаблона в `hldsmon_dir`; маркер краша и прочие файлы не трогаются.

Cvar'ы атрибуции `modnet`: `hldsmon_modnet` (default 0 — мастер-выключатель), `hldsmon_modnet_targets` (default `""` — пусто ⇒ встроенная пара `easy_http_amxx_i386.so`/`sockets_amxx_i386.so`), `hldsmon_modnet_rescan_sec` (default 5 — период rescan; плюс rescan по `SV_Spawn_f`).

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

Только stdlib; парсер устойчив к обрезанному хвосту файла. Эвристики аномалий **исключают** окна вокруг `mapchange`, `shutdown`/`crash` и стартового «прогрева» (`--no-exclude` отключает); `summary`/`digest` показывают `log_end` (graceful/crash/unknown) и число исключённого. Библиотечный API: `find_anomalies_detail`, `client_series`, `client_identities`, `exclusion_windows`, `log_end_state`.

## Веб-интерфейс анализа

Локальный офлайн-сервер с дашбордом и JSON-API (`GET /api/health`, `POST /api/analyze`, `POST /api/export`). Только stdlib, слушает `127.0.0.1` по умолчанию. Запуск:

- **WSL/Linux:** `make web`
- **Windows:** `web.bat` (или `python tools\analyzer\webui.py --open`)

Браузер открывается **автоматически**; флаг `--no-open` отключает авто-открытие (тогда откройте напечатанный URL вручную). Порт по умолчанию — `8765` (`--host`/`--port` переопределяют).

Страница показывает:

- сводку по логу (период, число записей, разбивка по типам, игроки, top drop-причины) и бейдж «завершение» (`log_end`);
- **выбор периода по таймлайну** — перетаскиваемые границы по мини-шкале всего лога + пресеты «Весь лог / 5 мин / 30 мин / 1 ч» и режим **«Слежение»** (правая граница без ограничения — новые пакеты попадают в выборку, ус едет за максимумом); ручной ввод от/до тоже есть; фильтрует графики, таблицы и дропы;
- графики серверных метрик с **полосами событий** (mapchange/shutdown/crash/warmup) и итогом «аномалии: N · исключено событиями: M»;
- **общий график «Все датасеты»** — вкл/выкл любых серверных метрик (нормализация) + **оверлей выбранного игрока: его метрики** (ping/loss/choke/fill/idle/Δseq/Δloss) **и трафик** in/out — **по умолчанию выключены**, включаются по одной;
- **per-client графики с выбором клиента** (ping/loss/choke/rel_fill/idle/dloss/delta_seq/трафик);
- таблицу клиентов по **сессиям** (`userid` в пределах запуска; связь между запусками по SteamID) с идентичностью (слот, ник, SteamID, IP:порт, длительность, флаги);
- список drop-событий (время, id, причина, категория, crash, источник, идентичность, простой, «был в игре») с кнопкой **«→ клиент»** (фильтр по причине сохраняется, в т.ч. при пустом за период наборе);
- **пагинацию** таблиц (25/50/100/все) и **сворачивание панелей** (клик по заголовку; «Свернуть всё/Развернуть всё»; свёрнутость сохраняется в `localStorage`);
- загрузку JSONL-файлов (не изменяют исходники) и экспорт в CSV.

Дополнительно:
- **«Все датасеты» выводится первым** среди панелей; к каждой метрике есть примечание (подсказка `title` + раздел «Справка»).
- **Единый (глобальный) выбор клиента** — один селектор, **перенесён в sticky-блок «Период»** (виден при скролле), синхронизирован между общим графиком и per-client (выбор сессии/игрока в одном месте отражается в другом).
- **Панель «События»** — «зависания клиента» (долгий непрерывный рост `idle_ms`; из `focus_points`) со кнопкой **«фокус»**: скролл к общему графику, выбор игрока и период ±10 с вокруг события (механизм расширяемый по `kind`).
- **Настройки датасетов** (набор серверных метрик, нормализация, полосы событий и **выбор клиентских метрик**) сохраняются в `localStorage`; клиентские метрики — **единый набор** (не зависит от выбранного игрока), фокус на событие его не меняет.

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

GitHub Actions (`.github/workflows/ci.yml`):
- **tests** — `make test` (host C++ ядро + Python-тесты анализатора);
- **plugin** — сборка 32-бит `.so` в образе `manylinux2014_i686` (CentOS 7, glibc 2.17) + проверки `verify-exports`, `GLIBC ≤ 2.17` и отсутствия динамических `libstdc++`/`libgcc`; артефакт `HldsMonitoring_mm_i386.so`;
- **analyzer-binaries** — одиночный бинарник веб-UI (PyInstaller) для Linux и Windows (`hldsmon-webui` / `hldsmon-webui.exe`);
- **release** — на тег `v*` прикрепляет плагин и бинарники к GitHub Release.

Готовые бинарники — в разделе **Releases** (или в артефактах прогона Actions).

## Лицензия

**GPL-3.0** — см. [`LICENSE`](LICENSE). Проект содержит и линкует вендоренные GPL-компоненты ReAPI/ReHLDS/Metamod-R (провенанс — [`include/VENDORED.md`](include/VENDORED.md)), поэтому распространяется под GPL-3.0.
