# HldsMonitoring — дизайн модуля сбора телеметрии ReHLDS (rev 2)

- **Статус:** Reviewed — design review выполнен (`oracle`), замечания сведены в §18; решения владельца зафиксированы в §16.
- **Decision state:** архитектура выбрана (native Metamod collector, Linux-first, пассивный, файловое логирование с суточной ротацией).
- **Профиль проекта:** `AGENTS.md`. Разбор-источник: диалог (3 слоя диагностики) + исходники ReHLDS/Metamod-R/ReAPI.

---

## 1. Проблема

С ReHLDS-сервера выкидывает игроков. Нужно понять причину: сеть (loss/choke/маршрут), сам сервер (подвисание кадра, CPU, диск), клиент, внешний флуд/атака.

Требование: **непрерывно собирать максимум полезных параметров** с низкими и ограниченными накладными расходами, писать их в **файлы с суточной ротацией**, чтобы владелец мог **стянуть их по FTP** и скормить агенту-анализатору. Дополнительно поставляются **инструменты анализа/агрегации**, чтобы агент работал эффективнее.

Оговорка: GoldSrc-сервер видит только **симптомы** отключения (timeout, переполнение reliable-канала, отсутствие ответа). Точная первопричина на стороне клиента недоступна. Продукт — **доказательная база для триажа**, а не автоматический вердикт.

---

## 2. Цели / Не-цели

### Goals
- G1. Непрерывный сбор per-client сетевых/состояниевых и per-server метрик.
- G2. Снимок клиента **в момент дропа**: причина + `crash` + поля клиента/неткана + окно предшествующих метрик.
- G3. Данные переживают падение модуля/сервера (буферизованная дозапись; максимум ~1 с потерь).
- G4. Стягивание в проблемный период **без изменения фаервола** — файлы с суточной ротацией, забираются по FTP.
- G5. Ограниченная память и предсказуемые накладные расходы; сборщик не деградирует сервер.
- G6. Офлайн-инструменты анализа/агрегации + машинно-читаемый формат для агента.

### Non-goals
- N1. Активная защита/дроп/фильтрация пакетов (v1 строго пассивный).
- N2. Определение причины на стороне клиента.
- N3. Web-UI/realtime-дашборд.
- N4. Windows-паритет в v1 (Linux-first; Windows — фаза 2).
- N5. Автоматические баны/кики.

---

## 3. Проверенные технические факты (ground truth)

Источник: `rehlds/ReHLDS@550f2d6` (API v3.15), `rehlds/ReAPI@95089cb`, `rehlds/Metamod-R@56590c5`.

- **Получение API:** `CreateInterface("VREHLDS_HLDS_API_VERSION001")` из модуля движка (`Sys_GetModuleHandle("engine_i486.so")` + `Sys_GetFactory`). Отдельного экспорта `GetRehldsApi` **нет**; `GetEntityAPI` для ReHLDS-API не используется. Гейт: `GetMajorVersion()==3 && GetMinorVersion()>=14` (целевой сервер — 3.14; хукчейн-vtable 3.14 == 3.15).
- **Hookchains:** `registerHook(func, priority)` / `unregisterHook`; сигнатура `t_ret(*)(IHookChain<T...>*, T...)`; `callNext(...)` — передать дальше в цепочку/оригинал; `callOriginal(...)` — сразу оригинал. **Ловушка:** хук, не вызвавший `callNext`, завершает цепочку и **подавляет оригинал** — и для `void`, и для возвращающих значение. Пассивный хук обязан вызвать `callNext` и вернуть его результат.
- **Хуки и сигнатуры:** `SV_DropClient` → `void(IGameClient*, bool crash, const char* reason)` (строка причины уже отформатирована движком); `Steam_NotifyClientDisconnect` → `void(IGameClient*)`; `ClientConnected` → `void(IGameClient*)`; `SV_ConnectClient` → `void()`; `SV_Frame` → `void()`; `PreprocessPacket` → `bool(uint8*, unsigned int, const netadr_t&)`; валидаторы коннекта (return 0 = reject): `SV_CheckProtocol`, `SV_CheckIPRestrictions`, `SV_FinishCertificateCheck`, `SV_CheckKeyInfo`, `SV_CheckUserInfo`; `Steam_NotifyClientConnect` → `qboolean(IGameClient*, const void*, unsigned int)`; `SVC_GetChallenge_mod` → `void(char*, int)`; `HandleNetCommand` → `void(IGameClient*, uint8)`; `SV_Spawn_f` → `void()`.
  - ⚠️ Сигнатуры `SV_ConnectClient`, `SV_Frame`, `Steam_NotifyClientDisconnect` в собранном ground truth не подтверждены дословно — **сверить с `rehlds_api.h` при вендоринге** (см. §13, фаза 6.3).
- **Публичные геттеры `IGameClient`:** `GetId/IsActive/IsSpawned/IsConnected/IsFullyConnected/IsProxy/IsFakeClient`, `GetNetChan()→INetChan*`, `GetChokeCount()`, `GetLatency()`, `GetPacketLoss()`, `GetDeltaSequence()`, `GetConnectTime()`, `GetConnectionStartedTime()`, `GetLastVoiceTime()`, `GetLastCmd()`, `GetUserInfo()`, `GetEdict()`, `GetNetworkUserID()`, `GetFrames()`.
  - `INetChan` в публичном ABI даёт **только** `GetRemoteAdr()` и `GetMessageBuf()`. Полный `netchan_t*` — только под `REHLDS_SELF`.
- **`netchan_t`** (движковый `net.h`): `sock`, `remote_address`, `last_received`, `connect_time`, `rate`, `cleartime`, `incoming_sequence`, `incoming_acknowledged`, `incoming_reliable_acknowledged`, `incoming_reliable_sequence`, `outgoing_sequence`, `reliable_sequence`, `last_reliable_sequence`, `message` (sizebuf, флаг `SIZEBUF_OVERFLOWED`), `reliable_length`, `reliable_buf[MAX_MSGLEN]`, `waitlist[MAX_STREAMS]`, `reliable_fragment[MAX_STREAMS]`, `fragbufcount[MAX_STREAMS]`, `frag_startpos/frag_length[MAX_STREAMS]`, `incomingbufs[MAX_STREAMS]`, `incomingready[MAX_STREAMS]`, `tempbuffer/tempbuffersize`, `flow[MAX_FLOWS]` (`flow_t`: `stats[32]`, `kbytespersec`, `avgkbytespersec`).
  - **НЕТ** полей Quake-неткана: `fatal_error`, `cursize`, `drops`, `congestion`, `last_send`.
- **`client_t`** (движковый `server.h`, доступ через `IRehldsServerStatic::GetClient_t(id)`): `active/spawned/fully_connected/connected` (booleans, **нет `cs_*` enum**), `netchan`, `chokecount`, `delta_sequence`, `latency`, `packet_loss`, `connecttime`, `cmdtime`, `connection_started`, `frames`, `edict`, `userinfo`, `userid`, `network_userid`, `proxy`, `fakeclient`.
- **Семантика метрик:** `packet_loss`, `chokecount`, `GetPacketLoss()`, `GetChokeCount()` — вероятно кумулятивные/усреднённые. Хранить сырые + считать Δ/тренд; loss дополнительно выводить из прироста `incoming_sequence`/ack с учётом переполнения.
- **Уже в движке:** команда `stats` (CPU%, суммарные in/out KB/s, uptime, users, FPS, players); `sv_remoteaccess` player-list; детекторы флуда `rehlds_security` (movecmd/stringcmd/dlfile/speedhack) с готовыми строками причин.
- **Сборка:** HLDS 32-бит. Есть `gcc/g++ 11.4`, `clang`, `make`, `git`, `gh`; `cmake`/`ninja` нет (сборка Makefile). Требуется multilib (`gcc-multilib`).

---

## 4. Архитектура

Две части: **плагин-коллектор** (native Metamod) и **офлайн-инструменты анализа** (Python). Внешний агент — не в v1.

```
┌────────────────────────── ReHLDS (32-bit, один процесс) ──────────────────────────┐
│  engine ──hooks──▶ Metamod-R ──loads──▶ HldsMon plugin (.so)                        │
│                                            │                                        │
│   ┌────────────────────────────────────────┴──────────────────────────────────┐    │
│   │ Bootstrap → HookManager → Sampler ─┐                                        │    │
│   │                     EventRecorder ─┴─▶ LogWriter (JSONL, суточная ротация)  │    │
│   │                     ClientRings (in-memory, generation-tagged)             │    │
│   │                     Control (RCON: hldsmon_status / hldsmon_flush_log)      │    │
│   └─────────────────────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────┬─────────────────────────────────────────┘
                                             ▼  addons/metamod/hldsmon/logs/hldsmon-YYYY-MM-DD.jsonl
                                    (владелец забирает по FTP)
                                             ▼
                              tools/analyzer/*.py (парсинг, агрегация, отчёты)
```

### Компоненты

| Компонент | Ответственность |
|---|---|
| **Bootstrap** | Metamod-entrypoints (`GiveFnptrsToDll`, `Meta_Query`, `Meta_Attach/Detach`, `GetEntityAPI2`, `GetEngineFunctions`); получение ReHLDS-API + версия-гейт; детекция writable-директории; запуск пробы layout. |
| **HookManager** | регистрация/снятие хуков, политика pre/post, гарантированный `callNext`+возврат значения, приоритеты. |
| **Sampler** | per-frame кадровые метрики; staggered опрос клиентов в in-memory кольца. |
| **EventRecorder** | дропы, отклонённые коннекты, маркеры смены карты, гистограммы команд/пакетов, массовые дропы. |
| **ClientRings** | bounded per-client кольца сэмплов, тегированные generation (защита от переиспользования слота). |
| **LogWriter** | append JSONL, суточная ротация, буферизация + периодический `fflush`, права 0600, лимит размера. |
| **Control** | RCON-команды `hldsmon_status`, `hldsmon_flush_log`. |
| **engine_layout** | адаптер доступа к движковым структурам + runtime-проба валидности layout. |
| **platform_linux** | `getrusage`, монотонное время, каталог/права, signal-обработчик. |

### Потоки данных
1. Движок зовёт хук → HookManager → Sampler/EventRecorder (только server thread).
2. Сэмплы (1 Гц) пишутся в in-memory кольца и в JSONL через LogWriter.
3. Дроп → drop-record = причина + `crash` + срез клиента/неткана + окно последних N сэмплов из кольца + серверный срез + соседний контекст.
4. Владелец забирает дневной файл по FTP; `tools/analyzer` парсит и агрегирует.

---

## 5. Карта хуков

| Хук | Сигнатура | Когда | Зачем | Политика (все: `callNext` + propagate) |
|---|---|---|---|---|
| `SV_DropClient` | `void(IGameClient*, bool, const char*)` | дроп | снимок + причина + `crash` | снимок **ДО** `callNext`; после — отметка исхода |
| `Steam_NotifyClientDisconnect` | `void(IGameClient*)` | после дропа | закрытие событий, fallback-детект | `callNext` |
| `ClientConnected` | `void(IGameClient*)` | подключён | старт кольца (generation), IP/SteamID/userinfo | `callNext` |
| `SV_ConnectClient` | `void()` (сверить) | начало коннекта | маркер попытки | `callNext` |
| Валидаторы (`SV_CheckProtocol` и др.) | `int(...)` | коннект | зафиксировать стадию отказа | вернуть результат `callNext` |
| `Steam_NotifyClientConnect` | `qboolean(IGameClient*, const void*, unsigned int)` | Steam-валидация | отказ Steam/VAC | `callNext` + propagate |
| `SVC_GetChallenge_mod` | `void(char*, int)` | challenge | connect-флуд | `callNext` |
| `PreprocessPacket` | `bool(uint8*, unsigned int, const netadr_t&)` | каждый пакет | pps/размеры/флуд по IP | **`bool r = callNext(...); return r;`** — только счётчики, никогда не короткозамкнуть |
| `HandleNetCommand` | `void(IGameClient*, uint8)` | команда | гистограмма; детект `clc_disconnect` | `callNext` |
| `SV_Frame` | `void()` (сверить) | каждый кадр | кадровое время | `callNext` |
| `SV_Spawn_f` | `void()` | смена карты | маркер карты (исключать массовые дропы) | `callNext` |

**Правила HookManager (обязательны):**
1. **Любой** хук вызывает `callNext` и возвращает его результат; короткое замыкание запрещено (пассивность, N1).
2. `SV_DropClient` регистрируется **в голове цепочки** (наивысший приоритет), чтобы не пропустить дроп, если другой плагин короткозамкнёт цепочку (см. §18, M1). Конкретные числовые приоритеты фиксируются в `hook_manager.cpp` и документируются.
3. Fallback-детект дропов, которые не дошли до нашего `SV_DropClient` (подавлены ранее в цепочке): по `Steam_NotifyClientDisconnect` и по `HandleNetCommand` (`clc_disconnect`) — генерируется «синтетический» drop-record с пометкой `observed_via=fallback`.
4. При `Meta_Detach` — снять все хуки, закрыть LogWriter.

---

## 6. Каталог метрик

### 6.1 Серверные — агрегирование раз в секунду (НЕ каждый кадр в лог)
- frame interval (min/avg/max) за секунду, EMA.
- hitch-гистограмма: число кадров `>50ms`, `>100ms` за секунду.
- CPU% (Δ `getrusage(RUSAGE_SELF)` за интервал), minflt/majflt (Δ), RSS.
- `num_edicts`, карта, uptime, target tickrate, измеренный FPS.
- игроки (всего/реальные/прокси/fake), суммарные in/out KB/s (Σ `flow.avgkbytespersec`).
- `maxclients` берётся в рантайме (а не хардкод 32).

### 6.2 Per-client — staggered, `hldsmon_client_hz` (default 1 Гц; в лог `hldsmon_disk_hz`, default 1)
`ClientSample`: `t_ms`, `gen` (connect-time/SteamID), `ping_ms`, `packet_loss` (raw + Δ), `choke` (raw + Δ), `delta_seq`, `in_seq/in_ack/out_seq/rel_seq`, `rel_fill_pct` (`reliable_length` к `MAX_MSGLEN`), `rate`, `cleartime`, `state_bitmask` (4 booleans), `fragbuf_total`, `incomingready_mask`, `flow_out/in_kbps`, `flow_out/in_avg`, `msg_flags`.
- `reliable_buf` (до 4 КБ) читается **только при дропе**.
- `userinfo` копируется при изменении; **парсятся** `rate`, `cl_updaterate`, `cl_cmdrate`, `name` (ключевой драйвер choke/overflow).
- IP/SteamID читаются при коннекте.
- **Идентичность** (P1 `client`/`drop`, v2): `name`, `authid` (Steam2, копируется сразу — ротирующийся буфер), `steamid64` (из `GetNetworkUserID()->m_SteamID`), `ip`+`port` (из `GetNetChan()->GetRemoteAdr()`, IPv4), `userid`, `slot` (1..maxclients), `conn_s` (свой таймстамп коннекта), `fake`/`proxy`. Кэш по слоту заполняется на `ClientConnected`; читается живьём при сэмпле и на дропе (в `SV_DropClient` идентичность ещё цела).
- Опрос клиентов **по кругу** (несколько за тик).

### 6.3 Событийные
| Событие | Данные |
|---|---|
| drop | причина (raw + категория: timeout/drop/kicked/overflow/error), `crash`, срез `client_t`/`netchan_t`, окно N сэмплов, серверный срез, соседний контекст, `observed_via` |
| reject | IP, SteamID, стадия отказа (какой валидатор) |
| connect/challenge | попытки, challenge-флуд |
| net command | гистограмма типов; `clc_disconnect` |
| mass disconnect | N дропов за T с одной причиной → флаг; исключать смену карты |
| packet stats | глобальные pps, гистограмма размеров, bounded per-IP таблица |
| hitch | кадр >100 ms (с длительностью) |
| map change | маркер (`SV_Spawn_f`); задаёт окно-исключение |
| shutdown | graceful-останов (`quit`/`exit`, SIGTERM/SIGINT) через `NEW_DLL_FUNCTIONS::pfnGameShutdown` |
| crash | аварийный сигнал (SIGSEGV/ABRT/BUS/FPE): best-effort JSONL-строка из signal-обработчика (async-signal-safe `write`) + маркер-файл |
| voice | `GetLastVoiceTime` |

---

## 7. Хранилище: JSONL с суточной ротацией

- **Формат:** JSON Lines, одна запись — строка. Типы: `server`, `client`, `drop`, `reject`, `netcmd`, `massdrop`, `packet`, `hitch`, `mapchange`, `voice`, `shutdown`, `crash` (см. Приложение A).
- **Файл:** `logs/hldsmon-YYYY-MM-DD.jsonl` (UTC), директория `hldsmon_dir` (default `addons/metamod/hldsmon/logs/`; fallback `/tmp/hldsmon` с предупреждением).
- **Ротация:** при смене UTC-даты на 1 Гц-тике LogWriter закрывает старый и открывает новый файл.
- **Буферизация:** `FILE*` append + `fflush` раз в `hldsmon_flush_sec` (default 1 с). Потеря при краше — ≤ ~1 с. Данные в page cache ОС переживают краш процесса.
- **Права:** файл `0600`, директория `0700`. Лимит размера/сутки — предупреждение при превышении `hldsmon_max_mb`.
- **Объём (оценка):** ~1 server + до N client строк/с → при 32 клиентах ~35 строк/с ≈ 3M строк/сутки ≈ 300–600 МБ. Уменьшается через `hldsmon_client_hz`/`hldsmon_disk_hz`. Указывается в §12; уточняется владельцем.
- **In-memory кольца:** bounded, generation-tagged; служат для окна в drop-record и команды `hldsmon_status`. Размеры: per-client 300 сэмплов, серверное 3600. Память < ~4 МБ.

---

## 8. Стягивание и инструменты анализа

- **Стягивание:** владелец забирает дневные JSONL по FTP (или иным доступом к ФС). Никакого входящего порта, никаких изменений фаервола.
- **`tools/analyzer/` (Python, только stdlib):**
  - `hldsmon.py` — библиотека парсинга/агрегации + CLI:
    - `summary <file...>` — сводка: период, игроки, топ причин дропов, всплески лага.
    - `drops <file...>` — дропы с фильтрами `--reason/--client/--since/--until`, сортировка по времени.
    - `window <file> --around <ts> --secs N` — контекст вокруг события (все записи окна) для агента.
    - `clients <file>` — per-client агрегаты (loss/choke/ping тренды).
    - `anomalies <file>` — эвристики: кучные дропы, hitch-серии, рост reliable-fill, loss-всплески.
    - `digest <file>` — компактный машинно-читаемый дайджест (для скармливания агенту).
    - `export-csv <file>` — выгрузка таблиц.
  - Тесты на фикстурах (`tools/analyzer/tests/`).
- **Исключения аномалий (v2):** окна вокруг `mapchange`, `shutdown`/`crash` и стартового «прогрева» подавляют hitch/loss при подсчёте (`find_anomalies(exclude=True)`; CLI `--no-exclude`). `digest` отдаёт `log_end`, `exclusions`, `anomalies_excluded`.
- **Per-client ряды:** `client_series`/`client_identities` (для UI).
- **Веб-морда:** `tools/analyzer/webui.py` + `webui/index.html` — загрузка файла, выбор периода, per-client графики с выбором клиента, колонки идентичности, события-полосы, навигация «→ клиент» из дропов.
- **FTP-live:** ввод креды+пути, полная выборка, затем дочитывание только хвоста по offset (stdlib `ftplib`; креды только в памяти сервера).
- **Целостность:** парсер устойчив к обрезанной последней строке.

---

## 9. Крах-безопасность

- Append-only + периодический `fflush` (≤1 с потерь). Обрезанная последняя строка игнорируется анализатором.
- Linux signal-обработчик (`SIGSEGV/SIGABRT/...`) делает **минимум**: async-signal-safe `write()` в два пред-открытых fd — маркер и текущий лог-файл (`{"type":"crash",...}`) — и re-raise. Аллоцировать/`snprintf`/stdio в обработчике нельзя.
- Никаких тяжёлых операций в обработчике.

---

## 10. Конкурентность / reentrancy

- Все хуки — на **единственном server thread**; чтение движкового состояния без блокировок.
- LogWriter пишет из server thread (буферизованный `fprintf`), блокировок нет.
- In-memory кольца — фактически один писатель (server thread); читатель (`hldsmon_status`) — тот же поток → гонок нет.
- Хуки нерекурсивны, без побочных эффектов; не вызывать `SV_DropClient` изнутри хука.
- Вся память аллоцируется на `Meta_Attach`; в hot path — без `malloc/strdup/ED_Alloc/Con_Printf`.

---

## 11. Failure modes

| Граница | Отказ | Поведение |
|---|---|---|
| ReHLDS API нет / версия не та | stock/старый ReHLDS | плагин не грузится, сообщает причину |
| Проба layout не прошла | несовпадение раскладки | плагин не грузится (fail-closed), пишет диагностику |
| Директория не writable | права/read-only | fallback `/tmp/hldsmon` (0600) + предупреждение |
| Частичная регистрация хуков | ошибка на одном | отметить деградацию в консоль/файл; продолжать с доступными |
| Кольцо переполнено | высокая частота | перезапись старых (ring) |
| Чужой плагин короткозамкнул цепочку | пропуск `SV_DropClient` | fallback-детект (§5 п.3) |
| Краш процесса | падение | буфер сброшен ≤1 с назад; маркер |
| Диск полон / запись не удалась | нет места | счётчик ошибок, отключение записи с предупреждением, сбор в память продолжается |
| Атака/флуд | pps | per-IP таблица bounded, O(1) на пакет |

---

## 12. Конфигурация

| Имя | Тип | Назначение | Default |
|---|---|---|---|
| `hldsmon_enable` | cvar | вкл/выкл | 1 |
| `hldsmon_client_hz` | cvar | частота опроса клиента | 1 |
| `hldsmon_disk_hz` | cvar | частота записи client-строк | 1 |
| `hldsmon_flush_sec` | cvar | период `fflush` | 1 |
| `hldsmon_client_ring` | cvar | размер кольца клиента | 300 |
| `hldsmon_server_ring` | cvar | размер серверного кольца | 3600 |
| `hldsmon_drop_window` | cvar | сэмплов окна в drop-record | 60 |
| `hldsmon_dir` | cvar | директория логов | `addons/metamod/hldsmon/logs` |
| `hldsmon_max_mb` | cvar | лимит размера/сутки (предупреждение) | 512 |
| `hldsmon_ip_table` | cvar | размер per-IP таблицы | 4096 |
| `hldsmon_status` | cmd | статус колец/дропов/ошибок | — |
| `hldsmon_flush_log` | cmd | принудительный `fflush` | — |

---

## 13. Структура проекта и сборка

```
HldsMonitoring/
├── AGENTS.md
├── Makefile
├── version_script.lds
├── docs/{design.md,plan.md,journal.md}
├── include/                      # вендоренные заголовки (HLSDK/ReHLDS/Metamod-R)
│   ├── hlsdk/...
│   ├── metamod/...
│   └── rehlds/...
├── src/
│   ├── bootstrap.{h,cpp}
│   ├── hook_manager.{h,cpp}
│   ├── hooks.cpp
│   ├── sampler.{h,cpp}
│   ├── events.{h,cpp}
│   ├── metrics.h                 # POD-структуры сэмплов (host-testable)
│   ├── ring_buffer.h             # bounded ring (host-testable)
│   ├── delta.h                   # cumulative→Δ + wrap (host-testable)
│   ├── serialization.{h,cpp}     # JSONL-эмиссия (host-testable)
│   ├── log_writer.{h,cpp}
│   ├── engine_layout.{h,cpp}     # доступ к движковым структурам + проба
│   ├── control.{h,cpp}
│   └── platform_linux.cpp
├── tests/                        # хостовые C++ unit-тесты
│   ├── test_ring.cpp
│   ├── test_delta.cpp
│   ├── test_serialization.cpp
│   └── run_tests.sh
└── tools/analyzer/
    ├── hldsmon.py
    └── tests/
```

- **Сборка:** Makefile, `-m32 -fPIC -shared`, `-Wall -Wextra`, `-ldl`, `version_script.lds` (экспорт `GiveFnptrsToDll/Meta_*/GetEntityAPI2/GetNewDLLFunctions/GetEngineFunctions`).
- **Заголовки:** вендорить из `metamod-r/extra/example/include` (HLSDK+metamod+`rehlds_api.h`) + движковые `server.h`/`net.h`; при вендоринге **сверить сигнатуры хуков** и зафиксировать версию/коммит.
- **Хостовые тесты:** компилируются без движка (`make test`) — только `metrics.h/ring_buffer.h/delta.h/serialization.*`.

---

## 14. Альтернативы (decision matrix)

Критерии (вес): доступ к внутренностям (0.30), накладные расходы (0.20), устойчивость/крах (0.20), объём работ (0.15), лицензия/портируемость (0.15).

| Вариант | Внутр. | Overhead | Устойч. | Работы | Лиц. | Итог |
|---|---|---|---|---|---|---|
| **Native Metamod (выбран)** | полный | низкий-контролируемый | средняя | большие | GPL | выбран |
| AMXX/ReAPI (Pawn) | ограничен: нет `fragbuf/waitlist/flow`, нет raw `PreprocessPacket`; `get_netchan` даёт узкое подмножество | низкий, но маршалинг | средняя | средние | GPL | отклонён по G1 |
| Внешний агент/capture (tcpdump + sv_remoteaccess/A2S poll) | нет per-client/drop reason | минимальный для сервера | высокая | средние | своя | отклонён как единственный (нет G1/G2); допустим как дополнение в фазе 3 |
| Hybrid (native + agent) | полный | ниже | выше | большие | GPL | фаза 3 |

- Внешний capture (tcpdump) видит пакеты, но не связывает их с `client_t`, не даёт причину дропа и требует парсинга GoldSrc-протокола — не покрывает G1/G2.
- AMXX/ReAPI: `get_netchan` enum не отдаёт `packet_loss/latency/choke/flow`, поэтому G1 недостижим без native-расширений.
- Лицензия: вендоринг GPL-заголовков делает плагин GPL; для внутреннего инструмента приемлемо (§16).

---

## 15. Качественные сценарии (проверяемые)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-1 | При 32 клиентах добавленная нагрузка сборщика < 2% CPU и < 0.5 ms p99 на кадр | O(1) хуки, staggered, буферизованная запись | in-process счётчики времени + внешний замер (harness; вне хостовых тестов — задокументировать метод) |
| QAS-2 | На `SV_DropClient` drop-record содержит причину, `crash`, срез и окно ≥ `hldsmon_drop_window` за < 0.2 ms | снимок из памяти + сериализация одной строкой | unit-тест сборки записи (без движка) + бенч сериализатора |
| QAS-3 | При `kill -9` дневной файл содержит все строки до последнего `fflush` (≤ `hldsmon_flush_sec` назад), обрезанный хвост игнорируется | append+fflush; парсер tolerant | интеграционный/ручной тест (вне CI), unit-тест парсера |
| QAS-4 | Память плагина < 8 МБ при 32 клиентах; per-IP таблица ≤ `hldsmon_ip_table` | bounded rings/таблица | unit-тест размеров + ручной RSS |
| QAS-5 | Дневная ротация: при смене UTC-даты новый файл, старый закрыт | LogWriter | unit-тест ротации по подменённому времени |
| QAS-6 | Без ReHLDS API — нет краха, понятное сообщение | Bootstrap | review/ручной |
| QAS-7 (non-interference) | Плагин не меняет поведение сервера: все хуки `callNext`+propagate; `PreprocessPacket` всегда возвращает результат цепочки | HookManager-контракт | review + unit-тест «wrapper возвращает вход» |
| QAS-8 | Проект собирается `make` (32-бит) и `make test` зелёный | Makefile | CI/локально |

---

## 16. Решения владельца и открытые вопросы

### Закрыто (владелец, 2026-10-09)
1. **Платформа v1:** Linux-first; Windows — фаза 2.
2. **Стягивание:** файлы с **суточной ротацией**, забираются по FTP; плюс **инструменты анализа/агрегации** для агента (вместо RCON-flush+внешний агент).
3. **Активность:** строго пассивный v1.
4. **Внешний агент:** не в v1, отдельная фаза.
5. **Лицензия/распространение:** внутренний инструмент; GPL-вендоринг приемлемо.
6. **Профиль/конвенции:** `docs/design.md`, `docs/plan.md`, `docs/journal.md`; reviewers oracle/momus; язык RU; коммит только по явной просьбе.

### Открыто (не блокирует v1; уточнить при эксплуатации)
- **O1. Объём логов/частота.** `hldsmon_disk_hz` и `hldsmon_client_hz` — подтвердить по фактическому объёму и стоимости FTP (default 1 Гц).
- **O2. Retention/PII.** Срок хранения IP/SteamID/userinfo; политика приватности. v1 — не удаляем, владелец чистит файлы сам.
- **O3 (целевые сборки ReHLDS). РЕШЕНО:** целевой сервер — ReHLDS API **3.14**; рабочий минимум — 3.14 (хукчейн-vtable совпадает с 3.15; отличие лишь в неиспользуемой `SetServerPause`). Требование хостинга: glibc **≤ 2.17**; сборка образом `hun1er/oracle-7-cxx-build-env-gnu` со статическим libgcc/libstdc++.

---

## 17. Дорожная карта

- **Фаза 0:** скелет репо, Makefile, version_script, вендоринг заголовков, каркас хостовых тестов.
- **Фаза 1:** host-testable ядро (`metrics.h`, `ring_buffer.h`, `delta.h`, `serialization.*`) + unit-тесты.
- **Фаза 2:** LogWriter (суточная ротация, буферизация, права) + тесты.
- **Фаза 3:** движковая обвязка: Bootstrap, HookManager, хуки, Sampler, EventRecorder, `engine_layout` (проба), `platform_linux`. Компиляция против вендоренных заголовков.
- **Фаза 4:** Control (RCON `hldsmon_status`/`hldsmon_flush_log`).
- **Фаза 5:** `tools/analyzer` (Python) + фикстур-тесты.
- **Фаза 6:** верификация (`make`, `make test`), документация, журнал.
- **Фаза 7:** коммит — **только по явной просьбе владельца**.

---

## 18. Disposition замечаний design-review (oracle)

| ID | Замечание | Как учтено |
|---|---|---|
| **B1** | «`PreprocessPacket`: всегда `return true`» короткозамкнуло бы цепочку и сломало сервер | §5: контракт «все хуки `callNext` + propagate»; `PreprocessPacket` → `return callNext(...)`; правило 1. |
| **B2** | `static_assert` не валидирует runtime-layout; путь получения `client_t*` не задан | §4/§13: `engine_layout` + **runtime-проба** инвариантов (сверка `netchan.remote_address` ↔ `GetRemoteAdr()`, `userid` ↔ `GetId()`, монотонность seq) + fail-closed; доступ только via `GetId()→GetClient_t()`, без каста `IGameClient*`; зафиксировать сборку. Приложение B. |
| M1 | Ранний suppressor в цепочке → пропуск дропа; приоритеты не заданы | §5: регистрация в голове цепочки + fallback-детект (`Steam_NotifyClientDisconnect`, `HandleNetCommand`); фиксируем приоритеты в коде. |
| M2 | Переиспользование слота клиента → смешение игроков в кольце | §6.2/§7: `gen`-тег (connect-time/SteamID); сброс кольца на `ClientConnected`. |
| M3 | Блокирующий flush на server thread | §7/§8: непрерывная буферизованная запись, малый `fflush`; RCON-команды лишь сигнализируют; крупная сериализация не на потоке кадра. |
| M4/M5 | mmap first-touch и seqlock crash-recovery | **Устранено сменой модели хранения** (§7): mmap/seqlock убраны, используется append JSONL. |
| M6 | QAS неверифицируемы; нет non-interference | §15: метод замера указан, добавлен QAS-7; QAS-3 переформулирован под append. |
| M7 | Нет `rate/cl_updaterate/cl_cmdrate` из userinfo; fill reliable-канала | §6.2: парсинг userinfo-ключей + `rel_fill_pct`. |
| M8 | PII/права/диск/токен | §7: 0600/0700, `hldsmon_max_mb`, счётчик ошибок; §16 O2. |
| M9 | Хардкод 32 клиентов | §6.1/§7: `maxclients` в рантайме. |
| M10 | Слабая матрица альтернатив | §14: взвешенные критерии + cost/лицензия. |

**Приложение B (ABI-безопасность):** доступ к `client_t`/`netchan_t` только через `engine_layout`; на `Meta_Attach` — проба инвариантов против публичных геттеров; при несовпадении — fail-closed. `static_assert` проверяет согласованность вендоренных заголовков (паккинг/размеры), но **не заменяет** runtime-пробу.

**Приложение A (схема JSONL):** Полная каноническая схема всех типов и вложенных срезов — `docs/plan.md`, Приложение P1. Ниже — примеры.
```json
{"type":"server","t":1712345678.123,"frame_ms":{"min":3.1,"avg":4.0,"max":61.2},"hitch":{"gt50":2,"gt100":0},"cpu_pct":41.2,"rss_kb":120000,"players":17,"real":16,"proxy":0,"fake":1,"in_kbps":812.4,"out_kbps":1400.1,"edicts":245,"map":"de_dust2","uptime_s":84213,"fps":100.0}
{"type":"client","t":1712345678.5,"id":7,"gen":1712000000,"ping":42,"loss":3,"dloss":1,"choke":0,"dchoke":0,"in_seq":100234,"out_seq":99980,"rel_fill":3,"rate":20000,"updaterate":101,"cmdrate":101,"state":7,"flow_in_avg":12.3,"flow_out_avg":18.1}
{"type":"drop","t":1712345679.0,"id":7,"gen":1712000000,"reason":"Reliable channel overflowed","category":"overflow","crash":false,"observed_via":"sv_dropclient","client":{},"netchan":{},"window":[],"server":{},"neighbors":{}}
{"type":"reject","t":0,"ip":"1.2.3.4","stage":"SV_CheckUserInfo:password"}
{"type":"mapchange","t":0,"map":"de_nuke"}
{"type":"hitch","t":0,"duration_ms":120}
```
