# HldsMonitoring — дизайн модуля сбора телеметрии ReHLDS (rev 4)

- **Статус:** rev 2 (база v1) — Reviewed, замечания сведены в §18; решения владельца зафиксированы в §16. **Стадия 3 (`modnet`)** — **rev 3 (modnet) — design re-reviewed after revision; residuals folded** (см. §19; ревью и остатки — §19.13); открытые вопросы §19.11 закрыты. **Стадия 3b (`retention`)** — **rev 4 (retention) — revised after oracle design-review (R-1..R-8 folded); owner questions §20.10 закрыты; план добавлен в `docs/plan.md`** (см. §20).
- **Decision state:** база v1 — архитектура выбрана (native Metamod collector, Linux-first, пассивный, файловое логирование с суточной ротацией). `modnet` — механизм выбран (**GOT/PLT interposition**, §19.3/§19.5); все развилки закрыты (§19.11), ревью — §19.13. `retention` — механизм выбран (**host-testable отбор** `retention_select` + prune на open/ротации, §20.2/§20.5); развилки — §20.10.
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
| `hldsmon_retention_days` | cvar | максимум дневных файлов логов (вкл. текущий); `<=0` — выкл | 3 |
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

---

## 19. `modnet` — per-`.so` атрибуция сетевого трафика (Стадия 3)

> Новый subsystem. Имя стадии — **«Стадия 3 — modnet»**; не путать с дорожной картой §17 (там «Фаза 2» = Windows, «Фаза 3» = внешний агент — отдельные пункты роадмапа). Журнал базы: стадии 2…2f.

### 19.1 Контекст и проблема

Коллектор v1 (§6) атрибутирует сетевой трафик **игровому** пути: per-client `netchan.flow[*]` и `PreprocessPacket` дают in/out KB/s, pps и размеры **игрового** канала. Между тем значимая и сейчас невидимая доля трафика процесса HLDS рождается в **других загруженных объектах того же процесса**:

- AMXX-модуль **EasyHTTP** (`easy_http_amxx_i386.so`) выполняет HTTP на собственном **пуле worker-потоков**; `libcurl`/`c-ares`/`OpenSSL`/`zlib` влинкованы в него **статически** (§19.2).
- AMXX-модуль **`sockets`** (`sockets_amxx_i386.so`) делает raw-вызовы libc `socket/connect/getaddrinfo/send/recv/close` **синхронно на server thread**.

При триаже дропов невозможно ответить: всплеск исходящего трафика, исчерпание сокетов или залипание DNS пришли из игрового канала или из AMXX-модуля? Нужна **per-`.so` атрибуция** счётчиков соединений и байт, вписанная в существующую модель одного JSONL-файла, **не нарушающая пассивность** (N1, INV-1) и не деградирующая игровой путь.

**Границы атрибуции (scope).** modnet атрибутирует трафик **прямых вызовов в целевом объекте** — вызовов, идущих через GOT/PLT этого объекта. Байты, которые libc генерирует **внутри себя** при обработке вызова (прежде всего DNS-обмен резолвера libc при `getaddrinfo`), в scope **не входят** (N10): для `sockets` считаем только сам вызов `getaddrinfo`, но не внутренний DNS-обмен libc; при этом DNS `easy_http` идёт через статически влинкованный `c-ares`, чьи сокетные вызовы находятся в самом объекте `easy_http_...so` → **считаются**.

### 19.2 Проверенные технические факты (ground truth)

- **AMXX EasyHTTP** = отдельный нативный модуль `easy_http_amxx_i386.so` (upstream `Next21Team/AmxxEasyHttp`), собран со **статически влинкованными** `libcurl` (curl-8.20.0) + `c-ares` + `OpenSSL` + `zlib`; HTTP-запросы идут на **пуле worker-потоков** (до ~10 `std::thread`), колбэки дренируются на server thread. ⇒ `socket/connect/send/recv/close/getaddrinfo` исходят из адресного пространства `easy_http_amxx_i386.so`.
- **AMXX `sockets`** = отдельный нативный модуль `sockets_amxx_i386.so` (upstream `alliedmodders/amxmodx`, `modules/sockets/sockets.cpp`); **raw libc** `socket/connect/getaddrinfo/send/recv/close`; **синхронно на server thread**; без потоков и без libcurl. ⇒ его syscall'ы исходят из `sockets_amxx_i386.so`.
- **ASSUMPTION — статическая линковка (H4; проверить при реализации).** Задеплоенные сборки совпадают с upstream, и `libcurl`, `c-ares`, `OpenSSL`, `zlib` в `easy_http_amxx_i386.so` **не являются динамическими**. Проверки **явные и полные**:
  - `ldd addons/amxmodx/modules/easy_http_amxx_i386.so` — **НЕ** должен перечислять `libcurl.so.4`, `libssl.so*`, `libcrypto.so*`, `libcares.so*`, `libz.so*` (иначе соответствующий код живёт в отдельном объекте, и syscall'ы исходят из него, а не из `easy_http_...so`);
  - `nm -D --undefined-only addons/amxmodx/modules/easy_http_amxx_i386.so` — не должно быть неразрешённых `curl_easy_*`/`SSL_*`/`ares_*`/`inflate*`/`deflate*`;
  - `ls addons/amxmodx/modules/` — фактический набор модулей (для `hldsmon_modnet_targets`);
  - **зафиксировать идентичность задеплоенных модулей** (путь + `sha256sum`) в журнале стадии.
- **ASSUMPTION — семейство `send`/`recv`, а не `read`/`write` (H5; проверить при реализации).** curl/OpenSSL/c-ares в `easy_http_amxx_i386.so` для сокетов используют семейство `send`/`recv`/`sendto`/`recvfrom` (а не `read`/`write`); oracle оценил это как вероятно верное. Проверить через `nm -D`/`objdump -T` (неразрешённые символы обоих модулей): наличие `send`/`recv`/`sendto`/`recvfrom` и отсутствие опоры на сокетные `read`/`write`. Если предположение ложно — вернуться к вопросу fd-registry (N7).
- **Почему атрибуция по ВЫЗЫВАЮЩЕМУ `.so`, а не по провайдеру libc.** В процессе один общий провайдер `libc.so.6`; приписать байты «libc» бессмысленно. Ценность — знать **какой объект** инициировал syscall. Т.к. curl/c-ares/OpenSSL влинкованы в `easy_http_amxx_i386.so`, его syscall'ы рождаются в этом объекте; syscall'ы `sockets` — в `sockets_amxx_i386.so`. Перехват GOT **каждого целевого объекта** + адрес возврата даёт корректного владельца даже при статической линковке.

### 19.3 Решение: GOT/PLT interposition

Механизм: **патч GOT целевых загруженных `.so`**. 32-бит ELF:

1. Обойти загруженные объекты через `dl_iterate_phdr` — колбэк получает `struct dl_phdr_info` (**не** `link_map`, L2): базовый адрес `dlpi_addr` и массив `dlpi_phdr`; найти `PT_DYNAMIC` сканированием `dlpi_phdr` (переиспользует принятый в `platform_linux` паттерн dlopen/dlsym, не вводит нового способа загрузки).
2. Разобрать `PT_DYNAMIC`: `DT_SYMTAB`/`DT_STRTAB`/`DT_STRSZ`, `DT_JMPREL`/`DT_PLTRELSZ`, `DT_REL`/`DT_RELSZ`/`DT_RELENT` (на i386 релокации PLT — таблица `DT_REL`; конкретный вид релокаций определяется `DT_PLTREL` — ожидаем `DT_REL`, L3).
3. Для каждой релокации типа **`R_386_JMP_SLOT`** (PLT) и **`R_386_GLOB_DAT`** (данные; сюда же попадают `-fno-plt`-вызовы, M4) взять имя символа; если оно в целевом наборе — вычислить адрес GOT-слота (`base + r_offset`).
4. **Истинный оригинал (C1).** Оригинал **не берётся** из текущего значения слота: под `-z lazy`/частичном RELRO слот может содержать stub `_dl_runtime_resolve`. Если сохранить stub и вызвать его из обёртки, ленивый резолвер найдёт адрес и **перезапишет GOT-слот поверх нашей обёртки** → интерполяция исчезает (one-shot no-op). Поэтому истинный провайдер резолвится **независимо** — `dlsym(RTLD_DEFAULT, name)` — и сохраняется **он** как `original`. Используем **только `RTLD_DEFAULT`** (R4): libc предшествует плагину в порядке загрузки, а `RTLD_NEXT` может вернуть NULL/неверный адрес. **`BIND_NOW` (R5):** либо требовать `BIND_NOW`/полный RELRO для целевого объекта и **fail-closed**, если объект лениво-связан; либо на каждом rescan **проверять, что слот всё ещё содержит нашу обёртку, и перепатчивать**, если он был перезаписан редким in-flight-резолвером. После подмены слота ленивый резолвер уже **не запускается**: он срабатывает только при переходе по PLT через *незарезолвленный* слот, который мы больше не используем (обёртка зовёт `original` напрямую).
5. `mprotect` **минимальной страницы** слота в RW, записать адрес обёртки. **Права страниц (M3):** портируемо «прочитать» их у ядра нельзя — перед первым `mprotect` снять снимок прав из `/proc/self/maps`, запоминать исходно-read-only страницы и восстанавливать **ровно** исходные права; **никогда не оставлять RWX**.
6. **Таблица оригиналов (H2).** Пары `(слот, original)` хранятся **постоянно** (никогда не освобождаются/не очищаются, пока плагин загружен); восстановление на disable/`Meta_Detach` — **атомарная 32-битная запись** исходного значения. Нельзя освобождать оригиналы, пока какой-либо целевой объект может исполняться. **Окно недосчёта:** вызов, успевший загрузить старый слот до патча, не учитывается — известное ограничение. Так как целевые модули (пул EasyHTTP) живут **до** нашего enable, гарантия «обёртка установлена до первого использования» **не даётся** — полагаемся на прозрачность обёртки и fail-closed probe.
7. **Восстановление (M2/R2):** перед записью исходного значения слот **пере-валидируется** против текущей карты `dl_iterate_phdr` (объект всё ещё отображён по тому же `base`, слот — в границах отображённой области) — чтобы не писать в освобождённую память (M2), — а текущее значение слота **сравнивается с нашей обёрткой**. Запись выполняется **только если текущее значение == наша обёртка** (compare-then-store): иначе слот либо уже восстановлен, либо **другой модуль, перезагруженный по тому же `base`**, и запись чужого значения в чужой символ была бы повреждением (R2). Затем `mprotect` RW → атомарная запись → возврат прав.

**Конфликты интерполяции и запреты.** Другие плагины, патчащие GOT (напр. ReUnion/ReChecker), могут перекрыть наш слот (или мы — их); порядок зависит от того, кто записал последним — конфликт принимается и фиксируется в статусе. **PaX/SELinux могут запретить `mprotect`** — probe обязан трактовать отказ как **fail-closed** (остаёмся OFF + предупреждение).

**Базовый набор символов (reconciled, M7).** Так как fd-registry отложен (N7), `socket`/`close` не служат учёту (`conns` = успешные **блокирующие** `connect()` + `accept()`) — **исключены** из базового набора этой стадии:

| Назначение | Символы |
|---|---|
| Байты | `send`, `recv`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg` |
| Соединения | `connect`, `accept` |
| Счётчик DNS-вызовов (поле `dns_calls`, без байт) | `getaddrinfo` |

**Неблокирующий `connect` (R1).** libcurl и c-ares используют неблокирующие сокеты: `connect()` возвращает `-1` при `errno == EINPROGRESS` (реже `EALREADY`), а успех наблюдаем лишь позже — по `SO_ERROR` после готовности к записи, которую мы **не** перехватываем. Такие вызовы нельзя считать провалом: `EINPROGRESS`/`EALREADY` учитываются как **pending**, а не `conn_fail`. Поскольку завершения неблокирующих соединений не наблюдаемы без перехвата writability-пути, для неблокирующих модулей (EasyHTTP) `conns` — **best-effort**; полный учёт достигается только вместе с отложенным fd-registry (N7). Счётчик `connect_attempts` (все вызовы `connect()`) при этом учитывает и pending.

**`read`/`write` (опция, отложена).** Не входят в базовый набор; учёт возможен только через **fd-registry** («этот fd — сокет»), наполняемый обёртками `socket`/`accept`/`close`. Стоимость: атомарный lookup на каждый `read`/`write` целевых объектов (в hot-path libcurl заметно) и **ложные пропуски** для fd, созданных до enable или в обход `socket()` (`pipe`/`dup`). Отложено (§19.11 Q2).

### 19.4 Целевые объекты, опциональность, безопасность (locked)

- **Master-switch** `hldsmon_modnet` — cvar, default **0 (OFF)**.
- **`hldsmon_modnet_targets` (H3, решено).** Пусто ⇒ патчится **встроенный список** {`easy_http_amxx_i386.so`, `sockets_amxx_i386.so`}; непусто ⇒ только перечисленные объекты (basename). При действии встроенного значения по умолчанию — **стартовое уведомление в лог**. (Ранее предполагалось «пусто = все»; заменено на встроенный список — см. §19.11 Q4 и §19.13 H3.)
- **«Подходящий» объект (eligible) — точное определение.** Загруженный разделяемый объект (`.so`, отображён из файла), **не** являющийся: `libc.so*`/glibc, динамическим загрузчиком `ld-*.so*`, **главным исполняемым файлом** (HLDS) и самим плагином HldsMonitoring. Ядро/движок/главный exe — **hard-exclude** (INV-M3). Признак «отображён из файла» — `dlpi_name`, начинающийся с `/`.
- **Q7 (решено).** Патчатся **только чужие модули**; GOT движкового/игрового объекта **не** патчится (его игровой канал уже покрыт §6). Поэтому особая арифметика base для главного exe (L4: `base==0` для non-PIE) не применяется.
- **Прозрачность (INV-1 extend):** обёртка = реальный вызов → реальный возврат; сохранение `errno`; сохранение семантики возврата (`-1`/NULL); никаких дропов/задержек/изменения порядка. Пассивность (N1) сохраняется для игрового пути.
- **Fail-closed probe до включения (INV-4 style):** проверяются (a) нахождение релокаций целевых символов; (b) **корректный round-trip через `socketpair`** (M6) — реальные `send`/`recv` через обёртку передают/возвращают то же, `errno` не искажается (R6: socketpair-проба покрывает только `send`/`recv` — подмножество патченного набора; для остальных символов проверяются нахождение релокаций и повторный инкремент); (c) счётчик **инкрементируется при ≥2 последовательных вызовах** (C1 — защита от one-shot резолвера); (d) `mprotect` не запрещён (PaX/SELinux). При провале — **остаёмся OFF + предупреждение**, плагин грузится нормально.
- **Kill-switch в рантайме:** `hldsmon_modnet 0` атомарно **восстанавливает** оригинальные GOT-слоты (с пере-валидацией, §19.3 п.7); `Meta_Detach` — полное восстановление.
- **Rescan (Q1, решено).** Периодически на существующем **1 Гц server-tick** — каждые `hldsmon_modnet_rescan_sec` (новый cvar, default **5**) — плюс **немедленный rescan на `SV_Spawn_f`**. Подхват модулей, загруженных позже (смена карты / перезагрузка плагина AMXX); при unload запись инвалидируется (M2/§19.3 п.7).
- **Потоки (INV-3 amend):** патч GOT выполняется на server thread, но **обёртки исполняются на потоке-вызывателе**, в т.ч. на пуле EasyHTTP. На 32-бит x86 выровненная 4-байтная запись указателя атомарна; `mprotect` выполняется до записи; гарантия «до первого использования» **не даётся** (модули живут до enable) — полагаемся на прозрачность и probe.

### 19.5 Альтернативы и взвешенная матрица

Критерии (вес): байты/модуль (0.20), in-process без внешнего артефакта (0.10), привилегии/root (0.10), blast radius (0.15), точность атрибуции (0.15), объём работ (0.10), портируемость (0.10), вписываемость в единый JSONL (0.10). Оценки 1–5 (5 — лучше).

| Вариант | Байты/модуль | In-proc | Привилег. | Blast r. | Точность | Работы | Портируем. | JSONL | Итог |
|---|---|---|---|---|---|---|---|---|---|
| **GOT/PLT interposition (выбран)** | 5 | 5 | 5 | 3 | 5 | 3 | 3 | 5 | **4.30** |
| `/proc`-интроспекция (дополнение) | 1 | 3 | 5 | 5 | 2 | 4 | 4 | 3 | 3.15 |
| AMXX-native hooks | 2 | 4 | 5 | 2 | 3 | 3 | 2 | 4 | 2.95 |
| eBPF/uprobe (внешний) | 4 | 1 | 1 | 5 | 4 | 2 | 1 | 2 | 2.85 |
| LD_PRELOAD | — | — | — | — | — | — | — | — | **снят (constraint)** |

- **GOT/PLT (выбран).** Полный доступ к байтам/модуль, in-process, без root, точная атрибуция по caller, ложится в единый JSONL. Минусы — blast radius (патч чужой памяти; закрыт default-off + probe + узким фильтром), 32-бит ELF-специфика (совпадает с v1), объём работ (ELF-парсер + probe).
- **`/proc`-интроспекция.** Без байт-счётчиков per-`.so` (первичный критерий провален) ⇒ годится лишь как **дополнение** (интерфейс-тотал для residual), не как основной механизм.
- **AMXX-native hooks.** ABI-coupling с AMXX, покрывает только AMXX и лишь модули с подходящим API; хрупко при обновлении AMXX.
- **eBPF/uprobe.** Не возмущает процесс и не требует патча, но **нужен root/CAP_BPF**, это **внешний артефакт** (отдельный инструмент + корреляция), точность зависит от user-stack unwinding и версии ядра; не вписывается в единый JSONL без отдельной сборки.
- **LD_PRELOAD.** Снят **до скоринга** по binding constraint: плагин грузится в **уже запущенный** процесс, момент preload упущен; к тому же не позволяет выбирать объекты.
- **Конфликты и запреты (риск выбранного варианта).** Чужие GOT-патчеры (ReUnion/ReChecker) могут перекрыть слот; `mprotect` может быть запрещён (PaX/SELinux). Оба случая обрабатываются: конфликт — фиксируется в статусе, запрет `mprotect` — fail-closed (§19.3, QAS-M10).

### 19.6 Влияние на требования, контракты и инварианты

- **Требования.** Новый **G7** (extends G1): per-`.so` счётчики соединений/байт для **прямых** сокетных вызовов модулей (scope — §19.1/§19.3). Сохраняются G5 (ограниченные накладные) и N1 (пассивность). Новые: **N6–N10** (§19.9).
- **Контракты.** Каноническая схема JSONL (`docs/plan.md`, Приложение P1) расширяется типом `modnet` (§19.8); `exports.whitelist`/`version_script.lds` **не меняются** (всё `local: *`).
- **Инварианты:**
  - **INV-1 (расширяется):** прозрачность распространяется и на modnet-обёртки — реальный вызов + реальный возврат + сохранение `errno`; никаких дропов/задержек на игровом пути.
  - **INV-2 (расширяется):** путь обёртки без аллокаций (кэш диапазонов — фиксированный предвыделенный массив, INV-M5; hot-path — атомарные счётчики и сравнение диапазона).
  - **INV-3 (аменд):** формулировка «весь сбор — на server thread» **ослабляется для modnet**: обёртки исполняются на потоке-вызывателе (включая пул EasyHTTP); счётчики **атомарные**; **эмиссия JSONL остаётся на server thread** (читает атомарные снимки). Базовые хуки §5 — по-прежнему на server thread.
  - **INV-M1 (modnet, прозрачность):** каждая обёртка транзитивна и сохраняет `errno`/возврат (§19.10 QAS-M1).
  - **INV-M2 (modnet, безопасность):** fail-closed probe до включения (в т.ч. отказ `mprotect`); master-off по умолчанию; таблица оригиналов **постоянна** (H2); восстановление — атомарная запись **после пере-валидации** слота против `dl_iterate_phdr`; периодический rescan (§19.10 QAS-M2/M3/M7/M10).
  - **INV-M3 (modnet, охват):** патчатся только GOT-слоты **целевых символов** у **выбранных foreign-объектов**; `libc`/`ld.so`/главный exe/сам плагин — hard-exclude (§19.10 QAS-M5/M6).
  - **INV-M4 (modnet, учёт):** счётчики атомарные, без потерь при конкурентных вызовах; эмиссия — согласованный снимок (§19.10 QAS-M4).
  - **INV-M5 (modnet, кэш диапазонов):** кэш `адрес→объект` — **фиксированный предвыделенный массив** с bounded-overflow (превышение ёмкости ⇒ fail-closed); публикуется **lock-free** атомарным обменом указателя, т.к. обёртки читают его конкурентно (M5).
  - Канонический список INV-1..INV-6 остаётся в `docs/plan.md`; INV-M1..M5 вводятся настоящим разделом. **L5:** INV-M\* переносятся в `docs/plan.md` до plan-гейта.

### 19.7 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **bootstrap** | Регистрация `hldsmon_modnet` (0), `hldsmon_modnet_targets` ("") и `hldsmon_modnet_rescan_sec` (5); после layout-probe — modnet-probe; enable при `hldsmon_modnet=1`; disable+restore на `Meta_Detach`. |
| **hook_manager** | Новых hookchain'ов нет; modnet хуков не регистрирует. |
| **sampler / hooks (L1)** | На 1 Гц server-tick: (a) периодический rescan раз в `hldsmon_modnet_rescan_sec` и **немедленный rescan на `SV_Spawn_f`** — триггеры в sampler/`hooks`, **не** в hook_manager; (b) эмиссия `modnet`-records из атомарных снимков. |
| **events / serialization** | Новый POD `ModnetRecord` + `write_modnet_json` (no-alloc, host-testable); расширение P1. |
| **LogWriter** | Без изменений (новый тип идёт тем же JSONL-путём). |
| **control** | `hldsmon_status` += состояние modnet (enable, probe ok/fail, targets, число пропатченных объектов/слотов, restored). |
| **platform_linux** | **Без изменений** для modnet: ELF/GOT-код (`dl_iterate_phdr`/`dladdr`, `mprotect`, снимок прав из `/proc/self/maps`) вынесен в header-free 32-бит TU `src/modnet_got.cpp` (libc/`dl`/`sys/mman` напрямую). |
| **engine_layout** | Без изменений (modnet не читает `client_t`/`netchan_t`). |
| **Структура/сборка** | Новые `src/modnet.{h,cpp}` (host-testable ядро: записи/атомарные счётчики/чистая логика) + `src/modnet_got.cpp` (ELF/GOT-адаптер — header-free 32-bit TU: libc/`dl`/`sys/mman` напрямую, без движковых заголовков) — зеркалит split `engine_layout.h`/`.cpp`. **M1:** `Makefile` собирает host-тесты из **фиксированного** `CORE_SRCS` — план обязан расширить его (`src/modnet.cpp`), иначе ядро modnet не попадёт в host-тесты. **H6:** план добавляет CI-цель со **32-битным синтетическим `.so`-харнессом** (§19.10). |

### 19.8 Запись JSONL `modnet` (design-level)

Один `modnet`-record на **каждый целевой `.so`** раз в секунду. Каноническая схема — `docs/plan.md`, Приложение P1 (владелец схемы — фаза плана). Design-level поля и семантика:

| Поле | Тип | Семантика |
|---|---|---|
| `type` | string | `"modnet"` |
| `t` | float | wall-clock epoch seconds (момент эмиссии) |
| `module` | string | короткая метка модуля (напр. `easy_http_amxx_i386`) |
| `so` | string | basename загруженного объекта |
| `conns` | uint | Δ успешных **блокирующих** `connect()` (return 0) + успешных `accept()` за интервал; **best-effort** для неблокирующих модулей (R1) |
| `conn_fail` | uint | Δ `connect()` с ошибкой (`return -1`, `errno ∉ {EINPROGRESS, EALREADY}`) за интервал (Q6/R1) |
| `connect_attempts` | uint | Δ **всех** вызовов `connect()` за интервал (любой результат; включая pending, R1) |
| `dns_calls` | uint | Δ вызовов `getaddrinfo` за интервал (без байт, R3) |
| `in_bytes` | uint64 | Δ принятых байт за интервал |
| `out_bytes` | uint64 | Δ отправленных байт за интервал |
| `in_kbps` | float | `in_bytes / Δt / 1024` |
| `out_kbps` | float | `out_bytes / Δt / 1024` |

**Семантика:** внутренние счётчики **кумулятивные, атомарные (uint64)**; в JSONL эмитятся **Δ за интервал**; при unload/reload объекта счётчики сбрасываются (анализатор делит ряд по `(module, so, run)`). Отсутствующие значения — по правилам P1 (`null`/zero); значения неотрицательны. `in_bytes`/`out_bytes` считают только **прямые** сокетные вызовы объекта (scope §19.1); resolver-внутренний DNS libc не входит (N10). **Счётчики соединений (R1):** `connect_attempts` = все `connect()`; `conns` = успешные **блокирующие** `connect()` (return 0) + `accept()`; `conn_fail` = `connect()` с ошибкой; `connect()` с `EINPROGRESS`/`EALREADY` (неблокирующие модули) относится к **pending** и не входит ни в `conns`, ни в `conn_fail`. Для неблокирующих модулей (EasyHTTP) успешные завершения не наблюдаемы без перехвата writability-пути, поэтому `conns` для них — **best-effort**.

**Эмиссия (Q5, решено):** один `modnet`-record на **каждый включённый целевой объект** на 1 Гц; запись эмитится для объектов, у которых **была активность** за интервал (пропуски трактуются анализатором как ноль).

**Влияние на анализатор:** отдельные per-`.so` ряды (`modnet_series`) наряду с per-client; корреляция `modnet` с `drop`/`hitch` в окне события; в `digest` — сводка по модулям.

**Residual (интерфейс − Σ игровой поток):** **не включаем** в этой стадии (Q3 — отложено, §19.11). Residual требует второго, независимого источника (агрегат интерфейса через `/proc/net/dev` или rtnetlink), который меряет **host-level** трафик (все интерфейсы, loopback, прочие процессы, контейнерные расхождения) и смешивает процессный и хостовый уровень; при этом ответ на вопрос триажа («какой модуль дал всплеск») уже даёт per-`.so` ряд.

### 19.9 Не-цели

- **N6 (отложено, Q3).** Интерфейс-тотал и residual-анализ — не в этой стадии (§19.8).
- **N7 (отложено, Q2).** Учёт `read`/`write` через fd-registry — не в базовом наборе (§19.3).
- **N8.** Активное вмешательство в чужие модули/пакеты: только наблюдение; никаких дропов/модификации данных/задержек (N1 сохраняется).
- **N9.** Не покрываются вызовы в обход GOT: `syscall()`/inline-asm, а также адресно-разрешённые вызовы (`dlsym("send")`/указатели на функции, M4). При этом `-fno-plt`-вызовы (indirect через GOT) **покрываются** через `R_386_GLOB_DAT`.
- **N10.** Резолвер-внутренний трафик (DNS-байты, которые libc генерирует внутри `getaddrinfo`) — вне scope; считаем только сам вызов `getaddrinfo` (поле `dns_calls`, §19.1/§19.8).

### 19.10 Приёмочные качественные сценарии (QAS-M)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-M1 (transparency) | При modnet=1 наблюдаемое поведение перехваченных вызовов идентично: возврат и `errno` совпадают; синтетический целевой `.so` получает тот же результат socketpair-round-trip | real call → save/restore `errno` → real return; `original` из `dlsym` | host-тест обёртки на фейке + CI-харнесс (синтетический `.so`) |
| QAS-M2 (fail-closed probe) | При невозможности найти релокации, провале round-trip или отказе `mprotect` (PaX/SELinux) — modnet остаётся OFF + предупреждение; краха нет | probe до включения | CI-харнесс (сломать probe/`mprotect`) + review |
| QAS-M3 (restore-on-disable) | `hldsmon_modnet 0` атомарно восстанавливает GOT-слоты (с пере-валидацией и compare-then-store); после восстановления счётчик попаданий обёртки **не растёт** — допускается только один benign **in-flight** инкремент от вызова, успевшего прочитать старый слот; `Meta_Detach` восстанавливает всё | save/restore + compare-then-store + mprotect | CI-харнесс (до/после) |
| QAS-M4 (thread-safe) | N потоков, конкурентно вызывающих целевой символ, дают точный суммарный счётчик байт (нет потерянных обновлений); снимок эмиссии согласован | атомарные uint64 + snapshot | host-тест конкурентного счётчика (threads) |
| QAS-M5 (attribution) | Трафик синтетического целевого `.so` атрибутируется ему (метка), а не `libc`; трафик `sockets` — `sockets_amxx_i386.so` | caller return address + lock-free кэш диапазонов | CI-харнесс на синтетическом `.so` (+ боевой smoke) |
| QAS-M6 (zero when disabled) | При modnet=0 ни один GOT-слот не изменён, ни одна обёртка не на пути; замер baseline не отличается | master-off гейт | CI-харнесс (сравнение GOT/поведения до/после) |
| QAS-M7 (rescan) | Модуль, загруженный после enable, подхватывается на ближайшем rescan (≤ `hldsmon_modnet_rescan_sec`); немедленно на `SV_Spawn_f` | периодический rescan + mapchange | CI-харнесс (`dlopen` целевого `.so` после enable) |
| QAS-M8 (volume) | Дополнительный объём логов от `modnet` ограничен (один record/с на активный объект) | один record/с/объект | review бюджета + unit-тест размера `ModnetRecord` |
| QAS-M9 (lazy binding, C1) | При `-z lazy`/частичном RELRO интерполяция **не** превращается в one-shot: ≥2 последовательных вызова через слот дают **растущий** счётчик; вызов не уходит в `_dl_runtime_resolve` поверх обёртки | `original` из `dlsym`; probe зовёт ≥2× | CI-харнесс (двойной вызов) |
| QAS-M10 (conflict/denial) | Если другой плагин перекрыл наш слот или `mprotect` запрещён — фиксируется в статусе и не приводит к краху; при запрете — fail-closed | детект чужой записи; handled `mprotect` | review + CI-харнесс (эмуляция) |

### 19.11 Вопросы (закрыты: владелец + автономные решения)

Открытых вопросов нет. Все ранее поднятые развилки (см. §19.13) закрыты:

1. **Q1 (каденция rescan) — решено.** Периодически на 1 Гц server-tick каждые `hldsmon_modnet_rescan_sec` (новый cvar, default **5**) плюс **немедленный rescan на `SV_Spawn_f`**.
2. **Q2 (`read`/`write` через fd-registry) — отложено** (N7).
3. **Q3 (интерфейс-тотал/residual) — отложено** (N6).
4. **Q4/H3 (`hldsmon_modnet_targets`) — решено (владелец).** Пусто ⇒ **встроенный список** {`easy_http_amxx_i386.so`, `sockets_amxx_i386.so`}; непусто ⇒ только перечисленные. Стартовое уведомление при встроенном значении; «eligible» и hard-exclude — §19.4.
5. **Q5 (каденция эмиссии) — решено.** Один `modnet`-record на **включённый целевой объект** на 1 Гц, эмитится для объектов с активностью за интервал (пропуски = ноль для анализатора).
6. **Q6 (семантика счётчиков соединений) — решено (уточнено R1).** `conns` = успешные **блокирующие** `connect()` (return 0) + `accept()`; `conn_fail` = `connect()` с ошибкой; `connect_attempts` = все `connect()`; `EINPROGRESS`/`EALREADY` (неблокирующие модули) — **pending**, ни в `conns`, ни в `conn_fail`; для неблокирующих модулей `conns` — **best-effort**.
7. **Q7 (охват движка/игры) — решено (владелец).** Только **чужие модули**; GOT движкового/игрового объекта **не** патчится.

### 19.12 План

Реализация modnet (фазы, DoD, расширение Приложения P1) добавляется в `docs/plan.md` на фазе планирования — **в этом артефакте план не пишется**. План обязан: (a) расширить Приложение P1 типом `modnet`; (b) расширить `Makefile` `CORE_SRCS` на `src/modnet.cpp` (M1); (c) добавить CI-цель со **32-битным синтетическим `.so`-харнессом** для attribution/restore/rescan (H6); (d) перенести INV-M1..M5 в инварианты плана до plan-гейта (L5). Каноническая схема `modnet` — `docs/plan.md`, Приложение P1.

### 19.13 Раздел ревью дизайна (oracle)

**Вердикт (первое ревью): PASS-WITH-REVISIONS.** Plan-гейт **закрыт** до исправления C1/H1/H2/H3/H6 и повторного ревью. Замечания и их disposition:

**Повторное ревью после правок: PASS с остатками (R1–R6); все остатки сложены (folded) в этой ревизии.** Plan-гейт разблокирован. Остатки:

| ID | Severity | Учтено |
|---|---|---|
| R1 | High | §19.3 (неблокирующий `connect`)/§19.8/§19.11 Q6: `EINPROGRESS`/`EALREADY` — **pending**, не fail; `conns` = успешные **блокирующие** `connect()`+`accept()`, добавлен `connect_attempts`; для неблокирующих модулей `conns` — best-effort (без перехвата writability). |
| R2 | Med | §19.3 п.7: restore — **compare-then-store** (писать только если текущее значение слота == наша обёртка), а не только проверка отображения по `base`. |
| R3 | Med | §19.8: добавлено поле **`dns_calls`**; `getaddrinfo` оставлен в базовом наборе. |
| R4 | Low | §19.3 п.4: только **`RTLD_DEFAULT`** (без `RTLD_NEXT`). |
| R5 | Low | §19.3 п.4: `BIND_NOW` — явно: fail-closed для лениво-связанного объекта либо re-verify на rescan и перепатч при clobber. |
| R6 | Low | §19.10 QAS-M3 смягчено (benign in-flight инкремент); §19.4: socketpair-проба покрывает только `send`/`recv` (подмножество набора). |

Ранее сведённые замечания первого ревью:

| ID | Severity | Учтено |
|---|---|---|
| C1 | Critical | §19.3 п.4: `original` резолвится независимо через `dlsym(RTLD_DEFAULT)`, **не** из текущего значения слота; `BIND_NOW` (fail-closed либо re-verify/перепатч, R5); probe зовёт целевой символ ≥2× (QAS-M9). Объяснено, почему ленивый резолвер не запускается после подмены слота. |
| H1 | High | §19.1/§19.3: scope явно сужен до **прямых вызовов в объекте**; resolver-внутренний DNS libc вынесен в N10; отмечено различие easy_http (c-ares → считаются) vs sockets (`getaddrinfo` DNS → нет). |
| H2 | High | §19.3 п.6: таблица оригиналов **постоянна**, restore — атомарная запись, окно недосчёта задокументировано, «до первого использования» не гарантируется; INV-M2. |
| H3 | High | §19.4 + §19.11 Q4: пусто ⇒ **встроенный список**, не «все»; стартовое уведомление при встроенном значении. |
| H4 | High | §19.2: полная проверка не-динамичности curl/c-ares/OpenSSL/zlib (`ldd` + `nm -D`) + фиксация `sha256sum` модулей в журнале. |
| H5 | High | §19.2: ASSUMPTION про семейство `send`/`recv` (не `read`/`write`) + проверка `nm -D`/`objdump`. |
| H6 | High | §19.10 + §19.12: 32-битный синтетический `.so`-харнесс в CI + host-testable ядро. |
| M1 | Med | §19.7/§19.12: `CORE_SRCS` — фиксированный; план расширяет его на `src/modnet.cpp`. |
| M2 | Med | §19.3 п.7 / INV-M2: пере-валидация слота против `dl_iterate_phdr` перед записью; `(soname, base)`-инвалидация помечена как неоднозначная при same-soname reload. |
| M3 | Med | §19.3 п.5: снимок прав из `/proc/self/maps`, минимальная страница, восстановление ровно исходных прав, никогда RWX. |
| M4 | Med | §19.9 N9: `dlsym`/function-pointer bypass; `-fno-plt` покрыт через `R_386_GLOB_DAT`. |
| M5 | Med | §19.6 INV-M5: фиксированный кэш с bounded-overflow + lock-free публикация атомарным обменом указателя. |
| M6 | Med | §19.4/§19.3: probe через **socketpair** с реальным round-trip + повторные инкременты. |
| M7 | Med | §19.3: базовый набор reconciled — убраны `socket`/`close`; {`send`,`recv`,`sendto`,`recvfrom`,`sendmsg`,`recvmsg`} + {`connect`,`accept`} + `getaddrinfo`. |
| L1 | Low | §19.7: rescan — sampler/tick + `SV_Spawn_f`, не hook_manager. |
| L2 | Low | §19.3 п.1: `dl_phdr_info`/`dlpi_phdr`, не `link_map`. |
| L3 | Low | §19.3 п.2: i386-релокации PLT — `DT_REL` (+`DT_PLTREL`). |
| L4 | Low | §19.4: main-exe base math не применяется (Q7 — движок не патчим). |
| L5 | Low | §19.6/§19.12: INV-M\* переносятся в `docs/plan.md` до plan-гейта. |

**«Не переусердствовать» (oracle: корректно, оставлено как есть):** выбор GOT/PLT-механизма; скан `DT_JMPREL` и `DT_REL` с `base+r_offset`; `mprotect` в RW достаточен для RELRO-страниц (нужно вернуть исходные права); атрибуция по адресу возврата корректна для прямых вызовов из объекта; форма INV-1/INV-2 (extend) и INV-3 (amend); на серверной стороне нет проблем с VAC. Отдельно учтён риск **конфликтов интерполяции** с другими GOT-патчерами (ReUnion/ReChecker) и **запрета `mprotect`** (PaX/SELinux) — оба ведут к fail-closed/статусу (§19.3/§19.10 QAS-M10).

---

## 20. Retention — автоудаление логов (Стадия 3b)

> Небольшая предрелизная стадия поверх суточной ротации §7. Имя стадии — **«Стадия 3b — retention»**; не путать с дорожной картой §17. Дизайн-ревью — `oracle`; план-ревью — `momus`; журнал стадии — `docs/journal.md`.

### 20.1 Контекст и проблема

Суточная ротация (§7) создаёт по одному файлу `hldsmon-YYYY-MM-DD.jsonl` в день и **никогда их не удаляет**: каталог `hldsmon_dir` растёт неограниченно (оценка §7 — 300–600 МБ/сутки при 32 клиентах). Это (а) исчерпывает диск при длительной эксплуатации и (б) бессрочно накапливает персональные данные (`ip`, `authid`/`steamid64`, `name` из §6.2) — открытый вопрос **O2** (§16). Нужно детерминированное авто-удаление старых ротированных дневных файлов.

### 20.2 Решение и точная семантика

Новый целочисленный cvar **`hldsmon_retention_days`** (default **3**) задаёт **максимум одновременно существующих дневных файлов, включая текущий**.

Пусть текущая UTC-дата — `T`, дата файла — `d` (обе — целые номера суток).

- **Держим** файл `d`, если `T - d <= N - 1`.
- **Удаляем** файл `d`, если `T - d > N - 1` (⟺ `d < T - (N-1)` ⟺ `d <= T - N`).

Итого: максимум `N` файлов = текущий день `T` плюс предыдущие `N-1` дневных файлов. **Пример (`N=3`, файлы `T, T-1, T-2, T-3`):** удаляются `T-3` и всё старше; `T, T-1, T-2` остаются.

- `N >= 1`: `N=1` ⇒ остаётся ровно один файл — текущий.
- `N <= 0`: retention **выключен** — не удаляется ничего (keep-all). **Решение владельца (§20.10 Q1).**

### 20.3 Scope и защита от ложного удаления

- Рассматриваются **только** имена точного шаблона `hldsmon-YYYY-MM-DD.jsonl` в каталоге `hldsmon_dir`.
- Кандидат берётся, только если `parse_log_date()` (§20.5) распознал `YYYY-MM-DD` и точный суффикс. Записи, не совпавшие с шаблоном (в т.ч. **маркер краша** §9, `.`/`..`, любые прочие файлы), не удаляются **никогда**.
- **Текущий файл не удаляется**: его дата всегда равна `T`, а предикат удаления требует `T - d > N-1 >= 0` (при `N>=1`) — для `d=T` невыполнимо. Дополнительно ФС-обёртка никогда не вызывает `unlink` для пути, равного `current_path()`.
- **R-8 (будущая дата).** Имена с датой **в будущем** (`d > T`, в т.ч. расхождение часов) дают `T - d < 0 <= N-1` и **сохраняются бессрочно**, пока дата `T` их не догонит. Это **безопасное направление** (ложное сохранение, а не ложное удаление) — принято осознанно; правка часов это устраняет.
- Удаляются только файлы; каталог не трогается.

### 20.4 Триггеры

Prune запускается в двух точках:

1. при успешном `LogWriter::open()` — старт плагина (стартовый prune);
2. при суточной ротации — в `LogWriter::maybe_rotate()` (смена UTC-даты, §7), **после** открытия файла нового дня.

Реализация: prune вызывается в конце успешного `open_day()` — единственного внутреннего пути открытия нового дня, которым пользуются обе точки.

**R-1 (значение cvar — только в рантайме).** Значение `hldsmon_retention_days` **не кэшируется в конфигурацию один раз**: сэмплер на каждом **1 Гц server-tick** перечитывает cvar и обновляет его в `LogWriter` (`set_retention_days(cvar_f(...))`) **до** вызова `maybe_rotate()` — по образцу live-гейта `plugin_enabled()`. Поэтому изменение cvar вступает в силу на ближайшей ротации (или при следующем `open()`). При старте значение выставляется один раз из `open_log()` до `open()`.

**R-6 (поток/аллокации).** Prune исполняется **только на server thread** в точках `open()`/ротации (не в hot path). На dir-I/O (`opendir`/`readdir`/`unlink`) и на **bounded**-аллокацию списка имён в этой точке ограничение INV-2 (hot path без аллокаций) **не распространяется** — оно относится к путям сбора/сериализации; сам отбор (`retention_select`) остаётся без аллокаций.

**Периодический re-check во время длительной работы не нужен.** Пересечь границу окна может только смена UTC-даты, которая **уже** вызывает ротацию (и, следовательно, prune); новые дневные файлы создаёт исключительно сам `open_day`. Внешние файлы (добавленные владельцем вручную в `hldsmon_dir`) вне scope. Отдельный таймер/повторная проверка не вводятся — это исключает лишний hot-path. (Отдельно от R-1: R-1 перечитывает **значение cvar** каждый tik, а не содержимое каталога.)

### 20.5 Host-testable логика отбора

Вся логика выбора — **чистая функция** (без ФС), покрываемая `make test` (ядро живёт в `src/retention.{h,cpp}` и компилируется нативным g++ без движковых заголовков, как `serialization`/`log_writer`):

```cpp
// src/retention.h — host-testable, без движковых заголовков.
namespace hldsmon {

// Разобрать basename "hldsmon-YYYY-MM-DD.jsonl" (точный шаблон).
// Возвращает true и заполняет y/m/d при точном совпадении, иначе false.
// Проверяется календарная дата: YYYY ∈ [1970,9999], MM ∈ [1..12],
// DD ∈ [1..дней_в_месяце(YYYY,MM)] (високос учтён — R-4).
bool parse_log_date(const char* name, int* year, int* month, int* day);

// Выбрать из `names` (count имён) подлежащие удалению при текущей UTC-дате
// (year,month,day) и retention_days == N. Чистая: без ФС и без аллокаций.
//  - N <= 0                        -> успех, *out_n = 0 (retention off);
//  - имя, не совпавшее с шаблоном   -> игнорируется;
//  - имя удаляется ⟺ day_num(name) <= today_num - N.
// Пишет невладеющие указатели на элементы `names` в `out` (ёмкость out_cap)
// и их число в *out_n. Возвращает true при успехе.
// R-2 (fail-safe, защита от опечаток в удаляющем коде): если кандидатов
// больше out_cap — возвращает false, НИЧЕГО не пишет и НЕ трогает *out_n.
// Вызывающий ФС-код при false выполняет РОВНО НОЛЬ `unlink` и никогда не
// итерирует `size_t`/`SIZE_MAX`.
bool retention_select(const char* const* names, size_t count,
                      int year, int month, int day, int retention_days,
                      const char** out, size_t out_cap, size_t* out_n);

} // namespace hldsmon
```

- `parse_log_date` — точный матч фиксированной длины (`hldsmon-` + 4 цифры + `-` + 2 + `-` + 2 + `.jsonl`); `YYYY ∈ [1970,9999]`, `MM ∈ [1..12]`, `DD` — **в пределах дней месяца с учётом високоса** (R-4; напр. `2026-02-29` отвергается). Принимается только basename (каталог задаёт вызывающий).
- Номера суток — внутренний чистый целочисленный `days_from_civil(y,m,d)` (без libc/`timegm`), что делает сравнение корректным на границах месяцев/лет.
- **R-2.** Возврат — `bool`; переполнение `out_cap` ⇒ `false` и **ноль удалений** (никакого магического `SIZE_MAX`). Проверяется QAS-R8.
- **Тонкая ФС-часть** (`LogWriter::prune()`): `opendir`/`readdir` → собрать basenames → `retention_select` → при `true` `unlink` каждого из `*out_n` выбранных, при `false` — ничего. Логики отбора в ней нет.

### 20.6 Обработка ошибок (§11)

- `opendir(dir)` не удался → предупреждение + `++error_count_`; запись/ротация **не** затрагиваются.
- Ошибка `unlink` отдельного файла → предупреждение + `++error_count_`; цикл не прерывается, остальные кандидаты удаляются.
- **Никаких крахов**; prune никогда не выключает запись и не меняет `enabled_`. Согласовано с §11 (IO/диск-ошибка ⇒ счётчик + предупреждение, продолжение работы).

### 20.7 Конфигурация и влияние на компоненты

Config §12 — добавить строку:

| Имя | Тип | Назначение | Default |
|---|---|---|---|
| `hldsmon_retention_days` | cvar | максимум дневных файлов логов (вкл. текущий); `<=0` — выкл | 3 |

| Компонент | Изменение |
|---|---|
| **log_writer** | `set_retention_days(int)` (хранение текущего значения; `<=0` = off, по образцу `set_max_mb`); `prune()` вызывается из `open_day()` при успехе и на `false` из `retention_select` не удаляет ничего. **R-1:** значение обновляется каждый server-tick (см. §20.4), а не только при старте. Отбор — `src/retention.{h,cpp}`. Запись/ротация/observer — без изменений. |
| **bootstrap** | В `register_cvars()` (H2-паттерн) зарегистрировать и закэшировать `hldsmon_retention_days` (value `"3"`, `FCVAR_ARCHIVE`); в `open_log()` — `c.log->set_retention_days(...)` перед `open()`. **R-5:** добавить в `CvarRefs` поле `retention_days`; хелпера `cvar_i` в коде **нет** — план обязан либо ввести `cvar_i()` (обёртка над `cvar_f`), либо читать `(int)cvar_f(c.cvars.retention_days, 3.0f)`. |
| **sampler (R-1)** | На каждом 1 Гц server-tick перечитать `hldsmon_retention_days` и вызвать `c.log->set_retention_days(...)` до `maybe_rotate()` (live-гейт по образцу `plugin_enabled()`), чтобы изменение cvar вступало в силу на ближайшей ротации. |
| **config §12** | + одна строка cvar (выше). |
| **README** | cvar-список (`hldsmon_retention_days`) + короткий абзац про retention. |
| **events / serialization / metrics** | Без изменений — новых типов JSONL нет (INV-5 не затрагивается). |
| **analyzer** | **Без изменений**: FTP-live читает то, что присутствует; удаление старых дневных файлов — ожидаемо (полнота истории не предполагается). |
| **Структура/сборка (R-3)** | Новые `src/retention.{h,cpp}` (host-testable, без движковых заголовков) + `tests/test_retention.cpp`. **Ловушка M1 (как в modnet):** `Makefile` собирает host-тесты из **фиксированного** `CORE_SRCS` — план **обязан** добавить туда `src/retention.cpp`, иначе `make test` не слинкует host-тесты retention. |
| **exports.whitelist / version_script.lds** | **Без изменений** — новых экспортов нет (всё `local: *`). |

**Выгода для O2 (§16):** retention ограничивает срок хранения PII (`ip`/`authid`/`steamid64`/`name`) сверху — `N` суток — детерминированным auto-удалением, снижая/закрывая O2.

### 20.8 Не-цели

- Не удаляются не-дневные файлы (маркер краша §9 и прочее) — никогда.
- Нет сжатия/архивации — только удаление.
- Нет квоты по объёму — только по возрасту (лимит размера/сутки остаётся за `hldsmon_max_mb`, §7).
- Не трогаются файлы за пределами `hldsmon_dir`.
- Нет принудительного prune из `hldsmon_status`/`hldsmon_flush_log` (см. §20.10 Q2).

### 20.9 Приёмочные качественные сценарии (QAS-R)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-R1 | `N=3`, файлы `T,T-1,T-2,T-3`: удаляются ровно `T-3` и старше; `T,T-1,T-2` остаются | `retention_select` | host-тест |
| QAS-R2 | Текущий файл (`d=T`) не удаляется ни при каком `N>=1` | предикат + guard `current_path()` | host-тест + FS-тест |
| QAS-R3 | `N=0` и `N<0`: не удаляется ничего | `retention_select` | host-тест |
| QAS-R4 | Не-дневные имена (маркер краша, `foo.jsonl`, `hldsmon-2026-13-40.jsonl`) не удаляются | `parse_log_date` | host-тест |
| QAS-R5 | `N=1`: после ротации остаётся только новый текущий файл | prune на `maybe_rotate` | host-тест (`test_log_writer`-стиль) |
| QAS-R6 | Prune на `open()` не падает при недоступном/пустом каталоге; ошибки считаются | `LogWriter::prune` | host-тест |
| QAS-R7 | `make`/`make test` зелёные; whitelist экспортов без изменений | Makefile | CI/локально |
| QAS-R8 (R-2) | `out_cap` меньше числа кандидатов ⇒ `retention_select` возвращает `false` ⇒ **ноль** `unlink`, без краха | fail-safe-контракт | host-тест |
| QAS-R9 (R-7) | Симлинк с дневным именем ⇒ `unlink` удаляет **ссылку**, не цель | `unlink` семантика | host-тест (FS) |
| QAS-R10 (R-7) | Файл, открытый читателем (FTP), удаляется безопасно: открытый fd продолжает читаться, inode живёт до `close` | POSIX unlink-while-open | host-тест (открыть fd → prune → читать) |
| QAS-R11 (R-7) | `hldsmon-crash.marker` (и прочие не-дневные имена) в каталоге ⇒ **никогда** не удаляется | `parse_log_date` + scope §20.3 | host-тест |

### 20.10 Решения владельца (вопросы закрыты)

- **Q1 (закрыт). Семантика `N<=0`.** `hldsmon_retention_days <= 0` ⇒ retention **выключен** (keep-all); ничего не удаляется. (Значение `0` при буквальном «at most N including current» невыполнимо, поэтому трактуется как «disabled».)
- **Q2 (закрыт). Prune по `hldsmon_flush_log`.** **Нет.** Prune выполняется **только** на `open()` и на суточной ротации; `hldsmon_flush_log` остаётся чистым `fflush`.
- **Q3 (закрыт). Retention во время «проблемного периода».** Отдельного флага паузы **нет**. Владелец повышает `hldsmon_retention_days` (или ставит `0`), и изменение вступает в силу **на ближайшей ротации** благодаря **R-1** (значение перечитывается каждый server-tick, §20.4).

### 20.11 План

Фазы, DoD и тесты retention (включая `retention_select`/`parse_log_date` и `prune` в `test_log_writer`) добавлены в `docs/plan.md` — раздел **«Стадия 3b — `retention` — план реализации»** (R-P0..R-P4).

### 20.12 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS; замечания R-1..R-8 сложены (folded) в этой ревизии §20.** Вопросы владельца §20.10 закрыты.

| ID | Severity | Учтено |
|---|---|---|
| R-1 | Med | §20.4 + §20.7 (sampler): `hldsmon_retention_days` перечитывается каждый 1 Гц server-tick и обновляет `LogWriter` до `maybe_rotate()`; изменение вступает в силу на ближайшей ротации. Q3 переписан (§20.10) — пауза через cvar, а не флаг. |
| R-2 | Med | §20.5: `retention_select` → `bool` + `size_t* out_n`; при переполнении `out_cap` — `false` и ноль удалений (без `SIZE_MAX`); ФС-вызывающий при `false` не делает `unlink`. QAS-R8. |
| R-3 | Med | §20.7 (Структура/сборка) + план R-P0: `CORE_SRCS += src/retention.cpp`, `tests/test_retention.cpp` — иначе `make test` не слинкует ядро retention. |
| R-4 | Low | §20.5: `parse_log_date` проверяет день месяца с учётом високоса (`DD ∈ [1..дней_в_месяце]`). |
| R-5 | Low | §20.7 (bootstrap): `cvar_i` не существует — план вводит `cvar_i()` либо читает `(int)cvar_f(...)`; поле `retention_days` в `CvarRefs`. |
| R-6 | Low | §20.4 (R-6): prune — только server thread, открытие/ротация, не hot path; dir-I/O и bounded-аллокация списка там допустимы; INV-2 — только hot path. |
| R-7 | Low | §20.9: QAS-R9 (симлинк⇒удаление ссылки), QAS-R10 (unlink открытого файла безопасен на Linux), QAS-R11 (маркер краша не удаляется). |
| R-8 | Low | §20.3 (R-8): файл с будущей датой (сбитые часы) сохраняется бессрочно — безопасное направление, принято. |
