# HldsMonitoring — дизайн модуля сбора телеметрии ReHLDS (rev 4)

- **Статус:** rev 2 (база v1) — Reviewed, замечания сведены в §18; решения владельца зафиксированы в §16. **Стадия 3 (`modnet`)** — **rev 3 (modnet) — design re-reviewed after revision; residuals folded** (см. §19; ревью и остатки — §19.13); открытые вопросы §19.11 закрыты. **Стадия 3b (`retention`)** — **rev 4 (retention) — revised after oracle design-review (R-1..R-8 folded); owner questions §20.10 закрыты; план добавлен в `docs/plan.md`** (см. §20). **Стадия 4 (`ping-fix`)** — **rev 5 (ping-fix) — oracle design-review пройдено (PASS-WITH-REVISIONS; PG-1..PG-7 сложены, §21.12); план — сокращённый багфикс-трек.** **Стадия 5 (`config-context`)** — **rev 6 — oracle design-review пройдено (PASS-WITH-REVISIONS; CFG-1..CFG-8 сложены, §22.11); план — обычный gated-цикл.** **Стадия 6 (`host-metrics`)** — **rev 7 — oracle design-review PASS-WITH-REVISIONS (H1..M5 folded); повторное ревью — PASS; план ожидает momus.** **Стадия 7 (`client-quality`)** — **rev 8 — oracle PASS-WITH-REVISIONS (H1/H2/M1-M4 folded); D1/D2 РЕШЕНЫ автономно (D1: overflow вынесен; D2: окно по `loss`).** **Стадия 8 (`connect-timings`)** — **rev 9 — oracle PASS-WITH-REVISIONS (CT-1..CT-9 folded); Q1 РЕШЁН.** **Стадия 9 (`event-enrich`)** — **rev 10 — oracle PASS-WITH-REVISIONS (EN-1..EN-7 folded); EN-7/Q2 РЕШЕНЫ.** **Стадия 10 (`analyzer-ui`)** — **rev 11 — oracle PASS-WITH-REVISIONS (AU-1..AU-9 folded); Q1 РЕШЁН (JSON-конверт).** **Стадия 12 (`large-logs`)** — **rev 12 — oracle design-review PASS-WITH-REVISIONS (B1/B2, M1–M7, m1–m5 сложены, §28.11); серверный период/window в `/api/analyze` — обязательный scope (B1).** **Post-owner-answers (2026-10-13):** решения владельца сложены (§16 O4–O9 закрыты) — чтение и запись **без лимита по умолчанию** (лимит — опциональный клапан), память ограничена **стримингом**, window — **UX**, а не потолок; добавлены UI-требования (компактный «Период», опциональный sticky, удобная механика FTP-окна). **Повторное ревью (R2/R3) пройдено: PASS — design-gate ЗАКРЫТ (§28.11); plan-review `momus` — PASS-WITH-REVISIONS (все сложены), plan-gate ЗАКРЫТ. Стадия `large-logs` РЕАЛИЗОВАНА И ВЕРИФИЦИРОВАНА (2026-10-13): код поставлен (LL-P1..LL-P6), `make test` = 278 OK, e2e в реальном браузере на файле владельца (~775 МБ) — ок.** **Стадия 13 (`instant-period`)** — **rev 16 — design-gate ЗАКРЫТ** (oracle R1/R2 — PASS-WITH-REVISIONS, R3 — PASS; R3-m1/m2/m3 сложены, §29.11); **владельческие вопросы O10–O15 решены** (§16/§29.9); **plan-gate ЗАКРЫТ; стадия РЕАЛИЗОВАНА И ВЕРИФИЦИРОВАНА; релиз 0.4.1** (`docs/plan.md`, «Стадия 13 — `instant-period`»). Направление: мгновенный период по `handle` (сессия анализа + разрежённый индекс `time→byte-offset→run`, без повторной загрузки/полного разбора) + единый бакетный контракт `min/max/avg` (адаптивная бакетировка; `CHART_MAX_POINTS=2000`, `min_resolution=1 с`) с **отрисовкой по типу графика** (общий — `min`/`max`, per-metric — band+`avg`); расширения владельца — **селективные секции** (REQ-I10) и **двухуровневый таймлайн** (`DETAIL_TIMELINE_RATIO=0.05`, REQ-I11). Scope — `tools/analyzer` (`webui.py`/`hldsmon.py`/`webui/index.html`), схема P1/`src/` не трогаются; FTP-период не меняется, но форма рядов FTP — тройки.**
- **Decision state:** база v1 — архитектура выбрана (native Metamod collector, Linux-first, пассивный, файловое логирование с суточной ротацией). `modnet` — механизм выбран (**GOT/PLT interposition**, §19.3/§19.5); все развилки закрыты (§19.11), ревью — §19.13. `retention` — механизм выбран (**host-testable отбор** `retention_select` + prune на open/ротации, §20.2/§20.5); развилки — §20.10. `large-logs` — **решения владельца закрыты (2026-10-13)**: чтение/запись **без лимита по умолчанию** (опциональный клапан), память ограничена **стримингом** (диск-spool + оконный анализ), window/период — **UX**, не потолок; серверный период/window и coarse-overview (оконность ii-b) — обязательный scope (§28.3/§28.5); UI-требования (компактный «Период», опциональный sticky, механика FTP-окна). Вопросы §16 O4–O9 закрыты; повторное ревью — §28.11. `instant-period` (стадия 13) — **направление выбрано владельцем**: мгновенный период на локальном файле через серверную сессию по `handle` (индекс `time→byte-offset→run`, O(окна) по времени, без повторной загрузки/полного разбора; run-корректность окна) + единый бакетный контракт `min/max/avg` (адаптивная бакетировка) с режимом отрисовки на фронте + селективные секции + двухуровневый таймлайн + отрисовка по типу графика (§29.3); дизайн-ревью R1/R2 (PASS-WITH-REVISIONS) и R3 (PASS) пройдены, замечания сложены (§29.11); **design-gate CLOSED**; владельческие O10–O15 решены; **plan-gate CLOSED**; стадия реализована и верифицирована; релиз **0.4.1**.
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
- **Сборка:** HLDS 32-бит, стандарт **C++23**. Канонический тулчейн — образ `hun1er/oracle-7-cxx-build-env-gnu` (**GCC 16.2.0** на glibc **2.17**, пинован по digest `sha256:4e3334c1…`), используется и в CI, и для деплой-бинарника; статический линк libgcc/libstdc++ + `_GLIBCXX_USE_CXX11_ABI=0` ⇒ максимум `GLIBC_2.17`. Локально — `make` с GCC ≥ 11 + multilib (`gcc-multilib`), только для разработки/тестов. `cmake`/`ninja` нет (сборка Makefile).

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
                                             ▼  addons/hldsmon/logs/hldsmon-YYYY-MM-DD.jsonl
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
`ClientSample`: `t_ms`, `gen` (connect-time/SteamID), `ping` (мс), `packet_loss` (raw + Δ), `choke` (raw + Δ), `delta_seq`, `in_seq/in_ack/out_seq/rel_seq`, `rel_fill_pct` (`reliable_length` к `MAX_MSGLEN`), `rate`, `cleartime`, `state_bitmask` (4 booleans), `fragbuf_total`, `incomingready_mask`, `flow_out/in_kbps`, `flow_out/in_avg`, `msg_flags`.
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
- **Файл:** `logs/hldsmon-YYYY-MM-DD.jsonl` (UTC), директория `hldsmon_dir` (default `addons/hldsmon/logs/`; fallback `/tmp/hldsmon` с предупреждением).
- **Ротация:** при смене UTC-даты на 1 Гц-тике LogWriter закрывает старый и открывает новый файл.
- **Буферизация:** `FILE*` append + `fflush` раз в `hldsmon_flush_sec` (default 1 с). Потеря при краше — ≤ ~1 с. Данные в page cache ОС переживают краш процесса.
- **Права:** файл `0600`, директория `0700`. Лимит размера/сутки — **опциональный клапан** `hldsmon_max_mb` (default **`0`=unlimited**; см. §28, решение владельца 2026-10-13): при выключенном клапане запись не ограничивается (риск disk-fill — осознанный выбор владельца); при `>0` — предупреждение/остановка как раньше.
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
| `hldsmon_dir` | cvar | директория логов | `addons/hldsmon/logs` |
| `hldsmon_max_mb` | cvar | опциональный лимит размера/сутки; `0`=unlimited (см. §28) | 0 |
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

- **Сборка:** Makefile, `-std=c++23 -m32 -fPIC -shared`, `-Wall -Wextra -Werror`, `-ldl`, `version_script.lds` (экспорт `GiveFnptrsToDll/Meta_*/GetEntityAPI2/GetNewDLLFunctions/GetEngineFunctions`). Канонический тулчейн — oracle-образ (GCC 16.2.0, glibc 2.17, пинован по digest).
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

### Стадия 12 (`large-logs`) — **закрыто владельцем (2026-10-13)**; гейт реализации открыт по решениям ниже (детали — §28; B1/B2, M1–M7, m1–m5 учтены — §28.11)
- **O4 (a, локальный приём) — РЕШЕНО.** Поток тела → spool; отказ от `f.text()` (прямой `Blob`; BOM/CRLF) — §28.3 п.3.
- **O5 (b, FTP) — РЕШЕНО.** Сегментный spool; **без cap по умолчанию**; улучшенная/очевидная механика окна (REQ-L6) — §28.3 п.4/п.10.
- **O6 (c, лимит) — РЕШЕНО.** Чтение **без лимита по умолчанию** + опциональный клапан (`--max-log-mb` default `0`); продюсер `hldsmon_max_mb` default **`0`=unlimited** (опц. клапан; риск **disk-fill** — осознанное решение владельца) — §28.3 п.7/п.9.
- **O7 (d, security/spool) — РЕШЕНО.** Spool на диске, budget как ресурсный предохранитель, crash-safe cleanup; loopback-дефолт; безлимит на не-loopback — opt-in; риск disk-fill задокументирован — §28.3 п.13/п.14.
- **O8 (e, сигнал о лимите) — РЕШЕНО.** По умолчанию отказа/обрезки нет (cap не отклоняет); при включённом клапане — единый `{"code":"log_too_large"}` — §28.3 п.8.
- **O9 (scope/B1) — РЕШЕНО.** Читается **всё**; window/период — **UX**, не потолок; полный день обрабатывается (coarse overview + окна по требованию) — §28.3 п.1.
- **Новых открытых вопросов нет.** UX-детали (высота «Период», layout FTP-окна, вид прогресса) — на этапе плана, решения владельца не требуют (§28.9 п.7). Единственное явно остающееся «на владельце» — принятие риска disk-fill при выключенном клапане (зафиксировано выше).

### Стадия 13 (`instant-period`) — **ЗАКРЫТО владельцем**; design-gate **CLOSED** (R1/R2 — PASS-WITH-REVISIONS, R3 — PASS; §29.11); scope: мгновенный период по `handle` + бакетный min/max/avg + **отрисовка по типу графика** + **селективные секции** + **двухуровневый таймлайн**; детали — §29. **Открытых вопросов нет.**
- **O10. Гранулярность и cap индекса `time→byte-offset→run` — РЕШЕНО.** `INDEX_STEP_S=5`, `INDEX_STEP_BYTES=1 МиБ`, `INDEX_MAX_ENTRIES=20000` (удвоение шага при cap); §29.3 п.1, INV-I1.
- **O11. Целевое число точек и базовая резолюция бакета — РЕШЕНО.** `CHART_MAX_POINTS=2000`; `min_resolution=1 с`; §29.3 п.7, INV-I10/QAS-I17.
- **O12. Отрисовка графиков — РЕШЕНО (разная по типу графика).** Backend всегда отдаёт `{min,max,avg,n}`; **общий «Все датасеты»** — только `min` **или** `max` (тоггл, default `max`, band запрещён); **per-metric** — `min–max` band + линия `avg`; персист `hldsmon.ui.*`; §29.3 п.8, REQ-I5.
- **O13. Размер memo-кэша окон — РЕШЕНО.** LRU = **8** записей на handle, ключ `(from,to,window,sections)`; §29.3 п.6.
- **O14. Дефолт детального таймлайна — РЕШЕНО.** Автопоказ при `span(window) < DETAIL_TIMELINE_RATIO·span(fullRange)`, `DETAIL_TIMELINE_RATIO = 0.05`; иначе только главный; вложенный зум; §29.3 п.12, REQ-I11.
- **O15. Дефолтный набор секций UI — РЕШЕНО.** Default = **все секции**; выбор блоков персистится **глобально** в `localStorage` (`hldsmon.ui.*`); §29.3 п.11, REQ-I10.

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

---

## 21. Ping — коррекция единиц `latency` (Стадия 4, багфикс)

> Небольшая корректирующая стадия. Имя стадии — **«Стадия 4 — ping-fix»**; не путать с дорожной картой §17. Дизайн-ревью — `oracle`; план-ревью — `momus`; журнал стадии — `docs/journal.md`.

### 21.1 Контекст и проблема

Владелец сообщил: per-client `ping` в логах **всегда `0`** у всех. Тем самым бесполезны поле `ClientSample::ping` (§6.2, мс), встроенное в каждую `client`-запись и в `drop.window[]`, и производное `drop.neighbors.avg_ping` (§6.3). Цель — починить значения без изменения схемы JSONL (INV-5), без новых cvar'ов и без изменения публичного контракта.

### 21.2 Проверенные технические факты (ground truth)

Источник: `rehlds/ReHLDS` (`rehlds/engine/pr_cmds.cpp`, `sv_user.cpp`, `sv_main.cpp`, `rehlds/engine/server.h`, `rehlds/common/net_api.h`); сверено с вендоренными заголовками проекта.

- **`client_t::latency` — `float`, в секундах.** Подтверждено движком: `PF_GetPlayerStats` делает `*ping = client->latency * 1000.0;`, а `net_api.h` документирует поле как «Latency, in seconds (multiply by 1000.0 to get milliseconds)». `SV_CalcClientTime`/`SV_ComputeLatency` кладут в `latency` среднее `frame->ping_time` (секунды).
- **`client_t::packet_loss` — `float`, процент `0..100`** (клиентский `packetLossByte & 0x7F`), не доля `0..1` и не счётчик. Текущий `cs.loss = static_cast<uint32_t>(ct->packet_loss)` **корректен** — не трогаем.
- **Причина бага:** `src/sampler.cpp:101` делает `cs.ping = static_cast<uint16_t>(ct->latency);` — усечение секунд до `uint16_t` даёт `0` при любом реалистичном ping (`0.040 → 0`). Симметрично `src/hooks.cpp:260` суммирует `o->latency` (секунды) в `rec.neighbors.avg_ping` (поле в мс) ⇒ тоже фактически `0`.
- **`IGameClient::GetLatency()`** существует в upstream-API ≥ 3.10 (коммит `f3d6024`) и возвращает **те же секунды**; вендоренный `include/cssdk/engine/rehlds_interfaces.h` его не содержит, хотя `rehlds_api.h` объявляет `REHLDS_API_VERSION_MINOR 15` ⇒ вендоренный интерфейс **устарел (pre-3.10)** — отдельная нить (21.7), не в scope фикса.
- **`pfnGetPlayerStats(const edict_t*, int* ping, int* packet_loss)`** (`include/cssdk/engine/eiface.h:259`) отдаёт `ping` в **мс** напрямую; объявлен, но в коде **не используется** — альтернативный источник (21.4).
- **`0` у реального клиента легитимен:** ReHLDS обнуляет `frame->ping_time` первые ~2 с после коннекта; `SV_CalcClientTime` возвращает `0` при разбросе last-4-frame > 0.2 с; `fakeclient`/боты `latency` не имеют вовсе. Это не регресс и отдельного флага не требует (21.8).

### 21.3 Решение (минимальный фикс)

Единый host-testable **инлайн-конвертер** в `src/metrics.h` (рядом с `gate_idle_flow`, без движковых заголовков), применяемый в обеих точках:

```cpp
// ReHLDS client_t::latency — В СЕКУНДАХ (float). Конвертирует в округлённые
// миллисекунды для uint16-полей ping. Неположительное/неизвестное (в т.ч. NaN) -> 0.
inline uint16_t ping_ms_from_latency(float latency_s) noexcept {
    const double ms = static_cast<double>(latency_s) * 1000.0;
    if (!(ms > 0.0)) {           // ловит <=0, -inf и NaN (PG-1)
        return 0;
    }
    const double r = ms + 0.5;   // округление half-up
    if (r >= 65535.0) {          // clamp по ОКРУГЛЁННОМУ значению (PG-2)
        return 0xFFFFu;
    }
    return static_cast<uint16_t>(r);
}
```

`!(ms > 0.0)` (а не `ms <= 0.0`) намеренно отсекает **NaN** (PG-1); clamp выполняется над **округлённым** `r`, а не над `ms`, чтобы не получить вне-диапазонное преобразование в `uint16_t` на краю `~65.535 с` (PG-2).

- `src/sampler.cpp:101` → `cs.ping = ping_ms_from_latency(ct->latency);`
- `src/hooks.cpp` (сбор `neighbors`) → `sum_ping += static_cast<double>(ping_ms_from_latency(o->latency));` (сумма в мс), `avg_ping` остаётся средним — теперь в мс.

Округление `+0.5` (без `<cmath>`), clamp в `0xFFFF` (секунды сверх 65.5 с «залипшего» значения не переполняют поле). Источник данных не меняется: остаётся доступ через `engine_layout` (`client_by_id`) с уже действующей runtime-пробой (INV-4). Побочно фикс распространяется на `drop.window[]` (там те же `ClientSample`).

### 21.4 Альтернативы

| Вариант | Плюсы | Минусы / почему не выбран |
|---|---|---|
| **A. Конвертер в месте чтения (выбран)** | Минимальный; host-testable; сохраняет существующий ABI-путь; без новых внешних зависимостей | Остаётся зависимость от раскладки `client_t` (уже покрыта runtime-пробой) |
| B. `pfnGetPlayerStats(ct->edict, &ping, &loss)` | Возвращает мс напрямую; каноничный путь игровых DLL | Меняет источник истины; `edict`-ориентирован; для не-полностью-подключённых клиентов требует аккуратности `NUM_FOR_EDICT`; большее поведенческое изменение на багфиксе; возвращает мс **усечением** (не округлением) и всё равно читает `client_t` (`ct->edict`) — зависимость от раскладки не снимается (PG-7) |
| C. `IGameClient::GetLatency()` | Публичный API, без зависимости от раскладки | Требует обновления устаревшего вендоренного заголовка (отдельная стадия); всё равно возвращает **секунды** — конвертер нужен в любом случае |

Выбран **A**: правка минимальная (bugfix-правило), тестируемая на хосте, и не тянет за собой пере-вендоринг заголовков.

### 21.5 Влияние на требования, контракты и инварианты

- **Требования.** Уточняет **G1/G2** (корректные per-client метрики и снимок дропа); новых требований не вводит. Non-goals §2 сохраняются.
- **Контракты.** Схема JSONL (`docs/plan.md`, Приложение P1) и `exports.whitelist`/`version_script.lds` — **без изменений**. Ключи `ping`/`avg_ping` сохраняют имена и типы; меняется только корректность значений.
- **Инварианты.** INV-1..INV-6 без изменений. Фикс не трогает hot-path-аллокации (INV-2 — чистые вычисления на стеке), не меняет потоковую модель (INV-3). **INV-4:** пробу не меняет, но корректность фикса **наследует** её — `latency`/`packet_loss` читаются через `engine_layout`/`client_by_id`; принимается, что runtime-проба (design §18, Приложение B) уже покрывает раскладку `client_t` достаточно, чтобы выявить сдвиг, затрагивающий `latency` (PG-3).
- **Нейминг.** В `docs/design.md` §6.2 поле названо `ping_ms`; канонично — `ping` (P1, `docs/plan.md`). Считать `ping` авторитетным (это только документное расхождение §6.2, правится в план-фазе).

### 21.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **metrics.h** | Добавить host-testable `ping_ms_from_latency()` (заголовок, без движковых зависимостей). |
| **sampler** | `src/sampler.cpp:101` — заменить каст на `ping_ms_from_latency(ct->latency)`. |
| **hooks** | `src/hooks.cpp` — сбор `neighbors`: суммировать `ping_ms_from_latency(o->latency)` (мс). |
| **serialization / events** | **Без изменений** — ключи и типы прежние. |
| **analyzer / webui** | **Без изменений** — читают те же ключи `ping`/`avg_ping`. |
| **engine_layout / bootstrap / log_writer / control** | **Без изменений.** |
| **Структура/сборка** | Новый host-тест `tests/test_ping.cpp` (включает только `metrics.h`; новый `.cpp` ядра не нужен). **Изменений Makefile НЕ требуется (PG-4):** `TEST_SRCS := $(wildcard tests/test_*.cpp)` подхватывает файл автоматически (прецедент — `tests/test_gate_idle_flow.cpp`); `src/metrics.cpp` отсутствует, хелпер — inline в заголовке. |
| **docs** | §6.2/§21 (нейминг `ping`); README — уточнение, что `ping` в мс (опционально). |

### 21.7 Открытые нити (вне scope фикса)

- **NT-1 (устаревший вендоренный интерфейс).** `include/cssdk/engine/rehlds_interfaces.h` не содержит `GetLatency()/GetPacketLoss()/GetChokeCount()/GetDeltaSequence()`, хотя `rehlds_api.h` заявляет API 3.15 (методы добавлены в 3.10). Это латентный дефект вендоринга (INV-4-риск) — **отдельная стадия** «re-vendor ReHLDS interfaces», не часть ping-фикса. Рекомендация: сверить/обновить пару `rehlds_api.h`+`rehlds_interfaces.h` до согласованной версии ≥ 3.10 и зафиксировать коммит в `include/VENDORED.md`.
- **NT-2.** Использование `pfnGetPlayerStats` (вариант B) как будущее упрочнение — не требуется для корректности; оставлено как возможная альтернатива при ре-вендоринге.

### 21.8 Не-цели

- Нет нового поля для различия «бот/не измерено» и «ping реально 0» — `ping=0` трактуется как «неизмерено» (документировано); новое поле — не багфикс.
- Не меняются `packet_loss`/`loss`/`dloss` (уже корректны).
- Не вводятся cvar'ы.
- Не выполняется ре-вендоринг заголовков (NT-1).
- Не меняется схема P1.

### 21.9 Приёмочные качественные сценарии (QAS-P)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-P1 | `0.040f→40`; `0.0f`/отрицательное/**`NaN`**→`0`; `65.535f` и `70.0f`→`65535` (clamp по округлённому); краевой `65.5347f→65535`; `0.0005f→1` | чистая функция | host-тест (`tests/test_ping.cpp`) |
| QAS-P2 | `sampler` вызывает `ping_ms_from_latency(ct->latency)` (не сырой каст) | конвертер в `sampler` | review-only (логика конвертера покрыта QAS-P1) |
| QAS-P3 | `drop.neighbors.avg_ping` — сумма мс по соседям (среднее в мс) | конвертер в `hooks` | review-only (логика конвертера покрыта QAS-P1) |
| QAS-P4 | Схема JSONL/ключи/типы и whitelist экспортов без изменений; `make`/`make test` зелёные | без изменения контракта | CI/локально |
| QAS-P5 | Бот/first-2s/высокий разброс → `ping=0` (ожидаемо, не регресс) | семантика движка | документировано; не тестируется как «должно быть >0» |

### 21.10 Вопросы

Открытых вопросов, блокирующих стадию, нет. Решения зафиксированы автономно:
1. **Fix A (конвертер в месте чтения)** — минимально и host-testable (§21.3/§21.4).
2. **Побочный `avg_ping`** — чинится тем же конвертером (§21.3).
3. **`ping=0` = «неизмерено»** — документировано, без нового поля (§21.8).
4. **NT-1 (ре-вендоринг заголовков)** — вынесено в отдельную стадию, не блокирует (§21.7).

### 21.11 План

Фазы, DoD и тесты ping-фикса добавляются в `docs/plan.md` — раздел **«Стадия 4 — `ping-fix` — план реализации»**. План обязан: (a) ввести `ping_ms_from_latency` в `metrics.h`; (b) поправить `sampler.cpp`/`hooks.cpp`; (c) добавить `tests/test_ping.cpp` (подхватывается `wildcard` автоматически, Makefile **не** трогаем); (d) прогнать `make` и `make test`; (e) **не** менять схему P1/экспорты. Согласно рекомендации ревью (PG-трек) — **сокращённый багфикс-трек**: отдельное независимое plan-ревью (`momus`) для этой стадии **не требуется** (нет развилок/схемы/ABI).

### 21.12 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS (лёгкий); plan-гейт может быть закрыт.** Минимальный фикс признан корректным и полным: `latency` (секунды) читается ровно в двух местах, оба исправлены; иных утечек секунд в мс-поля нет (`serialization.cpp` эмитит существующие целое/float без конверсии; `NetchanSnapshot` поля `latency` не имеет; `window[]` чинится транзитивно). Замечания — микро, сложены в этой ревизии. Critical/High нет, блокирующих вопросов владельцу нет.

| ID | Severity | Учтено |
|---|---|---|
| PG-1 | Low | §21.3: `if (!(ms > 0.0))` вместо `ms <= 0.0` — отсекает **NaN** (иначе `static_cast<uint16_t>(NaN)` — UB). |
| PG-2 | Low | §21.3: clamp по **округлённому** `r = ms + 0.5`, а не по `ms` — исключён вне-диапазонный каст на краю `~65.535 с`. |
| PG-3 | Med | §21.5: INV-4 переформулирован — проба не меняется, но корректность фикса **наследует** покрытие раскладки `client_t`. |
| PG-4 | Low | §21.6/§21.11: явно указано — изменений `Makefile`/`CORE_SRCS` **не требуется** (`TEST_SRCS` — wildcard). |
| PG-5 | Low | §21.9: QAS-P1 дополнен NaN/отрицательным/краевым clamp; QAS-P2/P3 помечены как review-only. |
| PG-6 | Low | §6.2: `ping_ms` → `ping` (устранён дрейф с каноническим P1). |
| PG-7 | Low | §21.4: к альтернативе B добавлено — возвращает мс усечением и не снимает зависимость от раскладки. |

**Рекомендация по треку (принята):** полный gated-цикл для этой стадии избыточен (≈3 строки + один host-тест, без развилок/схемы/ABI). Используется **сокращённый багфикс-трек**: данное oracle-ревью → сложение PG-1..PG-7 → реализация → гейт `make && make test` + запись в журнал; отдельное независимое plan-ревью `momus` **не проводится**.

**Не менять (fine):** каст `packet_loss` (`sampler.cpp:102`, процент 0..100 — корректен); отсутствие нового поля для «бот/не измерено» (`fake` уже различает); отсутствие cvar; схема P1/экспорты; `avg_ping` остаётся `float` мс.

---

## 22. Контекст запуска и rate-снимок (Стадия 5)

> Стадия владельца (запрос «Уровень 0»: контекст запуска и rate-снимок). Авторитетный дизайн — этот раздел; план — `docs/plan.md`, раздел «Стадия 5». Дизайн-ревью — `oracle`; план-ревью — `momus`.

### 22.1 Контекст и проблема

Диагностика отключений требует знать **конфигурацию сервера в момент инцидента**: лимиты `rate`/`timeout` и tickrate напрямую объясняют целый класс дропов (клиент просит `rate 30000` и `cl_updaterate 101`, а сервер задаёт `sv_maxrate 0`/`sv_minupdaterate` иначе; `sv_timeout` слишком мал для текущего jitter). Сейчас при дропе фиксируется **только** `sv_timeout` (§6.3, `drop.netchan`), а конфигурация запуска (версии движка/ReHLDS, `maxclients`, tickrate, rate-лимиты, карта) нигде не сохраняется и до инцидента не восстанавливается. Нужно: (a) разовый **boot**-снимок конфигурации; (b) снимок при **изменении** конфигурации; (c) **rate-подмножество в каждой drop-записи**.

### 22.2 Проверенные технические факты (ground truth)

- **Отслеживаемые cvar'ы (ASSUMPTION — подтвердить при реализации).** ReHLDS/HLDS: `sv_timeout`, `sv_maxrate`, `sv_minrate`, `sv_maxupdaterate`, `sv_minupdaterate`, `sv_maxcmdrate`, `sv_mincmdrate`; движковые `sys_ticrate`, `host_framerate`. Имена и **наличие** сверить по целевой сборке (ReHLDS API 3.14, §16 O3); отсутствующий cvar ⇒ значение "unknown" (не ошибка).
- **Чтение cvar.** `g_engfuncs.pfnCVarGetPointer(name)` → `cvar_t*` (NULL если не зарегистрирован); значение — `cvar_t::value` (**float** для всех перечисленных). Чтение на server thread, без аллокаций.
- **Версии.** `IRehldsApi::GetMajorVersion()/GetMinorVersion()` (версия-гейт уже используется, §3); номер сборки — при наличии getter в `RehldsFuncs_t`, иначе не пишем.
- **Карта / maxclients.** `current_map()` (`sampler.cpp`) и `c.maxclients` — уже доступны.
- **Совместимость.** Новый тип JSONL и поля — расширение канонической схемы (`docs/plan.md`, Приложение P1); `exports.whitelist`/`version_script.lds` не меняются.

### 22.3 Решение (design-level)

1. **Новый тип JSONL `config`** (INV-5: содержит `type` и `t`). Эмитится:
   - **boot:** один раз после успешного старта (после `open_log()`, на первом server-tick);
   - **change:** когда любой отслеживаемый параметр изменился — **поллинг на 1 Гц server-tick** и сравнение с **последним эмитированным** снимком (фиксированный POD, сравнение без аллокаций). Изменение вступает в лог в пределах ~1 с.
   - **Гейт `hldsmon_enable` и baseline (CFG-4).** Эмиссия подчинена общему гейту `plugin_enabled()` (как все записи). `last_config` в `PluginContext` несёт **флаг валидности**: на первом включённом тике baseline эмитится, даже если «настоящего» изменения не было (иначе конфиг на момент включения теряется); при `enable=0` `config` не пишется.
2. **Поля `config` (design-level):** `type`, `t`; версии `rehlds_major`/`rehlds_minor`; `map`; `maxclients`; `sys_ticrate`; `host_framerate`; `sv_timeout`; `sv_maxrate`; `sv_minrate`; `sv_maxupdaterate`; `sv_minupdaterate`; `sv_maxcmdrate`; `sv_mincmdrate`.
   - **Отсутствующий cvar (CFG-2).** В POD хранится **явный признак наличия** (`present`-битмаска; предпочтительнее NaN-сентинела, т.к. `append_fixed` уже отображает non-finite→`0`, а `-1.0` рискован для легитимных значений). В JSONL отсутствующий cvar пишется литералом **`null`** — для этого в сериализации добавляется **явный эмиттер `null`** (сейчас пути `null` нет). Правило `null` фиксируется в P1; анализатор `null` уже переживает (`_num`).
3. **`drop` — вложенный снимок `config`.** В `DropRecord` добавляется bounded POD `ConfigSnapshot` — **тот же подсет/эмиттер**, что и тип `config` (против дрейфа ключей, CFG-8). Числовые rate/timeout + tickrate читаются живьём при дропе (кэш `cvar_t*` в `CvarRefs`, чтение `->value`; без строкового lookup). Размер `DropRecord` (уже ~16 КБ из-за окна `window[64]`) вырастает незначительно (<0.5%); `static_assert(sizeof(DropRecord))` обновляется в плане. Это прямо закрывает запрос владельца «rate-снимок в drop». **`sv_timeout` дублируется** с существующим `drop.netchan.sv_timeout` (P1): `netchan.sv_timeout` сохраняется для обратной совместимости схемы, авторитетом считается `drop.config` (CFG-3).
4. **Сравнение снимков (CFG-5).** `ConfigSnapshot` перед заполнением **обнуляется** (`std::memset`, как у остальных записей) — иначе неинициализированные поля дадут ложное «изменение». Сравнение — по POD (memcmp/по полям), без аллокаций.

### 22.4 Альтернативы

| Вариант | Плюсы | Минусы / почему не выбран |
|---|---|---|
| **A. `config`-событие (boot+change) + снимок в `drop` (выбран)** | Полная картина: конфиг на таймлайне и локально в дропе; дроп самодостаточен | Дублирование rate-подмножества (несущественно: дропы редки) |
| B. Только `config`-событие, корреляция анализатором | Меньше изменений схемы | Дроп не самодостаточен; владелец явно просил снимок в drop |
| C. Отдельное событие на каждую смену cvar | Точные метки | Лишние записи/шум при частых изменениях; сложнее анализатор |
| D. Дамп всех cvar | «Всё под рукой» | Объём/шум; утечка ненужного; не-I/O-bound |

### 22.5 Влияние на требования, контракты и инварианты

- **Требования.** Расширяет **G1/G2** (контекст для триажа); новых N нет (см. §22.7).
- **Контракты.** Приложение P1 расширяется: новый тип `config` и вложенный `drop.config` (владелец схемы — фаза плана). `exports.whitelist`/`version_script.lds` — без изменений.
- **Инварианты.** INV-1 (только чтение); INV-2 (поллинг/сравнение на server-tick, фиксированные буферы, без аллокаций); INV-3 (server thread); INV-5 (`type`+`t`, валидный JSON); INV-4 не затрагивается.

### 22.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **bootstrap** | Кэшировать `cvar_t*` отслеживаемых cvar в `CvarRefs`; после `open_log()` инициировать boot-снимок; заполнить `ConfigSnapshot` (числовые + версии/map/maxclients). |
| **sampler** | На 1 Гц server-tick: собрать текущий `ConfigSnapshot`, сравнить с последним эмитированным, при отличии — эмитировать `config`; хранить `last_config`. |
| **hooks** | При дропе — заполнить `rec.config` (живой снимок rate/timeout). |
| **metrics** | Новый POD `ConfigRecord`; `ConfigSnapshot`; расширение `DropRecord` (+`config`). Обновить `static_assert` размеров. |
| **events / serialization** | `write_config_json` (no-alloc, host-testable); расширение `write_drop_json` вложенным `config`. |
| **control** | Опционально — показать текущий конфиг в `hldsmon_status`. |
| **analyzer / webui (CFG-1)** | **Обязательно** добавить `"config"` в `KNOWN_TYPES` (`tools/analyzer/hldsmon.py`) — иначе `iter_records` молча **отбросит** записи (парсер толерантен лишь в смысле «не падает»). Плюс фикстура `config` + расширение `types_ok.jsonl`, строка в `summary`/`digest`; UI — маркер/панель конфигурации (mирроринг прецедента modnet, plan M-P4). |
| **Makefile** | Без изменений (`metrics`/`serialization` уже в `CORE_SRCS`; новый тип — в существующих TU). |

### 22.7 Не-цели

- **Список загруженных Metamod-плагинов** — не в этой стадии (перечисление через Metamod-API добавляет связность; отдельная стадия при необходимости).
- **Дамп всех cvar** — нет, только релевантный набор (§22.3).
- **История каждого изменения cvar** — только отслеживаемый набор и только при фактическом изменении.
- **Пропуск быстрых изменений (CFG-6).** Изменение конфигурации с возвратом за <1 с не попадёт в лог; несколько изменений между тиками схлопываются в одну запись (последнее значение). `config` — контекст, а не аудит-лог.
- **Windows/внешний агент** — вне стадии.

### 22.8 Приёмочные качественные сценарии (QAS-C)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-C1 | Boot `config`-запись появляется при старте и содержит версии/map/maxclients/набор cvar; отсутствующий cvar → `null` | boot-снимок | host-тест сериализатора + smoke |
| QAS-C2 | Изменение `sv_maxrate` в рантайме ⇒ новая `config`-запись в пределах ~1 с | 1 Гц diff | host-тест diff-логики + smoke |
| QAS-C3 | Каждый `drop` содержит вложенный `config`-снимок с актуальными значениями | живой снимок при дропе | unit-тест сборки drop |
| QAS-C4 | Снимок/diff без аллокаций; размер `ConfigRecord` bounded (static_assert) | фиксированные POD | unit-тест размеров + тест «0 аллокаций» |
| QAS-C5 | Схема P1 расширена (тип `config`, `drop.config`); whitelist экспортов без изменений; `make`/`make test` зелёные | контракт/CI | CI/локально |

### 22.9 Вопросы

Открытых вопросов, блокирующих стадию, нет:
1. **Набор полей** — решён дефолтом (§22.3); расширяем при необходимости.
2. **Снимок в `drop`** — да (запрос владельца, §22.3 п.3).
3. **Список плагинов** — отложено (§22.7).

### 22.10 План

Фазы, DoD, расширение Приложения P1 (тип `config` + `drop.config`) и тесты добавляются в `docs/plan.md` — раздел **«Стадия 5 — `config-context` — план реализации»**.

### 22.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS.** Механизм (новый тип `config` + вложенный снимок в `drop`, поллинг 1 Гц + diff, кэш `cvar_t*`) признан корректным и не переусложнённым; замечания — о полноте/точности контракта, не о механизме. **CFG-1..CFG-8 сложены (folded) в этой ревизии §22; повторное ревью не требуется** (замечания механические и полностью адресованы — по образцу §20). Блокирующих вопросов владельцу нет.

| ID | Severity | Учтено |
|---|---|---|
| CFG-1 | High | §22.6: `config` **обязан** войти в `KNOWN_TYPES` (`tools/analyzer/hldsmon.py`) — иначе `iter_records` молча отбросит записи; добавлены фикстура `config`, `types_ok.jsonl`, строка `summary`/`digest` (mирроринг modnet M-P4). |
| CFG-2 | Med | §22.3 п.2: отсутствующий cvar — `present`-битмаска в POD + литерал `null` в JSONL; явный эмиттер `null` добавляется в сериализацию (пути `null` сейчас нет). |
| CFG-3 | Med | §22.3 п.3: дубль `sv_timeout` (`drop.config` vs `drop.netchan.sv_timeout`) зафиксирован — `netchan` для совместимости P1, авторитет — `drop.config`. |
| CFG-4 | Med | §22.3 п.1: гейт `hldsmon_enable` + флаг валидности `last_config` + baseline на первом включённом тике. |
| CFG-5 | Low | §22.3 п.4: `memset` снимка перед заполнением; сравнение по POD без аллокаций. |
| CFG-6 | Low | §22.7: задокументирована потеря изменений с возвратом <1 с и схлопывание нескольких изменений. |
| CFG-7 | Low | §22.2: верификация имён/наличия cvar и build-геттера перенесена в реализацию (журнал), прецедент modnet H4/H5. |
| CFG-8 | Low | §22.3 п.3: `config` и `drop.config` используют один эмиттер/подсет (против дрейфа). |

**Fine (не менять):** новый тип (а не перегрузка `server`); 1 Гц poll/diff; расширение `DropRecord` (не урезать до подмножества); кэш `cvar_t*` + live `->value`; путь через существующие `events`/`serialization`/`LogWriter`; без новых cvar/экспортов/файлов (`Makefile`/`CORE_SRCS`/`exports.whitelist` без изменений); набор cvars не расширять `sv_maxspeed`/`sv_lan`/`pausable`/`sv_region`.

**Взаимодействия:** retention (нет — `config` без PII, объём ничтожен), modnet (нет), Стадия 4 `ping-fix` (нет — общий только `build_drop_record`, разные поля).

**Трек:** обычный gated-цикл (новый тип в P1 + правки анализатора/UI + эмиттер `null`) — план → `momus` → реализация.

---

## 23. Хост/ядро-метрики (best-effort) (Стадия 6)

> Стадия владельца (запрос «Уровень 1»): «если сможем читать данные хоста — делаем; не уверен, что хостинг отдаст корректно». Дизайн — **best-effort с детекцией и деградацией**: собираем только осмысленное, помечаем неопределённость, никогда не падаем и не создаём ложный сигнал. Дизайн-ревью — `oracle`; план-ревью — `momus`.

### 23.1 Контекст и проблема

Метрики §6 описывают **игровой путь** HLDS, но не **хост**: переполнение UDP-приёмного буфера ядра (`RcvbufErrors`) — классическая причина отключений при низком `loss`; NIC-дропы (`rx_missed_errors`); **CPU steal** на VPS; залипание диска — сейчас невидимы. Сложность в том, что на разных хостингах (KVM-VPS vs LXC/OpenVZ/контейнер с общим netns) одни и те же `/proc`-файлы дают **разные по смыслу** значения; слепое чтение даёт ложный сигнал (данные хоста/соседа). Требование: собирать только то, что осмысленно, **помечать** неопределённость и деградировать.

### 23.2 Проверенные технические факты (ground truth)

Источники: `proc(5)`, `proc_net(5)`, `proc_stat(5)`, `proc_pid_io(5)`, `statvfs(3)`, документация ядра (SNMP/interface counters), исходники ядра (`net/ipv4/proc.c`). Per-metric вердикт:

| Источник | Что даёт | Namespace / доступ | Вердикт |
|---|---|---|---|
| `/proc/net/snmp` UDP `RcvbufErrors` | приёмный буфер сокета полон (`-ENOMEM`) — **главный сигнал**; `InErrors ⊇ RcvbufErrors` | per-netns; mode `0444`, без `CAP_*` | **USABLE**, CAVEATED при общем netns |
| `/proc/net/dev` | `rx_dropped`/`rx_missed_errors`/`overruns` (NIC ring) | per-netns; `0444` | **CAVEATED (M2)**: счётчики **зависимы от драйвера** — `0` часто означает «не сообщается», а не «нет дропов»; caveated наряду с netns-оговоркой |
| `/proc/stat` `steal` | время, «украденное» гипервизором | **не** namespaced; `0444` | **USABLE** только KVM; **0** в lxcfs/OpenVZ |
| `/proc/loadavg` | load | **не** namespaced | **USELESS** в контейнерах (нагрузка хоста); годится только на KVM |
| `/proc/self/io` `read_bytes`/`write_bytes` | байты к storage (writeback) | требует `CONFIG_TASK_IO_ACCOUNTING`; self — без прав | **CAVEATED** (конфиг/семантика/неатомарность) |
| `statvfs()` | `f_bavail` (не `f_bfree`) | — | **CAVEATED** (thin/quota переоценивает) |

- **Поля UDP зависят от версии ядра:** `IgnoredMulti` ~3.19+, `MemErrors` ~5.11+ (на CentOS 7 base 3.10 их нет) ⇒ **парсить по именам из header-строки, не по позиции** (прецедент: Linux 6.7 добавил `Ip OutTransmits` и сломал net-snmp).
- **Ось окружения (M3).** Значимая развилка — **выделенный хост** (bare metal/KVM VM, свой kernel ⇒ `/proc/net/*` и `/proc/stat`/`loadavg` авторитетны) vs **контейнер** (OS-виртуализация; при общем netns данные — агрегат хоста; lxcfs зануляет `steal`). «KVM» ≠ «контейнер»: `steal` непулевой ⇒ KVM (односторонний признак; `steal==0` не доказывает «не KVM» — может быть простаивающий KVM или bare metal).
- **Детекторы (дешёвые, без аллокаций):** контейнеризация — `/proc/self/cgroup`/`/proc/1/cgroup` (путь ≠ `/`, `docker`/`lxc`/`kubepods`), lxcfs-mount, `/.dockerenv`, `container=`; `io`-probe. **Правило netns (см. §23.3 п.2, H1) — исправлено.**
- `statvfs`/`/proc/*` требуют только чтения; `/proc/self/io` — без прав.

### 23.3 Решение (best-effort)

1. **cvar `hldsmon_host`**, default **0 (OFF)** — opt-in, консистентно с `hldsmon_modnet`; владелец включает после проверки на своём хостинге (**вопрос владельцу §23.9**).
2. **Детекция окружения (H1, исправлено).** Правило «`self`-inode == `/proc/1` inode ⇒ shared» **несостоятельно** (ложно срабатывает на выделенном хосте/KVM и на контейнере с приватным netns+PID-ns — напр. Docker по умолчанию — давая вывод «shared» почти всегда). Корректная постановка — отличать **выделенный хост** от **контейнера с общим netns**:
   - нет признаков контейнеризации (`cgroup`-путь `/`, нет lxcfs/`dockerenv`/`container=`) ⇒ выделенный хост/KVM ⇒ `/proc/net/*` **USABLE**;
   - контейнер + положительное подтверждение **приватного** netns ⇒ USABLE; иначе ⇒ **caveated** (безопасное направление).
   - чтение `/proc/1/ns/net` требует ptrace-доступа (не-root/`hidepid` ⇒ `EACCES`) — обрабатывать как «неизвестно ⇒ caveated», **не падать** и не выдавать «health». Детекция — **чистая host-testable функция** над входами (inode/флаги); I/O — в `host_metrics.cpp` (см. п.6).
3. **Сбор на 1 Гц server-tick:** `open`/`read`/`close` в **фиксированный** буфер (цикл до EOF/заполнения; `EINTR` — повтор; переполнение буфера ⇒ «частично» ⇒ caveated); разбор **по именам** (не по позиции), без аллокаций; файл недоступен ⇒ метрика «недоступна» (не ноль-как-здоровье).
4. **Семантика счётчиков (H2).** Все источники (`/proc/net/snmp`, `/proc/net/dev`, `/proc/stat`, `/proc/self/io`) — **кумулятивные**; в записи эмитятся **Δ за интервал** (по образцу modnet; raw — опционально). Предыдущий снимок — в `PluginContext` (фикс. POD, без аллокаций). **Первый сэмпл: Δ неизвестны ⇒ `null`** (не `0`). `steal` эмитится как **%** (`Δsteal/Δtotal·100`). Reset/wrap (пересоздание netns/перезапуск) ⇒ Δ не может быть отрицательным — обрабатывать.
5. **Одна JSONL-запись `host`** на интервал: поля метрик (**Δ**) + **маска достоверности** (`present`/`caveated`, M4). Caveat — **по-источниково** (netns-оговорка к `/proc/net/*`, драйверная — к net/dev), не глобально. `caveated` **≠ `null`**: значение присутствует, но помечено.
6. **Граница модуля (M1).** Обёртка чтения (`open`/`read`/`close`) и I/O детекции — в **engine-free** `src/host_metrics.{h,cpp}` (POSIX напрямую, как `modnet_got.cpp`), а **не** в `platform_linux` (иначе QAS-H2/H3 не host-testable: `platform_linux.h` тянет движковые заголовки). `platform_linux` **не трогаем**.
7. **Пассивность:** только чтение; никаких изменений системы. Эмиссия под общим гейтом `plugin_enabled()` (L1).

### 23.4 Альтернативы

| Вариант | Почему не выбран |
|---|---|
| Внешний агент читает `/proc` | Внешний артефакт; противоречит G4 (единый JSONL, passive) |
| eBPF/`softnet_stat` | root, внешний, сложность |
| Читать всё без пометок | Ложные выводы (данные хоста/соседа) — отвергнут |
| **Ин-процесс `/proc`, best-effort + флаги (выбран)** | Дёшево, passive, с честной неопределённостью |

### 23.5 Влияние на требования, контракты и инварианты

- **Требования.** Новый **G8** (host-контекст, best-effort) расширяет G1; новый **N11** (не атрибутировать при недостоверности). N1 (пассивность) сохраняется.
- **Контракты.** P1 расширяется типом `host` (владелец — план). Exports/`version_script` без изменений.
- **Инварианты.** INV-2 (фикс. буфер, без аллокаций), INV-3 (server thread), INV-5 (валидный JSON, `type`+`t`); INV-1 (только чтение).

### 23.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **`src/host_metrics.{h,cpp}` (новый)** | Host-testable парсеры (`parse_snmp_udp`, `parse_net_dev`, `parse_stat`, `parse_self_io`, **чистая детекция окружения**) **+ POSIX-I/O** (bounded `open`/`read`/`close`, `statvfs` по `hldsmon_dir` — M5); **engine-free** (POSIX напрямую, как `modnet_got.cpp`). |
| **platform_linux** | **Без изменений (M1)** — I/O вынесен в `host_metrics.cpp`, чтобы деградационные сценарии (QAS-H2/H3) остались host-testable. |
| **bootstrap** | Cvar `hldsmon_host` (0); детекция возможностей; кэш. |
| **sampler** | 1 Гц сбор + `host`-эмиссия. |
| **metrics** | POD `HostRecord` (+ маска доступности). |
| **serialization / events** | `write_host_json` (no-alloc; недоступное → `null`). |
| **analyzer / webui** | `KNOWN_TYPES += "host"` + ряды/панель (CFG-1-прецедент). |
| **Makefile** | `CORE_SRCS += src/host_metrics.cpp` (**ловушка M1** — иначе host-тесты не слинкуются). |
| **exports / version_script** | Без изменений. |

### 23.7 Не-цели

- **N11.** При недостоверных данных (shared netns, lxcfs-`steal`) значения **не** атрибутируются хосту; помечаются.
- Windows; внешний агент; активные действия; eBPF; per-process атрибуция (это `modnet`); `loadavg` в контейнерах (собирается только на KVM-детекте либо опускается).
- Обход `hidepid`/прав — `self`-файлы читаются без прав.

### 23.8 Приёмочные качественные сценарии (QAS-H)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-H1 | Разбор по именам устойчив к отсутствующим/новым полям UDP (fixture без `MemErrors`) | parse-by-name | host-тест `test_host_metrics` |
| QAS-H2 | shared netns ⇒ `/proc/net/*` помечены caveated, не атрибутируются | netns-детект | host-тест детекта + review |
| QAS-H3 | Отсутствие файла (нет IPv6/`io`) ⇒ деградация (флаг), не ноль-как-здоровье | graceful | host-тест |
| QAS-H4 | Сбор/сериализация без аллокаций; недоступное → `null` | фикс. буфер | тест «0 аллокаций» + сериализатор |
| QAS-H5 | `hldsmon_host=0` ⇒ ноль накладных, ни одной `host`-записи | гейт | review + host-тест |
| QAS-H6 | `make`/`make test`/`verify-exports` зелёные; whitelist без изменений | CI | CI/локально |
| QAS-H7 (H2) | Первый сэмпл ⇒ Δ = `null` (не 0); далее — корректные Δ; `steal` — % | Δ-семантика | host-тест |
| QAS-H8 (H1) | `EACCES` на `/proc/1/ns/net` или неопределённость ⇒ caveated, без краха | детектор окружения | host-тест детектора |

### 23.9 Вопросы

1. **Q1 (владелец).** Default `hldsmon_host`: **OFF** (рекомендация — opt-in; владелец проверяет на своём хостинге) vs ON. Ось окружения (M3): на **выделенном** хосте (bare metal/KVM) host-метрики авторитетны; в контейнере с общим netns — caveated. Дефолт зафиксирован OFF.
2. **Q2.** `loadavg`/`steal`: `steal` — да (признак KVM/выделенного хоста); `loadavg` — опустить (низкая ценность, M3).
3. **Q3.** Формат `host` — одна запись на интервал + **маска достоверности по-источниково** (M4).
4. **Q4 (зависимость).** `host` использует эмиттер `null` из Стадии 5 (`config`) — реализовывать после Стадии 5 либо включить эмиттер `null` в эту стадию (L5).

### 23.10 План

Фазы, DoD и расширение Приложения P1 (тип `host`) добавляются в `docs/plan.md` — раздел **«Стадия 6 — `host-metrics` — план реализации»**.

### 23.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS.** Механизм (best-effort чтение `/proc` + флаги достоверности + деградация + default OFF) признан верным. Существенные уточнения (H1/H2/M1..M5) сложены в этой ревизии §23; повторное ревью — в процессе (targeted). Блокирующий вопрос владельцу — только Q1.

| ID | Severity | Учтено |
|---|---|---|
| H1 | High | §23.3 п.2: детектор окружения переписан — по **контейнеризации** (cgroup/lxcfs/dockerenv), а не по `self==PID1` (правило было несостоятельно); `EACCES`/неопределённость ⇒ caveated; чистая host-testable функция. |
| H2 | High | §23.3 п.4: **Δ-семантика** (все счётчики кумулятивные); первый сэмпл ⇒ `null` (не 0); `steal` — %; reset/wrap. |
| M1 | Med | §23.3 п.6 + §23.6: POSIX-I/O и детекция — в engine-free `host_metrics.cpp`; `platform_linux` **без изменений**; QAS-H2/H3 host-testable. |
| M2 | Med | §23.2: `/proc/net/dev` → **CAVEATED** (драйверная зависимость `0` = «не сообщается»). |
| M3 | Med | §23.2/§23.9: ось «выделенный хост vs контейнер»; `steal` — односторонний признак. |
| M4 | Med | §23.3 п.5: маска достоверности **по-источниково**; `caveated` ≠ `null`. |
| M5 | Med | §23.6: `statvfs` — по каталогу `hldsmon_dir` (не `/`). |
| L1 | Low | §23.3 п.7: эмиссия под `plugin_enabled()`. |
| L3 | Low | §23.3 п.3: drain до EOF/заполнения, `EINTR`, truncation ⇒ caveated. |
| L5 | Low | §23.9 Q4: зависимость от эмиттера `null` (Стадия 5). |

**Fine (не менять):** пассивность (только чтение); default OFF (с исправленным обоснованием — opt-in + объём + избегание вводящих в заблуждение данных, а не «как modnet»); parse-by-name; «flag, don't attribute» (N11); `statvfs` `f_bavail` (не `f_bfree`); отсутствие новых экспортов; `KNOWN_TYPES += "host"`.

**Взаимодействия:** retention (объём +1 строка/с, без PII), modnet (оба 1 Гц-эмиттеры, без конфликта), ping-fix (нет).

---

## 24. Качество клиентской сети (Стадия 7)

> Стадия владельца (запрос «Уровень 2»): уточнить per-client сигналы качества (jitter, скользящий loss-rate, явный `reliable_overflow`). Дизайн-ревью — `oracle`; план-ревью — `momus`. **Зависит от Стадии 4** (`ping`) и опирается на §6.2.

### 24.1 Контекст и проблема

Набор §6.2 даёт per-client `ping`/`loss`/`choke`/`rel_fill`, но не различает **транзиентный сетевой спайк** и **устойчиво плохой линк**: нет **jitter** (разброса межпакетных интервалов), нет **скользящего** loss-rate (только кумулятивный Δ), и `reliable_overflow` не вынесен явным флагом (`msg_flags`/`rel_fill` кодируют его неявно). Нужны дешёвые, честные сигналы, вписанные в бюджет `ClientSample` (QAS-4, `<256`).

### 24.2 Проверенные технические факты (ground truth)

- `PreprocessPacket(uint8*, unsigned int, const netadr_t&)` вызывается **на каждый входящий пакет** (server thread, §5); `netadr_t` даёт IP источника. Есть пул активных клиентов (≤ `maxclients`) с кэшированным IP (`ident[]`), т.е. IP→слот разрешим.
- `netchan.message.flags` содержит `SIZEBUF_OVERFLOWED`; `rel_fill` (`reliable_length`/`MAX_MSGLEN`) уже собирается.
- **Бюджет `ClientSample` (QAS-4):** сейчас 247 Б, лимит `<256` ⇒ свободно ~8 Б. Новые поля обязаны уложиться.
- Клиентское кольцо (`hldsmon_client_ring`) уже хранит историю сэмплов (`in_seq`/`dloss`/…), т.е. скользящее окно выводимо **на этапе чтения** без новых полей.

### 24.3 Решение

1. **Jitter (межпакетные интервалы) — 3 поля в `ClientSample`.** На каждый входящий пакет клиента — Δ относительно предыдущего пакета того же клиента; копим `min/sum/max/count` за интервал (целочисленно, O(1), без аллокаций). В сэмпл — `jit_min_ms`/`jit_avg_ms`/`jit_max_ms` (**3×uint16 = 6 Б**; `count` не эмитим). **Часы — движковый `funcs->GetRealTime()`** (не wall-clock: NTP-скачки исказят jitter; M4). **Сброс — per-client, сразу после снятия сэмпла этого клиента** (не «глобально на интервале»: сэмплирование staggered, `kClientsPerTick=8`; M3), **под `client_hz>0`** (иначе аккумуляторы не сбрасываются) и **на `ClientConnected`/смене `gen`** (M4). **Clamp в `uint16`** (интервал >65.5 с; L1); `avg = sum/count` из `uint32`. `count==0`/первый пакет ⇒ поля `0` трактуются как «нет данных» (задокументировать; L1).
2. **IP→слот — bounded, O(maxclients).** В `PreprocessPacket` источник сопоставляется с активным клиентом. **Матч по `IP`+`port`** (только по IP ⇒ мисатрибуция при общем NAT/нескольких клиентах с одного IP; M1). Сравнение — по **упакованному `uint32` IP + `uint16` порт** в `ClientRuntime` (`ident[].ip` — `char[16]`, целочисленное сравнение по строке невозможно; M1). **Гейт по `active`/генерации слота** (защита от stale-IP; M1). `≤32` сравнений/пакет (GoldSrc `MAX_CLIENTS==32`, integer-only). Хот-пас: единственное добавление на путь пакета; альтернатива — расширить существующую `ip_table` (IP→слот) при необходимости.
3. **`reliable_overflow` — вне scope Стадии 7 (D1 РЕШЕНО).** `SIZEBUF_OVERFLOWED` (`netchan.message.flags`) транзиентный (`SZ_Clear` сбрасывает его при перестроении сообщения), поэтому надёжный сигнал требует наблюдения на исходящем пути (возможно, новый хук `SV_CreatePacketEntities`) — hook-поверхность без реального сервера не меняется. Поля `state`-бит/`drop.overflow`/`last_overflow_s` **не вводятся**; отдельная будущая опция владельца.
4. **Скользящий loss-rate — без новых полей (уточнено H2, D2 РЕШЕНО).** `client_t::packet_loss` — `float` **процент 0..100** (§21.2), **не кумулятивный счётчик**; `client_series` (`_SERIES_KEYS`) **не содержит `in_seq`**. Поэтому «скользящий loss» — **окно по уже процентному `loss`** в анализаторе (без `in_seq`); плагин не меняем (рекомендация (a) D2).
5. **Сериализация:** `emit_client` + поля jitter (`jit_min_ms`/`jit_avg_ms`/`jit_max_ms`). `drop`/`state` по overflow не меняются (D1).

**Бюджет:** 247 + 6 (jitter) = **253 < 256** ✅ (`static_assert` остаётся `<256`).

### 24.4 Альтернативы

| Вариант | Плюсы | Почему не выбран |
|---|---|---|
| **min/avg/max + bit + анализаторное окно (выбран)** | Укладывается в бюджет; минимум hot-path; окно без новых полей | — |
| Гистограмма/p99 jitter | Точнее хвост | Гистограмма (≥8 бакетов) не влезает в бюджет; отложено |
| Новое поле `loss_rate` в сэмпле | Самодостаточно | Тратит бюджет и объём; окно уже выводимо |
| Отдельный `clientq`-тип | Ноль изменений в `ClientSample` | Удваивает per-client объём/сложность; не нужно |

### 24.5 Влияние на требования, контракты и инварианты

- **Требования.** Уточняет **G1** (качество линка); новых G нет.
- **Контракты.** `ClientSample` + jitter (P1 `client`). Поля overflow не вводятся (D1). `exports` без изменений.
- **Инварианты.** INV-1 (только чтение), **INV-2** (jitter — O(1) целочисленно; IP→slot ≤maxclients), INV-3 (server thread), INV-5.

### 24.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **hooks (`PreprocessPacket`)** | IP+слот-матч ≤maxclients; накопление jitter Δ. |
| **sampler** | Сброс jitter-аккумуляторов после сэмпла; заполнение полей. |
| **metrics (M2)** | `ClientSample` +`jit_min/avg/max_ms` (253 Б < 256 ✅). **Ripple:** `DropRecord` встраивает `window[64]` ⇒ рост на 6×64=384 Б; обновить `static_assert(sizeof(DropRecord))` (`metrics.h`), `tests/test_serialization_events.cpp`, `tests/test_config.cpp`; проверить запас `kLineBufSize` (64 КБ). |
| **serialization / events** | Поля jitter в `client`. |
| **analyzer / webui** | Скользящий loss-rate из `client_series`; графики jitter. |
| **Makefile/exports** | Без изменений. |

### 24.7 Не-цели

- p99/гистограмма jitter (отложено); клиентская телеметрия; активные действия; отдельный `clientq`-тип; изменения `ping` (Стадия 4).

### 24.8 Приёмочные качественные сценарии (QAS-CQ)

| ID | Сценарий | Механизм | Проверка |
|---|---|---|---|
| QAS-CQ1 | jitter min/avg/max корректны на синтетических inter-arrival | аккумулятор | host-тест |
| QAS-CQ2 | ~~`overflow`-бит взводится при `SIZEBUF_OVERFLOWED`; `last_overflow_s` в `drop`~~ — **снят: D1 (вне scope)** | — | — |
| QAS-CQ3 | `sizeof(ClientSample) < 256` (QAS-4) | static_assert | compile |
| QAS-CQ4 | Скользящий loss-rate корректен на серии | анализатор | python-тест |
| QAS-CQ5 | Без аллокаций; `make`/`make test`/`verify-exports` | CI | CI/локально |

### 24.9 Вопросы

1. **D1 — РЕШЕНО (автономный дефолт, обратимо).** `reliable_overflow` **вынесен из scope Стадии 7**: флаг транзиентный, надёжный сигнал требует наблюдения на исходящем пути (возможно, новый хук `SV_CreatePacketEntities`) — не менять hook-поверхность без реального сервера. Отдельная будущая опция владельца.
2. **D2 — РЕШЕНО.** Скользящий loss — **окно по существующему процентному `loss`** (a) в анализаторе; новый кумулятивный счётчик не вводится (бюджет `ClientSample` сохранён).
3. **Q1.** p99/гистограмма jitter — отложено (бюджет, §24.4).
4. **Q3.** IP→slot матч — IP+port, packed, ≤maxclients (выбрано; откат к `ip_table` при необходимости).

### 24.10 План

Фазы/DoD/тесты — `docs/plan.md`, раздел «Стадия 7 — `client-quality` — план реализации». P1: `client` + jitter; скользящий loss в анализаторе; overflow — вне scope (D1).

### 24.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS. Plan-гейт НЕ закрыт** — до сложения H1/H2 (сложены в этой ревизии) и **ответа владельца по D1** (детекция `reliable_overflow`). H1/H2/M1-M4/L1-L3 сложены; блокирует только D1.

| ID | Severity | Учтено |
|---|---|---|
| H1 | High | §24.3 п.3: детекция `SIZEBUF_OVERFLOWED` перенесена на **исходящий путь** + latch (флаг транзиентный, 1 Гц его не ловит). Развилка **D1** владельцу. |
| H2 | High | §24.3 п.4: `packet_loss` — **процент** (не кумулятив); `client_series` без `in_seq`; скользящий loss — окно по `loss`. Развилка **D2**. |
| M1 | Med | §24.3 п.2: матч **IP+port**, packed IP, гейт `active`/gen (NAT/stale-IP). |
| M2 | Med | §24.6: ripple `DropRecord` `window[64]` (+384 Б) + обновить `static_assert` и три теста; запас `kLineBufSize`. |
| M3 | Med | §24.3 п.1: сброс **per-client**, гейт `client_hz>0`. |
| M4 | Med | §24.3 п.1: часы — `GetRealTime()`; сброс на connect/смене `gen`. |
| L1 | Low | §24.3 п.1: clamp `uint16`; «0 = нет данных» задокументировано. |
| L2 | Low | §24.3 п.3: семантика `last_overflow_s`; переименовать `drop.overflow` (коллизия с `category=="overflow"`). |
| L3 | Low | p99/гистограмма jitter — отложено (принято; бюджет). |

**Fine (не менять):** пассивность/чтение; INV-2/INV-3; выбор 3×uint16 (min/avg/max) и отказ от гистограммы/отдельного `clientq`; свободный `state`-бит `0x10`; делегирование rolling-loss анализатору; отсутствие новых cvar/экспортов/Makefile; бюджет `247+6=253<256` (pack(1)).

---

## 25. Тайминги коннекта и загрузки карты (Стадия 8)

> Стадия владельца (запрос «Уровень 2», вторая часть). Дизайн-ревью — `oracle`.

### 25.1 Контекст и проблема

Медленный коннект/auth-стопор и долгий **мапчейндж** сами по себе дают дропы и «отвалы» (клиенты не успевают войти/получить снапшот). Сейчас видна только стадия отказа (`reject`, §6.3) без **длительности**; длительность загрузки карты не фиксируется. Нужны: (а) тайминги стадий валидатора и Steam-auth; (б) длительность загрузки карты.

### 25.2 Проверенные технические факты (ground truth)

- Коннект-пайплайн (хуки §5): `SV_ConnectClient` (начало) → валидаторы `SV_CheckProtocol`/`SV_CheckIPRestrictions`/`SV_FinishCertificateCheck`/`SV_CheckKeyInfo`/`SV_CheckUserInfo` (return `0` = reject) → `Steam_NotifyClientConnect` (Steam/VAC) → `ClientConnected` (успех). Все — server thread; время — `funcs->GetRealTime()` (double).
- `reject` уже несёт `stage`; `connect`/`ClientConnected` — успех. `mapchange` (`SV_Spawn_f`) — маркер без длительности.

### 25.3 Решение (уточнено после design-review)

**Модель атрибуции (CT-1/CT-5/CT-7).** `SV_ConnectClient` — хук **без аргументов** (`IVoidHookChain<>`), поэтому `t0` **нельзя** привязать к слоту/клиенту; валидаторы получают `netadr_t*` (без id), а `Steam_NotifyClientConnect`/`ClientConnected` — `IGameClient*`. Вводится **единый контекст «коннект в процессе»** (один фикс. POD в `PluginContext`, server thread): `t0` — на входе `SV_ConnectClient`, сброс при завершении/reject. **Допущение синхронности** (весь пайплайн — в одном стеке на server thread) **обязательно проверить эмпирически** (по логу `stage`); при нарушении — деградация в `null`, а не misattribution (CT-5). Конкурентные/незавершённые попытки: контекст перезаписывается на новом `SV_ConnectClient`, guard по «активности».

1. **Тайминги коннект-пайплайна.** `t0` — на входе `SV_ConnectClient` (единый контекст). Валидаторы: `stage_ms` — длительность вокруг `callNext` (`funcs->GetRealTime()`, **guard на `<=0`/отрицательную дельту** — CT-8); при reject — `stage_ms` + `total_ms` (от `t0`). `Steam_NotifyClientConnect` → `steam_ms`; `ClientConnected` → `auth_ms` (общее `t0`→успех) + `steam_ms`. **Отсутствующие метки ⇒ `null`** (напр. короткое замыкание `SV_ConnectClient`/стадии другим плагином — все timing-хуки на `HC_PRIORITY_DEFAULT`; **CT-6**), без misattribution.
2. **Длительность загрузки карты (CT-2/CT-3).** JSONL append-only ⇒ **нельзя дополнить** уже записанный `mapchange`. Решение: `mapchange` эмитится при **готовности** (delayed-emission) с `load_ms` = от `SV_Spawn_f` до готовности; если готовность не наступила за окно N тиков — `load_ms=null`. Критерий готовности — **развилка владельцу Q1 (CT-3/CT-9)**; reset massdrop остаётся в `SV_Spawn_f` независимо.

### 25.4 Альтернативы

| Вариант | Почему не выбран |
|---|---|
| Отдельный тип `auth`/`mapload` | Доп. типы/шум; поля в существующих `reject`/`connect`/`mapchange` достаточно |
| Высокочастотный замер в коннекте | Хук-путь и так на стадиях; лишние сэмплы не нужны |

### 25.5 Влияние на требования/контракты/инварианты

- **Требования.** Уточняет G1/G2 (стопоры коннекта/карты). Контракты: `reject` +`stage_ms`,`total_ms`; `connect` +`auth_ms`,`steam_ms`; `mapchange` +`load_ms`. Exports без изменений.
- **Инварианты.** INV-1 (только чтение), INV-2 (фикс. per-client runtime, без аллокаций), INV-3, INV-5.

### 25.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **hooks** | Таймстемпы стадий (`SV_ConnectClient`/валидаторы/`Steam_NotifyClientConnect`/`ClientConnected`); `SV_Spawn_f` → `t_spawn`. |
| **metrics** | `RejectRecord` +`stage_ms`/`total_ms`; `ConnectRecord` +`auth_ms`/`steam_ms`; `MapChangeRecord` +`load_ms`. `static_assert` обновить. |
| **serialization / events** | Новые поля; «не наступило» → `null`. |
| **analyzer / webui** | Отображение таймингов коннекта/карты; корреляция с дропами. |
| **Makefile/exports** | Без изменений. |

### 25.7 Не-цели

- Тайминги на стороне клиента; DNS/маршрут; активные действия; изменение логики валидаторов (только замер).

### 25.8 QAS (QAS-CT)

| ID | Сценарий | Проверка |
|---|---|---|
| QAS-CT1 | `reject` несёт длительность стадии и общую | host-тест сборки записи |
| QAS-CT2 | `connect` несёт `auth_ms`/`steam_ms` | host-тест |
| QAS-CT3 | `mapchange.load_ms` = время до готовности; `null` при неготовности | unit + smoke |
| QAS-CT4 | Без аллокаций; `make`/`make test`/`verify-exports` | CI |

### 25.9 Вопросы

1. **Q1 — РЕШЕНО (автономный дефолт, обратимо).** Готовность карты = **первый server-tick после `SV_Spawn_f`, на котором мир/спавн доступны**; окно N=30 тиков; не наступило ⇒ `load_ms=null`.
2. **Q2.** Формат нового поля: `stage_ms`/`total_ms`/`auth_ms`/`steam_ms`/`load_ms` — `uint32` мс (как `HitchRecord.duration_ms`), nullable в P1; «не наступило/нет метки» ⇒ `null` (CT-4).
3. **Q3.** Форма: поля в существующих записях (выбрано); при отказе от delayed-emission — отдельная `mapload` (CT-2).

### 25.10 План

`docs/plan.md`, раздел «Стадия 8 — `connect-timings` — план реализации».

### 25.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS. Plan-гейт ЗАКРЫТ** — до сложения CT-1..CT-4 в дизайн (и решения Q1). Направление верное (поля в существующих записях, один clock, пассивность).

| ID | Severity | Учтено |
|---|---|---|
| CT-1 | High | §25.3: `SV_ConnectClient` без аргументов ⇒ **единый контекст «коннект в процессе»** вместо per-slot POD; корреляция/конкурентность описаны. |
| CT-2 | High | §25.3 п.2: **delayed-emission** `mapchange` до готовности (append-only JSONL; «дополнить» нельзя). |
| CT-3 | High | §25.3 п.2 + §25.9 Q1: задан контекст критерия готовности + окно + `null`; Q1 вынесена владельцу. |
| CT-4 | Med | §25.3/§25.9 Q2: тип полей `uint32` мс; nullable в P1; обновить `static_assert` и `emit_*`. |
| CT-5 | Med | §25.3: допущение синхронности + обязательная эмпирическая проверка; иначе `null`. |
| CT-6 | Med | §25.3 п.1: short-circuit других плагинов ⇒ `null` (все timing-хуки DEFAULT-приоритета). |
| CT-7 | Low | §25.3: guard контекста/генерации, перезапись на новом коннекте. |
| CT-8 | Low | §25.3 п.1: `GetRealTime()` + guard дельты. |
| CT-9 | Low | §25.9 Q1: готовность карты — развилка владельца. |

**Fine (не менять):** поля в существующих `reject`/`connect`/`mapchange`; `GetRealTime()` как clock; замер `stage_ms` вокруг `callNext`; отсутствие новых cvar/экспортов/Makefile; `DropRecord` не трогается.

---

## 26. Обогащение событий и корреляции (Стадия 9)

> Стадия владельца (запрос «Уровень 3»). Дизайн-ревью — `oracle`.

### 26.1 Контекст и проблема

Для триажа не хватает: (а) событий детекторов флуда `rehlds_security` (movecmd/stringcmd/dlfile/speedhack — §3), которые сейчас тонут в `drop.reason`; (б) flood-профиля — есть только один `top_ip`; (в) видимости **деградации сбора** (`write_error`/диска/retention) в самом логе (§11 описывает, но не эмитит); (г) скорости изменения эдиктов (шторм/утечка сущностей от плагина).

### 26.2 Проверенные технические факты (ground truth)

- Детекторы `rehlds_security` формируют **строки причин**, попадающие в `SV_DropClient(reason)` — детектируемы по подстроке в `reason` (эвристика; уточнить набор при реализации).
- `PacketStats` уже считает `unique_ips`/`top_ip` (bounded per-IP таблица, `hldsmon_ip_table`).
- `LogWriter` знает об ошибках записи/лимите объёма (§11); retention (`LogWriter::prune`) знает число удалённых файлов.
- `ServerSample::edicts` (`pfnNumberOfEntities`) — есть; churn = Δ.

### 26.3 Решение (уточнено после design-review)

1. **Детектор — ПОЛЕ в `drop`, не новый тип (EN-1).** В `DropRecord` добавляется bounded `detector[32]` (пусто = нет), заполняется тем же матчем в существующем `h_SV_DropClient`; `drop` остаётся. Новый тип `filter` **не вводится** (устраняет дубль записи и потерю `id`/`gen`, нужных для корреляции со Стадией 7).
2. **Substring — best-effort (EN-3).** Хукчейна детекторов `rehlds_security` нет ⇒ матч по подстроке `reason` — единственный in-process путь. Промах ⇒ `detector=""` (**не влияет на сам `drop`**); набор подстрок + **тест на реальной захваченной строке** — DoD. Ограничение: детекты без дропа (throttle/ignore) не покрываются.
3. **top-N — аддитивно (EN-4).** `packet` +`top_ips:[{ip,count}]` (N=3, compile-time const); `top_ip` **сохраняется** (совместимость P1/фикстуры); один линейный проход top-3 (вне hot path, без аллокаций). `static_assert(sizeof(PacketStats))` правятся.
4. **Деградация — pull из `sampler`, `LogWriter` остаётся sink (EN-2).** `LogWriter` уже отдаёт `error_count()`/`size_capped()`, а `prune()` знает число удалённых ⇒ добавить `last_prune_count()`+clear. 1 Гц server-tick читает дельты и эмитит `degraded`/`retention` через `events`, **только пока `log->enabled()`**. **Hard write-failure/кап ⇒ только console (`util_log`)/`hldsmon_status`** — запись о собственной ошибке в тот же отключённый файл физически невозможна (новый тип `write_error` **не вводится**).
5. **`edicts_churn` — `int32` + present-бит (EN-5).** Δ может быть **отрицательной** (убыль эдиктов, mapchange) ⇒ `int32`, не `uint32`. Первый интервал ⇒ `null` (present-бит, как в `host`); **baseline сбрасывается на `SV_Spawn_f`/mapchange**. Учесть, что `ServerSample` вложен в каждый `drop.server` ⇒ правка P1 (`server` + nested `drop.server`) + `static_assert`.
6. **Обязательства схемы (EN-6).** Каждое новое поле/тип ⇒ правка Приложения P1, фикстура, `KNOWN_TYPES` (для типов `degraded`/`retention`), README/`hldsmon_status` — по образцу стадий `modnet`/`retention`.
7. **Owner-fork (EN-7).** Схемообразующие решения (детектор-в-`drop`; additive `top_ips`; hard-failure=console) — **обратно-совместимы**, зафиксированы в §26.4/§26.9.

> Новые JSONL-типы стадии: только **`degraded`**, **`retention`**. Остальное — поля в существующих `drop`/`packet`/`server`.

### 26.4 Альтернативы

| Вариант | Почему не выбран |
|---|---|
| Разбирать `rehlds_security` лог-файл | Вне процесса; ненадёжно |
| Отдельный тип на каждый детектор | Шум; единый `filter` достаточно |
| top-N большой N | Объём/шум; N=3 bounded |

### 26.5 Влияние на требования/контракты/инварианты

- Требования: расширяет G1/G6 (корреляции). Контракты: новый тип `filter`; `packet` +top-N; `server` +`edicts_churn`; новые `degraded`/`write_error`/`retention`. Exports без изменений.
- Инварианты: INV-1, INV-2 (bounded), INV-3, INV-5.

### 26.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| **hooks** | Детект `rehlds_security`-причин при дропе → `filter`. |
| **sampler** | top-N в `packet`; `edicts_churn` в `server`. |
| **log_writer** | Эмиссия `degraded`/`write_error`/`retention` (через callback/шину, без аллокаций). |
| **metrics/serialization/events** | Новые POD/эмиттеры. |
| **analyzer/webui** | `KNOWN_TYPES +=` новые; фильтры/панели. |
| **Makefile/exports** | Без изменений. |

### 26.7 Не-цели

- Полный парсинг `rehlds_security`; активные действия; большой N; разбор внешних логов.

### 26.8 QAS (QAS-E)

| ID | Проверка |
|---|---|
| QAS-E1 | `rehlds_security`-дроп ⇒ `filter` с детектором |
| QAS-E2 | `packet` несёт top-N |
| QAS-E3 | `write_error`/`degraded`/`retention` эмитятся в соответствующих условиях |
| QAS-E4 | `edicts_churn` корректен; без аллокаций; `make`/`make test`/`verify-exports` |

### 26.9 Вопросы

1. Q1: набор подстрок детекторов `rehlds_security` — уточнить при реализации (не блокирует).
2. Q2 — **РЕШЕНО:** доставка `degraded`/`retention` — **pull-модель** (флаг/счётчики `LogWriter`, читаемые sampler); callback не вводится (re-entrancy).
3. **EN-7 — РЕШЕНО (автономный дефолт, обратимо):** детектор — **поле в `drop`**; `top_ips` — **аддитивно**; hard write-failure — **console**.

### 26.10 План → `docs/plan.md`, «Стадия 9 — `event-enrich`».

### 26.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS. Plan-гейт ЗАКРЫТ** до сложения EN-1..EN-6 (EN-1/EN-2 обязательны). Направление верное (пассивность, bounded, server thread); недоопределены контракт детектора и доставка деградации.

| ID | Severity | Учтено |
|---|---|---|
| EN-1 | High | §26.3 п.1: детектор перенесён **полем в `drop`** (тип `filter` не вводится). |
| EN-2 | High | §26.3 п.4: **pull-модель** (LogWriter — sink); hard-failure — console; тип `write_error` не вводится. |
| EN-3 | Med | §26.3 п.2: substring best-effort + `detector=""` при промахе + тест на реальной строке. |
| EN-4 | Med | §26.3 п.3: аддитивный `top_ips` + сохранён `top_ip`. |
| EN-5 | Med | §26.3 п.5: `edicts_churn` `int32` + present/`null` + сброс на mapchange + учёт `drop.server`. |
| EN-6 | Med | §26.3 п.6: обязательства P1/фикстуры/`KNOWN_TYPES`. |
| EN-7 | Low | §26.3 п.7/§26.9: схемообразующие решения обоснованы как обратно-совместимые. |

**Fine (не менять):** пассивность/server-thread; гейт `plugin_enabled()`; матч в существующем `h_SV_DropClient`; per-IP таблица для top-N; отсутствие новых cvar/экспортов; `retention`-prune на open/ротации; 1 Гц каденция.

---

## 27. Анализ/UI: наложение хост-метрик, incident bundle, config-diff (Стадия 10)

> Стадия владельца (запрос «Уровень 4»). Дизайн-ревью — `oracle`. **Зависит от Стадий 5–6** (`config`, `host`).

### 27.1 Контекст и проблема

Данные есть, но триаж ручной: хост-метрики не наложены на таймлайн дропов; собрать окно инцидента в один артефакт нечем; изменение конфигурации между окнами не подсвечено.

### 27.2 Решение (уточнено после design-review)

1. **Наложение host (AU-5).** UI: ряды `host` (Δ) на «Все датасеты»; **три состояния точки** — `null`=разрыв, бит caveat=маркер, иначе норма; **не рисовать `null` как 0** (первый сэмпл/недоступность). Нужна карта **field→caveat-source** (`udp_*→udp_netns`, `net_rx_*→net_dev`, `steal_pct→steal`, `io_*→io`, `disk_bavail_mb→disk`) + тест; `present_mask`/`caveat_mask` могут быть `null`.
2. **Данные в UI (AU-3).** `/api/analyze` расширяется ключами `host_series` (переиспользовать **существующий** `hldsmon.host_series`) и `config`-массивом; bundle — через новый endpoint (напр. `/api/bundle`; `/api/export` только CSV).
3. **`incident_bundle` (AU-1/AU-2).** Формат — **JSON-конверт** `{bundle_version, drop, window:{start,end}, config:[...], host:{...}, records:[...]}` + **ридер** в анализаторе (одиночный JSON не парсится `iter_records`). Политика: `drop` as-is **или** сырые записи — **не оба безусловно** (дроп уже встраивает `window[64]`); **верхняя граница размера** + предупреждение; **zip — опциональная обёртка транспорта** (не обязательна).
4. **config-diff (AU-4).** Ввести `config_at(records,t)` (last `config` с `t'<=t`) — его нет; `config_diff(before,after)` → `{changed[],added[],removed[],baseline}`; набор — `CONFIG_FIELDS` (+`map`/`maxclients`/`rehlds_*`?); `null`-семантика (`set`/`cleared` vs `changed`); **«нет baseline»** — явный флаг, не «всё изменилось».

### 27.3 Влияние

- Только инструменты анализа (`tools/analyzer`): `hldsmon.py` (функции), `webui.py`/`index.html` (UI). Плагин/схема — без изменений.
- Инварианты плагина не затрагиваются; analyzer — stdlib-only.

### 27.4 Компоненты

| Компонент | Изменение |
|---|---|
| **hldsmon.py** | `incident_bundle`, `config_diff`, `config_at`; `host_series` — **уже существует** (переиспользовать, AU-7) |
| **webui.py / index.html** | хост-ряды/overlay, кнопка «bundle», config-diff-панель |
| **fixtures/tests** | bundle/config-diff/host-series тесты |

### 27.5 Не-цели

- Плагинные изменения; realtime-дашборд; внешние зависимости; изменение схемы.

### 27.6 QAS (QAS-U)

| ID | Проверка |
|---|---|
| QAS-U1 | bundle содержит окно + config + host; парсится |
| QAS-U2 | config-diff корректен на границах |
| QAS-U3 | host-overlay рисуется; caveated помечены |
| QAS-U4 | Python-тесты; без плагинных изменений |

### 27.7 Вопросы

1. **Q1 — РЕШЕНО (автономный дефолт, обратимо).** Формат bundle — **JSON-конверт обязателен** (ридер; stdlib), zip — опциональная обёртка транспорта. (AU-6)
2. **Q2 (закрыто).** Размер окна N — **параметр** (CLI `--mins`, поле UI), default ±2 мин; не блокирует гейт. (AU-6)

### 27.8 План → `docs/plan.md`, «Стадия 10 — `analyzer-ui`».

### 27.9 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS.** Направление верное (чисто analyzer-side надстройка, без изменения плагина/схемы). AU-1..AU-9 сложены; недоопределены были формат/размер bundle, транспорт данных в UI и семантика config-diff.

| ID | Severity | Учтено |
|---|---|---|
| AU-1 | High | §27.2 п.3: JSON-конверт + ридер (одиночный JSON не парсится `iter_records`); zip опционален. |
| AU-2 | High | §27.2 п.3: политика включения (не дублировать `window[64]`) + верхняя граница размера. |
| AU-3 | High | §27.2 п.2: `/api/analyze` +`host_series`(existing)/`config`; endpoint `/api/bundle`. |
| AU-4 | Med | §27.2 п.4: `config_at`/`config_diff` + `{changed,added,removed,baseline}`. |
| AU-5 | Med | §27.2 п.1: карта field→caveat-source; три состояния точки; `null`≠0. |
| AU-6 | Med | §27.7: Q1 (формат) — владельцу; Q2 (N) — закрыто параметром. |
| AU-7 | Low | §27.4: `host_series` переиспользуется (уже есть). |
| AU-8 | Low | Расширение `ALL_DATASETS`/`localStorage`-схемы — учесть при реализации. |
| AU-9 | Low | QAS: bundle ре-инжестится в `iter_records`; граница размера; кейс «нет baseline». |

**Fine (не менять):** только analyzer/UI; `host`/`config` уже в `KNOWN_TYPES`; `host_series` с `None` вместо 0; точка интеграции — «Все датасеты»; реюз `iter_records`/`_record_ts`/`cmd_window`; stdlib-only; config-diff в анализаторе.

---

## 28. Большие логи (~700 МБ) в веб-UI: потоковый приём и снятие жёстких лимитов (Стадия 12)

> Стадия владельца (запрос: «веб-UI должен тянуть полный дневной лог ~700 МБ — и локальной загрузкой, и FTP-live — а не обрываться на 256 МиБ / 30-минутном хвосте»). Дизайн-ревью — `oracle`; план-ревью — `momus`. **Scope (после ответов владельца 2026-10-13):** `tools/analyzer` (`webui.py`, `webui/index.html`) **+ плагинный дефолт продюсера** (`hldsmon_max_mb` → `0`/unlimited в **`src/bootstrap.cpp`** — default-строка `s_val_max_mb[]` (`:55`) и fallback `cvar_f(..., 512.0f)` (`:308`); cvar-таблица `README.md`, design §7/§12. **`src/log_writer.cpp` менять не нужно** — он уже трактует `0`=unlimited (`:184`, `:225`)) **+ UI-компактность/sticky-тоггл**; JSONL-схема P1 и `KNOWN_TYPES` **не меняются**. Зависит от §7 (оценка объёма), §8 (FTP-live), журнал стадии 2f.

### 28.1 Контекст и проблема

Оба пути приёма веб-UI сегодня либо буферизуют весь лог в памяти, либо упираются в жёсткий лимит 256 МиБ — поэтому полный дневной лог (~700 МБ) сквозь UI не проходит:

- **Локальная загрузка.** `readAllFiles` (`webui/index.html:1153-1158`) читает файл целиком в JS-строку (`f.text()`), склеивает файлы; `fetchAnalyze` (`:1160-1192`) шлёт всё **одним** телом. Сервер `_read_body` (`webui.py:1049-1070`) при `Content-Length > MAX_BODY_BYTES` (`:53`, 256 МиБ) отдаёт `None` → `_handle_analyze` (`:1141-1146`) отвечает **HTTP 413**; при отсутствии/невалидном `Content-Length` возвращает `b""` → **молча 0 записей**. Дополнительно JS-строка ~700 МБ упирается в предел длины строки движка браузера (V8 **≈** 512 МБ — приблизительный лимит длины строки в UTF-16-кодовых единицах) → исключение ещё до отправки.
- **FTP live-tail.** `_load_tail` (`:665-698`) тянет хвост окном `FTP_WINDOW_S=1800` (`:59`) чанками `FTP_BACK_CHUNK=8 МиБ` (`:62`), останавливаясь при `start<=0` **или** `loaded[0] >= MAX_FTP_BYTES` (`:56`; проверка `:696`). `ftp_load_earlier` (`:931-991`) останавливается так же (`:962`; `loaded` — счётчик **на запрос**, `:948`). Сессия хранит **все** распарсенные `records[]` в RAM и каждый раз пере-анализирует всё накопленное; при отсутствии REST-поддержки `_download`/`_download_range` уходят в полнофайловый `_retr_all` (`:512`, `:595`) → на 700 МБ мгновенный `_FtpTooLarge` (`:477-478`). То есть даже при постраничном «← загрузить раньше» сессия не доходит до начала файла практически (накопление `records[]` + O(n²)-переразбор), а fallback без `REST`/`SIZE` обрывается сразу.
- **Разбор потоковый, но анализ — нет (B1).** `_records_from_bytes` (`:166-181`) уже пишет байты во временный файл и зовёт `hldsmon.parse_records`; `iter_records` (`hldsmon.py:163-187`) **потоковый**, терпимый, без лимита размера. **Но** `parse_records` (`hldsmon.py:190-201`) возвращает **полный `list`**, а `analyze_records` (`webui.py:293-319`) материализует все записи и строит полный JSON-payload (`build_series` `:191-213`, `client_series`, `drops`, …). Для ~3M записей/сутки это гигабайты RSS и огромный ответ → **ингест-стриминг сам по себе цели «весь день целиком» не достигает**; серверный период/window обязателен (§28.3 п.1).
- **O(n²) на FTP-сессии (B2).** `ftp_load_earlier` (`:931-991`) на каждом вызове вызывает `analyze_records` по **всему накопленному** набору (`:973`), как и `ftp_refresh` (`:918`). Сегментный spool убирает копирование байтов, но **не** устраняет переразбор всего набора, пока анализ не станет оконным/инкрементальным (следствие B1).

Итог: это не «настройка», а блокер цели. Нужны: (1) потоковый приём в spool для обоих путей; (2) **серверный период/window** в `/api/analyze` (иначе анализ дня материализует всё — B1/B2); (3) память, ограниченная **стримингом**, а не размерным лимитом; (4) модель угроз для unbounded-приёма при не-loopback bind.

**Решение владельца (2026-10-13) — лимитов чтения нет.** И FTP, и локальный файл читаются **без ограничения размера по умолчанию**; `MAX_BODY_BYTES`/`MAX_FTP_BYTES` перестают быть *отклоняющими* лимитами (REQ-L1). Верхняя граница памяти обеспечивается **стримингом** (диск-spool + оконный/потоковый анализ), а **не** размерным cap (REQ-L2). Размерный лимит остаётся только как **опциональный клапан** безопасности (default **выключен**; REQ-L3). Продюсер — аналогично без лимита записи по умолчанию (`hldsmon_max_mb=0`) с опциональным клапаном (REQ-L4). Window/период — **UX-механизм** (удобство/производительность), **не** потолок принимаемого размера (REQ-L5). Подробности — §28.3.

### 28.2 Проверенные технические факты (ground truth)

- Константы (`webui.py`): `MAX_BODY_BYTES = 256*1024*1024` (`:53`); `MAX_FTP_BYTES = 256*1024*1024`, «matching `MAX_BODY_BYTES`» (`:56`); `FTP_WINDOW_S = 1800.0` (`:59`); `FTP_BACK_CHUNK = 8*1024*1024` (`:62`); `DEFAULT_FTP_TIMEOUT = 30.0` (`:65`).
- Local ingest: `_read_body` (`:1049-1070`) возвращает `None` при `length > MAX_BODY_BYTES`; `b""` при `raw is None`, не-`int`, `<=0`. `_handle_analyze` (`:1141-1146`)/`_handle_export` (`:1155-1175`)/`_handle_bundle` (`:1177-1199`) при `None` → 413 + `close_connection`.
- FTP ingest: `_retr_all` (`:465-487`) кидает `_FtpTooLarge` при превышении `MAX_FTP_BYTES` (raise `:477-478`). `_download` (`:490-523`) и `_download_range` (`:548-598`) падают на полнофайловый `_retr_all` при REST-фейле (`:512`, `:595`). `_load_tail` (`:665-698`) тормозит на `:696`; `ftp_load_earlier` (`:931-991`) — на `:962`. Сессия хранит `records` (list) + `start`/`end`/`leftover`/`cfg`/`window_s`/`updated_at` (`ftp_connect` `:855-864`; `_session_update`). `_ftp_meta` (`:701-720`) отдаёт `path/bytes/records/updated_at/reached_start/start_t/end_t`.
- Парсер: `_records_from_bytes` (`:166-181`) → temp-файл → `parse_records`; `_records_from_lines` (`:391-398`), `split_tail` (`:401-421`), `_parse_line` (`:363-388`) — терпимые. `iter_records` (`hldsmon.py:163-187`) потоковый, без cap.
- Сервер: `ThreadingHTTPServer` (`_Handler`, `make_server` `:1300-1308`), `daemon_threads=True`; `--host` default `127.0.0.1`, `--port` default `8765` (`build_parser` `:1311-1324`); аутентификации нет.
- Фронт: `ACCEPT_RE` (`index.html:1107`); `readAllFiles` (`:1153-1158`) → `fetchAnalyze` (`:1160-1192`) кладёт строку в `state.body` (`:1180`); `state.body` переиспользуется для `/api/export` (`:1205`) и `/api/bundle` (`:1239`); при FTP `state.body = null` (`:1528`), поэтому экспорт/bundle для FTP отключены (`:3692`, `:3708`). `ftp.reachedStart` читается из `ftp.reached_start` (`:1499`, `:1524`, `:1567`, `:1602`).
- Продюсер (R2-B1-факт): дефолт `hldsmon_max_mb` = **512** задан в **`src/bootstrap.cpp:55`** (`char s_val_max_mb[] = "512";`), регистрация cvar — `:124`; рантайм-fallback — **`:308`** (`c.log->set_max_mb(static_cast<uint64_t>(cvar_f(c.cvars.max_mb, 512.0f)))`), вызывается один раз на attach. По решению владельца меняются **оба**: `"512"`→`"0"` и `512.0f`→`0.0f`. **`src/log_writer.cpp` уже трактует `0`=unlimited** (`:225` — `mb>kLimitMb` ⇒ `0`; `:184` — гейт срабатывает только при `max_bytes_ != 0`) → **изменений в `log_writer.cpp` не требуется**. `hldsmon_retention_days` default **3**; дневной файл ≈300–600 МБ (README, §7).
- Анализ/парсер: `parse_records` (`hldsmon.py:190-201`) возвращает **полный `list`**; `analyze_records` (`webui.py:293-319`) материализует все записи; `ftp_refresh` (`:918`) и `ftp_load_earlier` (`:973`) пере-анализируют **весь** накопленный набор (B1/B2).
- Temp: `_records_from_bytes` (`:173-181`) использует `tempfile.NamedTemporaryFile(delete=False)` в дефолтном каталоге (часто **tmpfs/RAM**), без гарантии `finally` при краше процесса (M1/M6).
- Кодирование/строки (M7/m1/m3): `iter_records` открывает `encoding="utf-8"` (`hldsmon.py:172`, **не** `utf-8-sig`), но делает `line.strip()` (`:174`) — **CRLF уже обрабатывается там**; `f.text()` в браузере (текущий путь) снимает BOM, а прямой `Blob` — нет, поэтому BOM-риск относится только к новому Blob-пути. **Решение (m1):** BOM снимается **при приёме на сервере** (одно место), а **не** правкой `hldsmon.iter_records` — чтобы не менять глобальное поведение CLI/library-путей. Нормализация CRLF (`index.html:1157`) относится только к line-splitting нового Blob-пути.
- Ротация суток: `ftp_refresh` (`:881-884`) переоткрывает путь при смене UTC-даты (`resolve_path`, токен `{date}`) — сегментная модель должна описать смену дня (m5).

### 28.3 Решение (design-level)

Направление (решения владельца — §28.9; **чтение и запись без лимита по умолчанию**). Надстройка над `tools/analyzer` + плагинный дефолт продюсера; схема P1 не трогается. **Серверный период/window в `/api/analyze` — обязательная часть scope (B1), но как UX/производительность, а не потолок размера.**

**Требования стадии (REQ-L):**

| ID | Требование |
|---|---|
| **REQ-L1** | **Чтение без лимита по умолчанию** — и FTP, и локальный файл; никакого отклоняющего cap. Размер файла не ограничивает приём (`«ограничения быть не должно… читать без ограничений, что в ФТП, что, тем более, из файла»`). |
| **REQ-L2** | Память ограничена **стримингом** (диск-spool приёма + оконный/потоковый анализ), **не** размерным лимитом. Полный день обрабатывается end-to-end. |
| **REQ-L3** | **Опциональный клапан чтения** (`--max-log-mb`, default `0`=unlimited) — доступен, но **выключен**; при включении (>0) — единый машиночитаемый отказ. |
| **REQ-L4** | **Продюсер без лимита записи по умолчанию:** `hldsmon_max_mb=0` (unlimited), опциональный клапан; риск disk-fill — осознанное решение владельца. |
| **REQ-L5** | **Window/период — UX**, не потолок: читается всё; окно — только удобство/скорость. |
| **REQ-L6** | **FTP-окно: удобная и очевидная механика** — явный размер окна, индикатор загруженного диапазона, очевидные «раньше/позже», прогресс, отсутствие скрытых тихих остановок. |
| **REQ-L7** | **Компактный sticky-блок «Период»** — уменьшить занимаемую вертикальную высоту. |
| **REQ-L8** | **Sticky/fixed — опционально** (тоггл пользователя, персист в `localStorage`). |

**Решения (design-level):**

1. **Серверный период/window — обязателен, но как UX/производительность (B1/REQ-L5).** `analyze_records` (`:293-319`) материализует все записи, поэтому:
   - `/api/analyze` принимает **`from`/`to`** (epoch-секунды, включительно) и опц. `window`. **Если `from`/`to` не заданы, сервер ВСЕГДА применяет окно по умолчанию** (никогда не «весь файл»); сервер читает spool **потоково** (`iter_records`) и материализует **только записи окна**.
   - **Полный день — coarse overview + оконность (ii-b) обязательна (R1-B2).** Ответ строится **однопроходным стриминговым агрегатором** по spool, который **не держит полный список записей**. Контракт coarse-ответа:
     - **Даунсэмплированные/потоковые ряды** (память **O(window)** или O(точек даунсэмпла), не O(records)): `series` (server) и `client_series` — даунсэмпл/скользящие агрегаты.
     - **Полнодневные агрегаты** (память **O(players)+O(drops)+O(1)**, не O(records)): `digest.counts_by_type`, суммы по типам, `top drop reasons`, список игроков/сессий (`player_groups`/`client_identities`), полный список `drops` (O(drops)), `log_end`, счётчики `anomalies`.
     - **НЕ входит:** полный список сырых `records`.
   - **Стриминговый агрегатор:** один проход по spool; инкрементально накапливает счётчики/топ-списки/идентичности/даунсэмпл-ряды; при запросе окна — проход только по окну для полного разрешения (`series`/`client_series`/`focus_points`). Хранит **только** агрегаты/окно, никогда — все `records` (это делает INV-L2 и QAS-L10 выполнимыми).
   - **Фронт меняется:** период выбирается **до/через** запрос (серверная нарезка); клиентская фильтрация — только для уже загруженного окна. Без `from`/`to` фронт получает coarse; движение периода/зум ⇒ дозапрос окна.
   - `/api/export`/`/api/bundle` адресуются окном (`from`/`to`) и **стабильным ключом drop `(run, t, userid)`** (п.6; `run` — из `hldsmon._attach_runs`).
2. **Потоковый приём в spool.** Оба пути кладут байты в **диск-backed spool**, а не в память; анализ читает spool потоково. Память приёма — **O(чанк)**; память анализа — **O(window)** (п.1), **не** O(файл).
3. **Local: тело → spool, без size-reject.** `_read_body` стримит тело в spool-файл со счётчиком байт (не в `chunks`); **по умолчанию нет отклоняющего порога** (REQ-L1); при включённом клапане (REQ-L3, >0) — отказ по достижении. Отсутствие/невалидный `Content-Length` → **не тихий 0**: стрим до EOF либо 411. Фронт шлёт `File`/`Blob` напрямую (`fetch(url,{body:file})`), без `f.text()`; несколько файлов — композитный `Blob`. **R1-M7/R2-m1:** снять BOM **при приёме на сервере** (одно место — `hldsmon.iter_records` не трогаем, чтобы не менять глобальное поведение CLI/library); CRLF в `iter_records` уже покрыт `line.strip()` (`hldsmon.py:174`) — нормализация фронта (`index.html:1157`) нужна только для line-splitting Blob-пути; сохранить порядок `Blob`↔`files=`.
4. **FTP: сегментный spool вместо `records[]`.** Сессия хранит **упорядоченный список сегментных spool-файлов** (`[older…, current]`) + `t_lo/t_hi` + `spool_bytes`; `load-earlier` **добавляет** сегмент старше. Память приёма — O(чанк); **анализ — O(window)** (п.1). **Сегментный spool сам по себе O(n²) не снимает** — снимает именно оконный анализ (B2). Смена суток (`{date}`): сегменты разных дней раздельно, без потерь/дублей на границе (R1-m5).
5. **FTP без REST/`SIZE`.** Fallback не тянет файл в память (`_retr_all`): полный `RETR` стримится в spool-файл, диапазоны нарезаются из файла. Любой размер проходит (REQ-L1).
6. **Handle-жизненный цикл и адресация (R1-M4/R2-m2).** Server-side spool-**handle** (local и FTP): создаётся на приёме, принадлежит серверу, очищается по закрытию/TTL/старту; **вытесненный handle** ⇒ явный `{"code":"session_expired"}` + фронт перезагружает. `/api/export`/`/api/bundle` принимают handle (+ окно) вместо повторной отправки тела. **Drop-адресация — стабильный ключ `(run, t, userid)`** (m2; `run` из `hldsmon._attach_runs`; позиционный индекс и голый `(t,userid)` расходяются при оконной нарезке/рестартах). FTP-путь перестаёт отключать export/bundle.
7. **Клапан чтения — опциональный и выключен (REQ-L3/R1-M2).** `--max-log-mb` (+ env `HLDSMON_WEBUI_MAX_MB`) default **`0`=unlimited**. При `>0`: local — на запрос; FTP — **кумулятивно на сессию**; срабатывание — единый отказ (п.8). **`0` (безлимит) разрешён только на loopback** (иначе — предупреждение/opt-in).
8. **Отказ — только при включённом клапане (R1-M3/REQ-L3).** При клапане `>0` — единый контракт over-cap на **всех трёх** endpoint: `{"ok":false,"code":"log_too_large","limit_bytes":N}`; для local — **читаемый JSON**, без «Failed to fetch» из-за `close_connection`; FTP-мета — `cap_hit`/`limit_bytes` (+ опц. `truncated`), `reached_start` = offset 0 **удалённого** файла; `_ftp_meta.bytes` = байты удалённого диапазона (не спула). **По умолчанию (клапан `0`) этого пути нет.**
9. **Продюсер: `hldsmon_max_mb` default `0`=unlimited (REQ-L4).** Запись — «сколько пишется, столько пишется»; прежний размерный cap перестаёт быть отклоняющим по умолчанию. **Рекомендация + риск (решение владельца):** `hldsmon_max_mb` остаётся **опциональным** клапаном (default `0`=unlimited); при выключенном клапане возможен **диск-fill** сервера — осознанный выбор владельца. План обязан включить: **`src/bootstrap.cpp`** — default-строка `s_val_max_mb[]` `"512"`→`"0"` (`:55`) и fallback `cvar_f(c.cvars.max_mb, 512.0f)`→`0.0f` (`:308`); **`src/log_writer.cpp` менять НЕ нужно** — `0` уже =unlimited (`:184`, `:225`); cvar-таблица `README.md`; design §7/§12.
10. **FTP-окно UX (REQ-L6).** Явный **размер окна** (шаг/пресеты), **индикатор загруженного диапазона** (от/до времени + байты `[start,end]` удалённого файла), очевидные **«← раньше» / «позже →»**, **прогресс** скачивания/разбора, **`reached_start`** и **отсутствие скрытой тихой остановки**; при включённом клапане — видимый `cap_hit`.
11. **UI: компактный sticky «Период» (REQ-L7).** Уменьшить высоту закреплённого блока периода/тулбара: однострочный layout (компактные поля от/до, пресеты в одну линию/меню, глобальный выбор клиента — в подстроку), sticky-поведение без потерь; ориентир — ≤ ~2 строк.
12. **UI: sticky — опциональный тоггл (REQ-L8).** Пользователь может **выключить** fixed/закрепление; состояние персистится в `localStorage` по конвенции репозитория (`hldsmon.ui.*`) и применяется без reload.
13. **Spool: диск, бюджет, crash-safety (R1-M1/M5/M6).** Spool-dir — **диск-backed** (не tmpfs): `--spool-dir`/env. Free-space pre-check; **per-session и process-wide диск-бюджет** — ресурсный предохранитель, **не** размерный cap лога (при исчерпании — явный `spool_full`). **Лимит одновременных** загрузок/сессий + `read-timeout`. Crash-safe cleanup: startup-cleanup + age-sweeper + `finally`/BrokenPipe.
14. **Security/DoS при отсутствии cap.** Loopback по умолчанию; не-loopback + безлимит ⇒ предупреждение + **opt-in**; риск disk-fill (spool и лог) задокументирован; аутентификации нет — вне scope.

### 28.4 Альтернативы (по развилкам, trade-offs)

**Развилка (a) — локальный приём:**

| Вариант | Механика | Память браузера | Память сервера | Плюсы | Минусы |
|---|---|---|---|---|---|
| **A. Поток в spool** | `fetch(body:File)`; сервер стримит в temp | O(1) (шлётся Blob) | O(чанк) | Чинит и JS-лимит, и серверный OOM; парсер уже потоковый | Меняются `_read_body` + фронт; нужен handle для export/bundle |
| B. Поднять whole-file cap | `MAX_BODY_BYTES` ↑ | ~700 МБ JS-строка → RangeError | ≈2× body | Минимум кода | Не чинит браузер/OOM; не «end-to-end» |
| C. Chunked upload | Нарезка `File` → N POST+append | O(чанк) | O(чанк) | Resume/прогресс/устойчивость | Новый протокол, chunk-index; избыточно (YAGNI) |

**Развилка (b) — FTP: достижение всего файла + хвост-окно:**

| Вариант | Механика | Память | Плюсы | Минусы |
|---|---|---|---|---|
| **A. Сегментный spool + конфиг-cap** | Сегментные spool-файлы; cap конфигурируем | приём O(чанк); **анализ O(окно)** | Достижимо начало файла; ингест-память O(чанк) | Сам по себе O(n²) **не** снимает — нужен оконный анализ (B2); новые поля сессии |
| B. Только поднять `MAX_FTP_BYTES` | `records[]` in-memory | O(файл) → OOM на 700 МБ | Мало кода | Не решает память/переразбор |
| C. In-memory, cap под дневной размер | — | O(файл) | — | То же; ломается при >дефолта продюсера |

Отдельная под-развилка (b2): **сохранять ли 30-минутное хвост-окно на connect.** Рекомендация — **да, по умолчанию** (быстрый connect, дешёвый инкрементальный refresh), но окно — **параметр** (уже есть `window_s` в spec), а `load-earlier` обязан доходить до offset 0 без скрытого обрыва.

**Развилка (c) — политика лимита (решено владельцем: без лимита по умолчанию):**

| Вариант | Плюсы | Минусы |
|---|---|---|
| Хардкод-константа (как сейчас) | Просто | Отклоняет большой лог — против REQ-L1 |
| **Без лимита по умолчанию + опциональный клапан (рек., решение владельца)** | Соответствует «читать без ограничений»; память держит стриминг; клапан доступен при нужде | По умолчанию нет защитного порога → нужен loopback-guard и явная модель риска |
| Раздельные cap upload/FTP | Точный контроль | Против REQ-L1 (лимиты по умолчанию); больше ручек |

**Развилка (d) — модель угроз (кратко):** default-loopback + отсутствие auth ⇒ любой не-loopback bind выставляет endpoint приёма (и FTP-креды в RAM) наружу. Unbounded-тело → забивание диска (spool) или RAM; unbounded remote-read → ресурсы + произвольный FTP-хост (SSRF-подобно). Митигации — §28.3 п.13–14, детали — развилка (d) §28.9.

**Развилка (e) — поведение при лимите (по умолчанию неприменимо).** По умолчанию лимита нет, поэтому путь отказа **отсутствует**. Он существует только при явно включённом клапане (REQ-L3): тогда — **explicit-reject + машиночитаемый код** (никогда тихая обрезка/0 записей). При выключенном клапане `Content-Length`-пустышка (`b""`) недопустима: стрим до EOF либо 411.

**Развилка (f) — охват: приём vs приём + оконный анализ (B1).** Ингест-стриминг обязателен, но `analyze_records` (`:293-319`) **материализует все `records`** — без оконного анализа полный день даёт гигабайты RSS. **Решение владельца: читается всё; window — UX-механизм** (REQ-L5). Уровень оконности: **(ii-a) анализ с периодом** (нарезка из spool по `from`/`to`) — рек. минимум; **(ii-b) пред-агрегация** для coarse overview полного дня; **(ii-c) чисто клиентский период** — отвергнут (требует полного payload). «Только приём» без оконного анализа — не «сокращение scope», а невыполнение REQ-L2. Отдельный QAS на память анализа дня — QAS-L10.

**Развилка (g) — UI/FTP-UX (решено владельцем).** Дополнительные требования: **(REQ-L6)** удобная/очевидная механика FTP-окна; **(REQ-L7)** компактный sticky «Период»; **(REQ-L8)** sticky — опциональный тоггл с персистом. Это owner-decided UX-требования (без развилки «делать/не делать»); детали layout — на этапе плана.

### 28.5 Влияние на требования, схему, контракты

- **Требования (design §2):** закрывает G4 (стягивание файлов), G5 (ограниченная память/предсказуемые накладные — теперь и на стороне UI, **обеспечивается стримингом, а не размерным лимитом**), G6 (офлайн-инструменты); вводит **REQ-L1..REQ-L8** (§28.3). N3 (web-UI как non-goal v1) **устарел** — UI развивается стадиями 2c…11. Инварианты плагина (INV-1..INV-4) не затрагиваются; **INV-5** (формат JSONL) — без изменений; **INV-6** (границы: без БД/внешнего агента) — сохраняется; инструмент остаётся **stdlib-only**.
- **Расширение scope (owner #2).** Дефолт продюсера `hldsmon_max_mb` меняется на **`0`=unlimited** (опц. клапан) → обязательные артефакты плана: **`src/bootstrap.cpp`** (default-строка `s_val_max_mb[]` `"512"`→`"0"` (`:55`); fallback `cvar_f(c.cvars.max_mb, 512.0f)`→`0.0f` (`:308`)), cvar-таблица `README.md`, design §7/§12. **`src/log_writer.cpp` не меняется** — `0` уже =unlimited (`:184`, `:225`).
- **Схема P1:** **без изменений** — ни одного нового поля записи, `KNOWN_TYPES` не трогается.
- **Контракты (аддитивно/совместимо):**

| Контракт | Изменение | Совместимость |
|---|---|---|
| `POST /api/analyze` | **+** входы `from`/`to`/`window`; тело стримится в spool; **всегда применяется окно** (default-окно, если `from`/`to` пусты); отдаёт **coarse overview дня** + handle; контракт coarse — §28.3 п.1 (ряды даунсэмпл; агрегаты O(players)/O(drops); **без** полного `records`) | Период теперь серверный (фронт меняется); **ответ НЕ прежний full-resolution**: малый файл → весь файл как одно окно, большой → coarse + окна по требованию; прежний контракт приёма тела сохранён |
| `POST /api/export`, `/api/bundle` | Источник — spool по **handle** (+ окно); адресация drop — **ключ `(run,t,userid)`** (не позиционный индекс); работают и для FTP-пути. **Вытесненный handle** (TTL/лимит сессий) ⇒ явный `{"code":"session_expired"}` + фронт перезагружает файл/сессию | Body-путь сохранён для малых файлов |
| Over-cap (**только при клапане >0**) | Единый `{"ok":false,"code":"log_too_large","limit_bytes":N}` | Путь существует только когда клапан включён; по умолчанию неприменим |
| FTP `ftp`-мета | **+** `cap_hit`/`limit_bytes`/опц. `truncated`; `bytes` = удалённый диапазон (не спул); `reached_start` = offset 0 **удалённого** файла; **+** индикатор диапазона (REQ-L6) | Аддитивно |
| FTP/local-сессия | `records[]` → сегментные spool-файлы + `t_lo/t_hi`/`spool_bytes` + handle | Внутренняя; тесты обновить |
| CLI/ENV (клапан) | **+** `--max-log-mb` default **`0`=unlimited** (+env), `--spool-dir` (+env) | Новые флаги, но **выключены** по умолчанию |
| Плагин/cvar | `hldsmon_max_mb` default **`0`=unlimited** (опц. клапан) — **`src/bootstrap.cpp`** (`:55`,`:308`), не `log_writer` | Смена дефолта; README/§7/§12 |
| `webui/index.html` upload flow | `fetch(body:File/Blob)` вместо `f.text()`; **BOM снимается приёмом на сервере** (m1); CRLF-нормализация — только line-splitting Blob-пути; период — до запроса; ключ drop `(run,t,userid)` | Меняет UX-поток периода |
| `webui/index.html` период/sticky/FTP-окно | **+** компактный «Период» (REQ-L7); **+** тоггл sticky в `localStorage` (REQ-L8); **+** FTP-окно UX (REQ-L6) | Новые UI-элементы |

- **Инварианты стадии:** INV-L1 (память приёма — O(чанк); память анализа — **O(window)**: сервер **всегда** применяет окно, подставляя default-окно при отсутствии `from`/`to`; сегментный spool сам O(n²) не снимает); **INV-L2 (нет размерного лимита по умолчанию; память ограничена стримингом — диск-spool + coarse-агрегатор без полного списка записей; клапан `>0` — только явно и с машиночитаемым отказом)**; INV-L3 (ни один путь не даёт молча 0 записей/обрезку); INV-L4 (spool на **диске**, budget + crash-safe cleanup); INV-L5 (stdlib-only; схема P1 без изменений; `ftp.bytes`/`reached_start` — семантика удалённого файла).

### 28.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| `webui.py` | `_read_body`/`_handle_analyze` (стрим в spool; входы `from`/`to`/`window`; материализация только окна; coarse overview дня; **клапан `--max-log-mb` off by default**); `_handle_export`/`_handle_bundle` (handle + стабильный ключ drop); `_load_tail`/`ftp_load_earlier`/`ftp_connect`/`ftp_refresh`/`ftp_close` (сегментный spool, без лимита по умолчанию, `t_lo/t_hi`, индикатор диапазона); `_download`/`_download_range`/`_retr_all` (fallback → spool-файл); `_ftp_meta` (`cap_hit`/`limit_bytes` при клапане; семантика `bytes`/`reached_start`); реестр spool-handle + lifecycle/TTL/cleanup + бюджет + семафор + read-timeout; `build_parser`/`main` (+`--max-log-mb` default `0`, `--spool-dir`, guard не-loopback) |
| `webui/index.html` | `readAllFiles`/`fetchAnalyze` (прямой `Blob`, без `f.text()`; BOM/CRLF; порядок `files=`); период — **до запроса** (серверная нарезка), coarse overview + дозапрос окон; export/bundle через handle; стабильный ключ drop; **FTP-окно UX (REQ-L6)**: размер окна, индикатор диапазона, «раньше/позже», прогресс, `reachedStart`, `cap_hit`; **компактный «Период» (REQ-L7)**; **тоггл sticky + `localStorage` (REQ-L8)**; обработка 413/`log_too_large` (только при клапане) |
| `hldsmon.py` (в scope) | снятие BOM **при приёме на сервере** (M7/m1; **`hldsmon.iter_records` не трогаем** — глобальное поведение CLI/library-путей не меняется); оконная фильтрация — при парсинге окна |
| `src/bootstrap.cpp` + `README.md` + design §7/§12 (**в scope, owner #2**) | `src/bootstrap.cpp`: default-строка `s_val_max_mb[]` `"512"`→`"0"` (`:55`) и fallback `cvar_f(c.cvars.max_mb, 512.0f)`→`0.0f` (`:308`); **`src/log_writer.cpp` НЕ меняется** — `0` уже =unlimited (`:184`/`:225`). cvar-таблица `README.md`; §7/§12. Тест: `tests/test_log_writer.cpp` (семантика `0`=unlimited уже покрыта) |
| `tools/analyzer/tests/` | Python `unittest`: потоковый приём без лимита; клапан >0 → `log_too_large`; missing `Content-Length` не 0; FTP-до-нуля (без cap); REST-less fallback; сегменты + смена суток; spool budget/exhaustion; cleanup; BOM/CRLF/мультифайл; окно `from`/`to`; память анализа (QAS-L10); coarse-агрегатор. **UI QAS-L16/L17/L18 — вне `make test`** (метод — §28.8) |

### 28.7 Не-цели

- Изменения JSONL-схемы P1, `KNOWN_TYPES`, `exports.whitelist`, Makefile, ABI/хуков плагина. Плагинное изменение ограничено **дефолтом/семантикой `hldsmon_max_mb`** (owner #2) — не трогает сбор/сериализацию/схему.
- Аутентификация/многопользовательская модель (web-UI остаётся локальным инструментом).
- Resumable/chunked-upload (если развилка (a) не выберет вариант C).
- Внешние зависимости (остаётся Python-stdlib).
- Windows-специфика (отдельная фаза); изменение `hldsmon_retention_days` (retention не трогаем).
- Realtime-дашборд и кэш повторных окон/инкрементальный агрегатор **сверх** обязательных (ii-a)+(ii-b). **Пред-агрегация для coarse overview (ii-b) — В scope** (см. §28.3 п.1), не исключается.

### 28.8 Приёмочные качественные сценарии (QAS-L)

| ID | Проверка |
|---|---|
| QAS-L1 | Локальная загрузка файла **>256 МиБ** (без лимита) → HTTP 200; **измеримо:** `ru_maxrss` сервера (subprocess) **не растёт пропорционально файлу** (Δ ≤ фикс. порог при росте файла); браузер не строит JS-строку |
| QAS-L2 | **По умолчанию без лимита:** файл произвольного размера принимается. **При клапане >0:** HTTP 413 + `{"ok":false,"code":"log_too_large","limit_bytes":N}` (читаемый JSON, не «Failed to fetch»); missing/invalid `Content-Length` → не тихий 0 (стрим до EOF/411) |
| QAS-L3 | FTP connect грузит хвост-окно; `load-earlier` доходит до offset 0 ⇒ `reached_start=true`, весь файл в сегментном spool; **измеримо:** RSS сервера ~O(окно), не O(файл) |
| QAS-L4 | Упор в **включённый** клапан ⇒ `cap_hit=true` + `limit_bytes` в `ftp`-мета; UI показывает причину; без тихой обрезки. При выключенном клапане `cap_hit` не выставляется |
| QAS-L5 | FTP без `SIZE`/`REST` → полный `RETR` стримится в spool; большой файл не даёт `_FtpTooLarge`; диапазоны нарезаются из файла |
| QAS-L6 | Export/bundle на большом файле через handle (без повторной отправки тела) для **local и FTP**; стабильный ключ drop `(run,t,userid)`; вытесненный handle → `session_expired`; малый body-путь сохранён |
| QAS-L7 | Bind на не-loopback ⇒ предупреждение; безлимит (клапан `0`) на не-loopback требует opt-in/отклоняется |
| QAS-L8 | Spool-dir на **диске** (не tmpfs); free-space pre-check; per-session/process budget (**ресурсный предохранитель, не size-cap лога**); при исчерпании — явный `spool_full` |
| QAS-L9 | Cleanup: startup-cleanup + age-sweeper + `finally`/BrokenPipe; после закрытия/TTL файлов нет |
| QAS-L10 | **Память анализа дня:** запрос окна материализует ≤ записей окна (не всего файла); coarse overview дня строится без полного списка; RSS не растёт с полным днём |
| QAS-L11 | M7: файл с BOM → первая строка не теряется; CRLF и порядок мультифайлового `Blob` ↔ `files=` сохранены |
| QAS-L12 | Лимит одновременных загрузок/сессий + read-timeout; превышение параллелизма ⇒ отказ, не неограниченные потоки |
| QAS-L13 | Оконная корректность: `from`/`to` (включительно) дают то же множество записей, что клиентская фильтрация по полному набору (эталон) |
| QAS-L14 | Смена суток в FTP-сессии: сегменты разных дней без потерь/дублей на границе |
| QAS-L15 | stdlib-only; схема P1/`KNOWN_TYPES` без изменений; существующие Python-тесты зелёные + новые |
| QAS-L16 | **FTP-окно UX (REQ-L6):** явный размер окна; индикатор загруженного диапазона (время + `[start,end]`); кнопки «раньше/позже»; прогресс; `reached_start`; нет скрытой тихой остановки. **Метод — ручной QA/`selfTest` (см. ниже)** |
| QAS-L17 | **Компактный «Период» (REQ-L7):** высота sticky-блока сокращена до целевого ориентира (≤ ~2 строк), функциональность (от/до, пресеты, выбор клиента) сохранена. **Метод — ручной QA/`selfTest`** |
| QAS-L18 | **Sticky-тоггл (REQ-L8):** закрепление можно выключить; состояние персистится в `localStorage` и восстанавливается после reload. **Метод — ручной QA/`selfTest`** |
| QAS-L19 | **Продюсер (owner #2):** `hldsmon_max_mb=0` = unlimited (запись не останавливается по размеру) + опциональный клапан; plugin-тест `test_log_writer` |
| QAS-L20 | **Чтение без лимита (REQ-L1):** приём файла произвольного размера не отклоняется по размеру; память не растёт O(файл) |

**Метод проверки UI-QAS (M2).** QAS-L16/L17/L18 **не покрываются `make test`** (там C++ host-тесты + Python `unittest`; `test_webui.py` проверяет лишь отдачу `<!DOCTYPE html>`, страница — браузерная). Механизм: **ручной QA-чеклист** (фиксируется в `docs/plan.md`/журнале) **и/или** прогон существующего page-`selfTest` (`webui/index.html:4144`) с новыми кейсами (компактный период, sticky-тоггл-персист, индикаторы FTP-окна). Python-статическая проверка допустима только для реально проверяемого (напр. наличие HTML/JS-ключей тоггла).

### 28.9 Решения владельца (закрыто, 2026-10-13)

1. **(scope/B1, развилка f) — РЕШЕНО (owner, §16 O9).** Читается **всё**; серверный период/window — **UX** (удобство/скорость), **не** потолок. Полный день обрабатывается (coarse overview + окна по требованию); `from`/`to`/`window` — механизм, а не ограничение принимаемого размера.
2. **(a) Локальный приём — РЕШЕНО (owner, §16 O4).** Поток тела → spool; отказ от `f.text()` (прямой `Blob`); BOM/CRLF-обработка.
3. **(b) FTP — РЕШЕНО (owner, §16 O5).** Сегментный spool; **без cap по умолчанию**; улучшенная/очевидная механика окна (REQ-L6).
4. **(c) Политика лимита — РЕШЕНО (owner, §16 O6).** **Без лимита чтения по умолчанию**; опциональный клапан (`--max-log-mb`, default `0`); продюсер `hldsmon_max_mb` default **`0`=unlimited** с опциональным клапаном.
5. **(d) Security/spool — РЕШЕНО (owner, §16 O7).** Spool на диске, budget как ресурсный предохранитель, crash-safe cleanup; loopback-дефолт; риск disk-fill задокументирован как решение владельца.
6. **(e) Сигнал о лимите — РЕШЕНО (owner, §16 O8).** По умолчанию нет отказа/обрезки; при включённом клапане — explicit `{"code":"log_too_large"}`.
7. **(не развилка, на план)** Точные UX-пороги (целевая высота «Период», layout FTP-окна, вид прогресса) и дефолты деталей — фиксируются в `docs/plan.md`; отдельного решения владельца не требуют.

### 28.10 План → `docs/plan.md`, «Стадия 12 — `large-logs`».

Обязательные артефакты плана: **`src/bootstrap.cpp`** (default `s_val_max_mb[]` `"512"`→`"0"` (`:55`); fallback `cvar_f(..., 512.0f)`→`0.0f` (`:308`)); `README.md` (cvar-таблица) + design §7/§12; `tools/analyzer/webui.py`; `tools/analyzer/webui/index.html`; `tools/analyzer/hldsmon.py` (оконный разбор + coarse-агрегатор); `tools/analyzer/tests/`. **`src/log_writer.cpp` — НЕ трогается** (`0`=unlimited уже реализован).

### 28.11 Раздел ревью дизайна (oracle)

**Вердикт: PASS-WITH-REVISIONS.** Направление верное (scope — `tools/analyzer` **на момент ревью**; далее расширен владельцем на плагинный дефолт `hldsmon_max_mb`; схема P1/`KNOWN_TYPES` не трогаются; stdlib-only; `iter_records` и temp-файл как основа; аддитивность FTP-меты). Существенные правки: ингест-стриминг **не достигает** цели без серверного окна (B1), а сегментный spool сам по себе O(n²) не снимает (B2). Все замечания сложены в §28.1/§28.3/§28.4/§28.5/§28.6/§28.8/§28.9. **Легенда ID:** таблица ниже — **R1** (первый дизайн-ревью, pre-owner); второй targeted re-review (post-owner) — **R2** (ниже). В тексте §28 ссылки `B#/M#/m#` **без префикса = R1**; с префиксом `R2-` — ко второму раунду; где важно — указано явно.

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| B1 | Blocking | Ингест-стриминг без серверного window не даёт «весь день» (`analyze_records` материализует всё) | **Внесено** — §28.3 п.1: `/api/analyze` получает `from`/`to`/`window`, материализуется только окно, coarse overview + дозапрос окон, фронт переделан; ingest-only = сокращение scope (§28.9(1)/§16 O9) |
| B2 | Blocking | Ложное «сегментный spool снимает O(n²)»; refresh/load-earlier пере-анализируют всё | **Внесено** — §28.1 (O(n²) на FTP), §28.3 п.4, INV-L1 (анализ O(window)), §28.4 fork (b) |
| M1 | Med | Spool на tmpfs = RAM | **Внесено** — §28.3 п.13 (диск-backed spool-dir, free-space, бюджет); QAS-L8 |
| M2 | Med | Неоднозначная семантика единого cap; инвариант cap>файла; 700 МБ vs 512 МиБ | **Внесено** — §28.3 п.7, §28.5 INV-L2, §28.9(4) |
| M3 | Med | `cap_hit` только FTP; local 413 → reset/нечитаемый ответ | **Внесено** — §28.3 п.8, §28.5 (over-cap контракт), QAS-L2 |
| M4 | Med | Handle для export/bundle не определён; drop по позиционному индексу расходится при окне; FTP-путь отключает export/bundle | **Внесено** — §28.3 п.1/п.6, §28.5 (таблица), QAS-L6 |
| M5 | Med | ThreadingHTTPServer-потоки не ограничены | **Внесено** — §28.3 п.13 (семафор/лимит сессий/read-timeout); QAS-L12 |
| M6 | Med | `delete=False`+`finally` не crash-safe | **Внесено** — §28.3 п.13 (startup-cleanup + age-sweeper + BrokenPipe); QAS-L9 |
| M7 | Med | `Blob`-путь: BOM, CRLF, склейка файлов | **Внесено** — §28.3 п.3, §28.6, QAS-L11 |
| m1 | Low | QAS-L1/L3 неверифицируемы; нет QAS на память анализа | **Внесено** — QAS-L1/L3 (`ru_maxrss`/окно), новый QAS-L10 |
| m2 | Low | «≈512 МБ» V8 неточно | **Внесено** — §28.1: помечено «≈» (лимит длины строки UTF-16) |
| m3 | Low | `cap_hit` редко срабатывает под дефолтами | **Внесено** — §28.3 п.7/п.8 (клапан чтения **off by default**; `cap_hit` выставляется только при включённом клапане; продюсер default `0`=unlimited) |
| m4 | Low | `_ftp_meta.bytes`/`reached_start` — удалённый файл vs спул | **Внесено** — §28.3 п.8, §28.5 INV-L5 |
| m5 | Low | Ротация суток в сегментной модели | **Внесено** — §28.3 п.4, QAS-L14 |
| Missed fork | Med | Уровень оконности; где spool и бюджет; lifecycle handle; инвариант cap; смена суток; нет QAS памяти анализа | **Внесено** — §28.4 fork (f)/(g) (ii-a/b/c), §28.3 п.6/п.13, INV-L2/L4, QAS-L10/L14 |

**Отложено:** нет. Отклонённых замечаний не выявлено; единственное «сокращение scope» (ingest-only) **отклонено решением владельца** — читается всё (window — UX; §16 O9). **Addendum ниже** фиксирует решения владельца поверх verdict.

**Fine (не менять):** scope `tools/analyzer` + плагинный дефолт `hldsmon_max_mb` (owner #2); схема P1/`KNOWN_TYPES`; stdlib-only (INV-6); `iter_records` (`hldsmon.py:163-187`) и temp-файл как основа; аддитивность FTP-меты и `reached_start`; security-рамка loopback default + opt-in.

**Post-owner-answers addendum (2026-10-13).** Решения владельца сложены: §28.3 (REQ-L1..REQ-L8 + решения п.7–12), §28.5 (scope/contracts/INV-L2), §28.6 (`hldsmon_max_mb` + UI), §28.7, §28.8 (QAS-L16..L20), §16 (O4–O9 закрыты). Ключевые изменения против reviewed-версии: лимиты чтения/записи **по умолчанию не отклоняют** (опциональный клапан выключен), память ограничена **стримингом**, window — **UX**; добавлены UI-требования (компактный «Период», опциональный sticky, механика FTP-окна). Повторное ревью `oracle` выполнено (R2 — PASS-WITH-REVISIONS; R3 — PASS, design-gate закрыт; §28.11).

**R2 — второй targeted re-review (oracle, 2026-10-13) — PASS-WITH-REVISIONS (гейт не закрыт):** B1/B2 (blocking) + M1–M4 + m1–m3. Все сложены:

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| B1 | Blocking | Продюсерский дефолт цитировал не тот файл; реальный дефолт (`src/bootstrap.cpp`) отсутствовал в scope | **Внесено** — §28.2 (факт: `src/bootstrap.cpp:55`/`:124`/`:308`; `log_writer.cpp` **не меняется** — `0` уже unlimited), §28 blockquote, §28.3 п.9, §28.5, §28.6, §28.10 |
| B2 | Blocking | Coarse overview требовался §28.3 п.1, но исключался §28.7; контракт не определён; INV-L2 не установлен | **Внесено** — §28.3 п.1 (контракт coarse + однопроходный стриминговый агрегатор; memory O(window)/O(players)/O(drops), **без** O(records)), §28.5 (строка контракта + INV-L2), §28.7 (ii-b **в scope**) |
| M1 | Med | Устаревший decision-state header (`docs/design.md:4`) | **Внесено** — строка 4: owner-решения закрыты (2026-10-13); unlimited+valve; window=UX |
| M2 | Med | UI-QAS L16–L18 неверифицируемы `make test` | **Внесено** — §28.6 (убрано «UI-тесты» как Python-факт), §28.8 (метод — manual QA / page-`selfTest` `index.html:4144`) |
| M3 | Med | INV-L1 требовал обязательные `from`/`to`, а они опциональны | **Внесено** — §28.3 п.1 (default-окно применяется всегда) + INV-L1 переформулирован (§28.5) |
| M4 | Med | «backward-compat для малых файлов» преувеличен; не определено вытеснение handle | **Внесено** — §28.5 (реальный контракт: малый файл → одно окно; большой → coarse+окна; `{"code":"session_expired"}` при вытеснении) |
| m1 | Low | Две локации BOM-фикса | **Внесено** — §28.2/§28.3 п.3: BOM снимается **при приёме** (одно место); `hldsmon.iter_records` не трогаем |
| m2 | Low | Ключ `(t,userid)` может коллизировать | **Внесено** — §28.3 п.1/п.6, §28.5, QAS-L6: ключ `(run,t,userid)` |
| m3 | Low | CRLF уже обрабатывается `strip()` | **Внесено** — §28.2: CRLF покрыт `hldsmon.py:174`; M7-забота — только line-splitting Blob-пути |

**Отложено:** нет. **R3 — финальное targeted re-review (oracle, 2026-10-13) — PASS; design-gate ЗАКРЫТ.** Остаточный NEW-1 (§28.6, строка про `iter_records`/BOM) исправлен: BOM снимается **при приёме**; `hldsmon.iter_records` не трогаем.

---

## 29. Мгновенный период и даунсэмплинг графиков (Стадия 13 — `instant-period`)

> Стадия владельца (запрос: «смена периода на локальном файле не должна перезагружать весь файл и пере-разбирать его целиком; для больших периодов графики группируют точки, а режим отрисовки выбирает пользователь»). Расширения владельца: **(2) селективные секции** (запрос только нужных блоков payload; выключенные не считаются) и **(3) двухуровневый (детальный) таймлайн** (точная правка малого окна). Дизайн-ревью — `oracle` (**R1: PASS-WITH-REVISIONS — B1/B2 + M1–M4 + m1–m6 сложены, §29.11**); план-ревью — `momus`. **Scope:** `tools/analyzer` (`webui.py`, `hldsmon.py`, `webui/index.html`). **`src/` не трогается; схема JSONL P1 и `KNOWN_TYPES` не меняются; инструмент остаётся stdlib-only (INV-6).** FTP-**период** не меняется (клиентская фильтрация), но **форма рядов FTP меняется на бакетные тройки** (общий `_payload_from_agg`) — см. §29.5/§29.7. Зависит от §28 (диск-spool, `handle`, coarse-overview, `_source_paths`, TTL/бюджет) и §16 O4–O9.

### 29.1 Контекст и проблема

Стадия 12 (§28) ввела диск-spool, серверный `handle` и серверные окна. Но при смене периода на **локальном** файле остаются две стоимости на каждый запрос:

- **Повторная загрузка тела.** `_handle_analyze` (`webui.py:2102-2133`) безусловно вызывает `self._read_body()` (`:2105`) — ветки на `handle` нет, поэтому уже принятый под `handle` spool не переиспользуется. Тело приходит заново, а `_read_body_locked` (`:1964-2019`) стримит его в **новый** spool-файл (`_spool_create` `:1986`); прежний, идентичный spool остаётся на диске нетронутым (до TTL). Фронт: `applyPeriodValues` (`webui/index.html:2389-2393`) для `source === "local"` снова зовёт `fetchAnalyze` (`:2391`), который шлёт `state.fileBlob` (весь файл) через XHR (`:1408`/`:1417`). Для ~740 МБ это десятки секунд на **каждую** смену периода.
- **Повторный полный разбор.** Даже когда запрошено окно, `_analyze_iter` (`webui.py:857-923`) прогоняет **все** `(record, run)` всего spool через `StreamAggregator` (`:885`/`:904-909`) и лишь затем оставляет окно. Память при этом ограничена (§28.3 п.1), но **время — O(файла) на каждый запрос**: окно материализуется, но файл всё равно читается и парсится целиком.

Итог: одноразовый `handle` + spool стадии 12 **уже даёт** материал для переиспользования (`_source_paths` `webui.py:2138-2148` отображает handle → путь(и) spool; `/api/export`/`/api/bundle` уже принимают `handle`), но **путь периода этим не пользуется** и пере-разбирает файл каждый раз. Это чистый performance-дефект поверх корректной стадии 12, а не новая функциональность.

### 29.2 Проверенные технические факты (ground truth)

- **Analyze:** `_handle_analyze` (`webui.py:2102-2133`) — `_read_body()` (`:2105`), прогресс `_on_progress` (`:2114`), `coarse_overview(entry.path, …)` (`:2121`), `payload["handle"] = entry.handle` (`:2132`). `_read_body` (`:1946-1962`)/`_read_body_locked` (`:1964-2019`) — `_UPLOAD_SEM` (`:1954`), `_spool_create` (`:1986`), `receive_body` (`:1992`).
- **Spool/handle и утечка (R1-B2):** `SpoolHandle` (`webui.py:235-244`, `__slots__` = `("handle","path","created","size")`); `_SPOOLS` (`:249`), `_SPOOL_LOCK` (`:248`), `_SPOOL_BUDGET` (`:247`). `spool_get` (`:383-396`) делает **ленивый** TTL-reap лишь при повторном доступе; **`spool_sweep` (`:399-411`) сервером НЕ вызывается** — в `main` есть только `spool_startup_cleanup` (`:414-427`, вызов `:2507`); заброшенные handle (и будущий `entry.analysis`) не освобождаются. `SPOOL_TTL_S=3600` (`:81`), `SPOOL_SESSION_MB=2048` (`:73`), `SPOOL_TOTAL_MB=8192` (`:77`).
- **Run-производная (R1-B1):** `run` выводится **из позиции в потоке** — `_iter_spool_pairs` (`webui.py:938-952`) инкрементит `run` после каждого `shutdown`/`crash`; `_attach_runs` (`hldsmon.py:342-355`) — так же; `session_key` (`hldsmon.py:379`) = `"<run>:u<userid>"`/`"<run>:i<id>:g<gen>"` и входит в ключи `client_series` (`:655`), `client_identities` (`:777`), `player_groups` (`:838`), drop-ключ `(run,t,userid)` (§28.3 п.6), `focus_points(..., runs=…)` (`:1588`). Окно, начинающееся после границы run, без seed даст **неверный run** → расхождение с полным сканом.
- **Анализ и состав payload (R1-B2):** `_analyze_iter` (`webui.py:857-923`) — полный проход по spool; `coarse_overview` (`:964-973`); `window_records` (`:983-997`). `_payload_from_agg` (`:804-854`) вкладывает в **один** payload тяжёлые структуры: полный `drops` (O(drops); каждый несёт вложенный `window`-срез), `clients`, `sessions`/`player_groups`, `client_identities`, `config`, `events`/`exclusions`, `anomalies` ⇒ coarse-payload **не** O(buckets), а O(drops)+O(players)+O(sessions). `SERIES_MAX_POINTS` (`hldsmon.py:168` = `webui.py:119`), `ANALYZE_DEFAULT_WINDOW_S` (`webui.py:116`).
- **Парсер/offsets (R1-m1):** `hldsmon.iter_records` (`hldsmon.py:180-…`) открывает **текстовый** файл; `on_progress(fh.tell())` (`:204`) троттлится `PROGRESS_INTERVAL_S=0.25` (`:173`); в текстовом режиме `tell()` — непрозрачный cookie, не гарантированно байтовый offset ⇒ для seek нужен **бинарный** режим.
- **Часы продюсера (R1-M1):** `t` пишется из `platform_wall_seconds()` (`src/bootstrap.cpp:331`/`:456`; `src/hooks.cpp:74`/`:137`/`:197`/`:205`/`:441`), реализованного через `clock_gettime(CLOCK_REALTIME)` (`src/platform_linux.cpp:54-56`) — **стенные** часы, могут «шагнуть» назад (NTP) ⇒ неубывание `t` **не** гарантировано.
- **Даунсэмплинг:** `_Decimator` (`hldsmon.py:1742-1768`) — прореживание шагом `rows[::stride]` (`:1763`); **не** min/max/avg. `StreamAggregator` (`hldsmon.py:1815+`) — `add(rec, run)` (`:1865`) обновляет **все** структуры безусловно (нет гейтинга секций).
- **Переиспользование (прецедент):** `_source_paths` (`webui.py:2138-2148`); `_handle_export` handle-ветка (`:2177-2199`); `_handle_bundle` (`:2218-2236`); `session_expired` 410 (`_expired` `:2135-2136`). FTP-remote идёт через тот же `_payload_from_agg`: `_ftp_analyze` (`:1621`), `_handle_ftp_refresh` (`:2312`).
- **Прочее:** `ThreadingHTTPServer` (поток на запрос), аутентификации нет (loopback default); `PROTOCOL_VERSION=1` (`:553`); прогресс `_progress_set`/`_progress_finish` (`:1481`/`:1504`), `/api/analyze/progress` (`:2057-2060`).
- **Фронт — потребители плоских рядов (R1-M3):** `sliceByIdx` (`index.html:925`), `seriesIndices` (`:929`), `aggregateSeries` (`:945`; прямой `arr[i]`), `mergeSessions` (`:1015`), `computeView` (`:1032-1118`), player-group merge (`:1123-1167`, через `mergeSessions`), `buildCharts` (`:3187`; `series.fps`/`frame_ms_min|avg|max`/`cpu_pct`/`rss_kb`/`players`/`real_players`/`in_kbps`/`out_kbps`), `buildClientCharts` (`:3236`; `cs.ping`/`loss`/`choke`/`rel_fill`/`idle_ms`/`delta_seq`/`dloss`/`flow_in_kbps`/`flow_out_kbps`), `buildAllChart` (`:3370`) + `ALL_DATASETS[].values(s)` (`:3076-3092`), `alignToServerTime` (`:3296`), `alignHostToServerTime` (`:3324`), `fillAllChart` (`:3424`), `aggregateSeries(client_series[s])` (`:4141`), `selfTest` (`:4444`). Все ожидают **плоские числовые** массивы. Плюс фронт-поток `fetchAnalyze` (`:1396-1442`) — `state.handle` (`:1421`), `state.fullRange` (`:1426`); `downloadCsv` уже обрабатывает `session_expired` (`:1459`); `localProgressStart` (`:1327`/`:1406`).

### 29.3 Решение (design-level)

Две базовые развилки (решения владельца): **(B)** мгновенный период через серверную сессию анализа на `handle`; **(C)** единый бакетный контракт `min/max/avg` + режим отрисовки на фронте. Плюс два расширения владельца: **селективные секции** и **двухуровневый (детальный) таймлайн**.

**Требования стадии (REQ-I):**

| ID | Требование |
|---|---|
| **REQ-I1** | Повторная смена периода на локальном файле **не пере-загружает тело** и **не пере-разбирает файл целиком**: данные периода обслуживаются из серверной сессии по `handle` (O(окна), не O(файла)). |
| **REQ-I2** | **Первый** (телесный) запрос анализа строит coarse-overview И разрежённый индекс `time→byte-offset→run` за **один** проход по spool. |
| **REQ-I3** | Оконный запрос `[from,to]` обслуживается **seek'ом** в spool: O(окна + гранулярность индекса); **run-счётчик окна seed'ится из индекса**, поэтому оконные `client_series`/`focus_points` совпадают с полным сканом. `drops`/`clients`/`sessions`/`client_identities` — **coarse-only** (§29.5; в оконном ответе их нет), их `run`/`session_key` строятся в полнодневном проходе. |
| **REQ-I4** | Для больших периодов сервер **группирует** точки; по каждому ряду/метрике сервер **всегда** отдаёт `min/max/avg` (+`n`) на бакет — унифицированный контракт. |
| **REQ-I5** | Backend **всегда** отдаёт `{min,max,avg,n}`; **отрисовка зависит от типа графика** (O12): на **общем «Все датасеты»** — только **один** из `min`/`max` за раз (band запрещён; тоггл `min`↔`max`, default `max`, персист `localStorage`); на **индивидуальных per-metric** графиках — **все три** (`min–max` band + линия `avg`). Смена режима/тоггла — без запроса. |
| **REQ-I6** | **Обратная совместимость:** телесная форма `/api/analyze` (первый запрос) и handle-форма `/api/export`,`/api/bundle` сохраняются; вытесненный handle ⇒ `session_expired` + пере-загрузка. |
| **REQ-I7** | Маркеры drop/hitch/event остаются выровненными по времени при любой группировке (эпохально-выровненные бакеты + точный `t` маркеров). |
| **REQ-I8** | Сохраняется ограничение памяти: индекс разрежён (O(duration/T), **не** O(records)); **сессия кэширует только индекс + краткую сводку + бакетные coarse-ряды** (тяжёлые списки не кэшируются) с per-handle бюджетом; полный список записей не кэшируется. |
| **REQ-I9** | **Multi-run корректность (оконные артефакты):** оконные `client_series` и `focus_points` равны полному скану даже когда окно начинается после `shutdown`/`crash` — индекс несёт run-счётчик на offset, `_serve_window` его seed'ит. **`drops`/`clients`/`sessions`/`client_identities` — coarse-only** (§29.5): их `run`/`session_key`/drop-ключ `(run,t,userid)` строятся в кэшированной полнодневной сводке и не пересчитываются в окне (R2-N-M1). |
| **REQ-I10** | **Селективные секции:** `/api/analyze` (**обе** формы) принимает `sections=…`; **default = все секции** (backward-compatible), выбор персистится **глобально** в `localStorage` (`hldsmon.ui.*`) (O15); сервер **пропускает работу** по выключенным секциям; выключенные блоки не запрашиваются и не рисуются. Re-enable при активном окне: **лёгкая** секция → оконный запрос `?handle&from&to` с расширенным `sections` (**O(окна)**, INV-I3, **не** полный проход); **тяжёлая** → **coarse-recompute** (handle без `from`/`to`; один O(файла) проход, `coarse:true`, без кэша) (R3-m3/m2). **Канонический набор ключей** — §29.3 п.11 (R3-m1). |
| **REQ-I11** | **Двухуровневый таймлайн:** главный brush — весь лог; второй («детальный») brush — текущее окно; **автопоказ, когда `span(window) < DETAIL_TIMELINE_RATIO · span(fullRange)`** (`DETAIL_TIMELINE_RATIO = 0.05` — конфигурируемая константа), иначе только главный (O14); ручки детального brush точно настраиваются при малом окне; правка задаёт `[from,to]` внутри окна; повторный drag = вложенный зум; всё через мгновенное handle-обслуживание (без повторной загрузки/разбора). |

**Инварианты стадии (INV-I):**

| ID | Инвариант |
|---|---|
| **INV-I1** | Индекс разрежён: сэмпл = `(t, segment_idx, byte_offset, run)`; ≤ `INDEX_MAX_ENTRIES`; сэмплирование по времени (≥ `INDEX_STEP_S`) и/или байтам (≥ `INDEX_STEP_BYTES`); при cap — **удвоение шага** (индекс остаётся отсортированным); память O(точек), **НЕ** O(records)/O(файл). |
| **INV-I2** | Память анализа — O(окна) + O(бакетов); полный список сырых записей не удерживается (наследует INV-L2). |
| **INV-I3** | Временна́я сложность оконного запроса — O(окна + T); телесный путь — O(файла) **один раз**; при **немонотонном `t`** (INV-I8) оконный путь деградирует до O(файла) (fallback), но корректен. |
| **INV-I4** | Корректность окна: `from`/`to` (включительно) дают то же множество записей, что полный скан/клиентская фильтрация; **оконные run-чувствительные артефакты** (`client_series`, `focus_points`) несут тот же `run`/`session_key`, что полный скан. Coarse-only списки (`drops`/`clients`/`sessions`/`client_identities`) в оконный ответ не входят (их run-ключи — из полнодневной сводки) (R2-N-M1). |
| **INV-I5** | stdlib-only; схема P1/`KNOWN_TYPES` без изменений; анализ работает и на Windows (без POSIX-only вызовов); аутентификации нет, `handle` — непрозрачный случайный токен. |
| **INV-I6** | **Per-handle сессия ограничена (R1-B2):** `entry.analysis` хранит только индекс + краткую сводку + бакетные coarse-ряды; тяжёлые списки (`drops`/`clients`/`sessions`/`client_identities`/`config`/`events`/`anomalies`) **не** кэшируются; per-handle байтовый cap `ANALYSIS_SESSION_MB`; превышение ⇒ memo/coarse-ряды сбрасываются, индекс сохраняется. |
| **INV-I7** | **TTL-sweeper реально исполняется (R1-B2):** освобождает файл+budget+`entry.analysis` по TTL (фоновый поток либо тик по запросу); заброшенные handle не текут. |
| **INV-I8** | **Монотонность `t` валидируется** дешёво в проходе; при нарушении индекс помечается немонотонным и `_serve_window` откатывается на полный скан (корректность важнее скорости) (R1-M1). |
| **INV-I9** | **Offsets — истинные байтовые** (бинарный режим; снятие **до** `readline()`): seek попадает точно на начало строки; `_seek_aligned` — только защитный fallback (R1-m1/m5). |
| **INV-I10** | **Адаптивная бакетировка** потоковая: старт с `min_resolution`; при > 2·`CHART_MAX_POINTS` — слияние соседних бакетов удвоением ширины (ассоциативно min/max/avg/n); итог ≤ `CHART_MAX_POINTS`; эпохально-выровнено (R1-M2). |

**Решения:**

1. **Разрежённый индекс `time→byte-offset→run`, строится в ОДНОМ проходе с coarse (B/R1-B1/m1/m5/m6/M1).** Новый **аддитивный** API `hldsmon.iter_records_offsets(path)` открывает файл **в бинарном режиме** (`open(path,"rb")`), снимает `offset = fh.tell()` **до** `readline()` (начало строки) и декодирует строку (`utf-8`+`errors="replace"`, BOM снимается при offset 0), повторяя терпимые пропуски `iter_records`; сам `iter_records` **не трогается**. Сэмпл индекса — `(t, segment_idx, byte_offset, run)`, где `run` = число `shutdown`/`crash` **строго до** этого offset (run, которому принадлежит запись). Сэмплирование: при `(t − last_t) ≥ INDEX_STEP_S` **или** `(offset − last_off) ≥ INDEX_STEP_BYTES` (что раньше); первая запись — всегда; при достижении `INDEX_MAX_ENTRIES` — **удвоение обоих шагов** (append-порядок сохраняет отсортированность ⇒ бинарный поиск валиден). Дефолты **решены**: `INDEX_STEP_S=5`, `INDEX_STEP_BYTES=1 МиБ`, `INDEX_MAX_ENTRIES=20000`. В том же проходе **валидируется неубывание `t`** (`t < last_t` ⇒ `index.monotonic=False`).
2. **Сессия анализа на `handle` — ограниченный кэш (B/R1-B2).** `entry.analysis` (новое поле `SpoolHandle.__slots__`) хранит **только**: индекс (O(entries)), `t_lo/t_hi`, `segments`, `monotonic`, **краткую сводку** (counts_by_type/period/players count/sessions count/drop_count/top_drop_reasons/hitch/lag/log_end/anomalies_excluded count — всё O(1)/O(types)/O(reasons)), **бакетные coarse-ряды** (server/client/host, ≤ `CHART_MAX_POINTS`) и memo окон (LRU=8). **Тяжёлые списки** — полный `drops` (с nested `window`), `clients`, `sessions`/`player_groups`, `client_identities`, `config`, `events`/`exclusions`, `anomalies` — **НЕ кэшируются** (INV-I6). Per-handle байтовый cap `ANALYSIS_SESSION_MB` (рек. 16 МиБ): при превышении сбрасываются memo/coarse-ряды, индекс остаётся. Тот же TTL, что у spool.
3. **Реальный sweeper (R1-B2-i).** Сервер запускает **периодический** `spool_sweep()` (фоновый daemon-поток с интервалом `SPOOL_SWEEP_S`, рек. 60 с; либо тик на каждом `/api/*`), освобождающий на TTL spool-файл+budget+`entry.analysis`. Сейчас `spool_sweep` (`:399`) сервером не вызывается (только тесты) — это чинится (INV-I7). `spool_get` остаётся ленивым быстрым путём; `spool_startup_cleanup` — для крашей. **Residual (R2-N-m3):** на Windows удаление открытого файла может дать `PermissionError` (`_silent_remove` → `False`) — eviction откладывается до `spool_startup_cleanup` (безвредно: spool-файлы открываются лишь на время запроса).
4. **Новая форма запроса (NO body) (R1-m2).** `POST /api/analyze?handle=<id>&from=&to=&window=&sections=&job=` — **без тела** → обслуживается из сессии. **Присутствие `Content-Length>0`/непустого тела ⇒ HTTP 400 `{"ok":false,"code":"handle_body_conflict"}`** (не «осушать/игнорировать»). Handle резолвится через `spool_get` (**локальный** spool); FTP-сессионные handle этим путём **не** принимаются. `_UPLOAD_SEM` не берётся (upload-слоты не занимаются).
5. **Обслуживание окна seek'ом (B/R1-B1/R2-N-m2).** Бинарный поиск наибольшего сэмпла с `t ≤ from`; `seek(offset)`; `run` **seed'ится** значением сэмпла; чтение идёт от сэмпла, и **run инкрементится по каждому `shutdown`/`crash` ДЛЯ ВСЕХ прочитанных записей, включая отфильтрованные по времени** (границы run до `from` учтены); включаются записи `lo ≤ t ≤ hi`; стоп на первой `t > to`. **Если ни один сэмпл не имеет `t ≤ from`** (окно начинается до первой записи) ⇒ seed `run=0`, `offset=0` (начало spool), скан с начала сегмента. Штатно offset — начало строки (снят **до** `readline`), частичной строки нет; `_seek_aligned` — защитный fallback при изменённом файле. При `index.monotonic == False` — **fallback на полный скан** `_iter_spool_pairs` (корректно, O(файла)).
6. **Memo-кэш (R1-m4).** LRU на сессии, ключ `(from, to, window, sections)` → фрагмент; размер **решён: 8** записей. Повтор — из RAM.
7. **Единый бакетный контракт, адаптивная бакетировка (C/R1-M2/O11).** Метрика → `{min, max, avg, n}` + общий `t[]`. Схема потоковая: старт с `min_resolution` (**решено: 1 с**), эпохально-выровненные бакеты; при > 2·`CHART_MAX_POINTS` — слияние соседних бакетов удвоением ширины: `min=min(a,b)`, `max=max(a,b)`, `avg=(a.avg·a.n + b.avg·b.n)/(a.n+b.n)`, `n=a.n+b.n` (ассоциативно, null-aware); итог ≤ `CHART_MAX_POINTS` (**решено: 2000**; переименование `SERIES_MAX_POINTS`, `hldsmon.py:168`). Тройка **сохраняет пики** (в отличие от stride-`_Decimator`, `hldsmon.py:1763`); не требует известного span. Пустой бакет ⇒ `null` (никогда 0) — правило «null ≠ ноль» (host-caveats).
8. **Отрисовка зависит от типа графика (C/O12).** Backend всегда отдаёт тройку `{min,max,avg,n}`; **рендер различается**:
   - **Общий график «Все датасеты»** — показывает **только один** из `min`/`max` за раз (**никогда оба; band/min+max запрещён**): пользовательский тоггл `min ↔ max`, **default `max`** (пики видны), персист `hldsmon.ui.*`; ряды читают выбранную проекцию (`view.series[key]`).
   - **Индивидуальные per-metric графики** — **всегда рисуют все три**: `min–max` band (fill) + линия `avg`.
   Payload одинаков для всех режимов/тоггла ⇒ смена мгновенна (без запроса).
9. **Выравнивание маркеров событий.** drop/hitch/event/config-маркеры несут точный `t` и рисуются на той же оси; эпохально-выровненные бакеты ⇒ маркеры **не** снапятся к центрам — выравнивание сохраняется в любом режиме (QAS-I8).
10. **Полный vs оконный ответ (R1-M4/R2-N-M1/N-M2).** Оконный ответ несёт **только**: `handle`, `window`, `window_count`, `coarse:false`, `series`/`client_series`/`host_series` (окно), `focus_points` (окно), **краткую сводку**. **Тяжёлые полнодневные списки** (`drops`, `clients`, `sessions`/`player_groups`, `client_identities`, `anomalies`, `events`/`exclusions`, `config`, `host_caveats`) отдаются **только в coarse/первом** ответе — их `run`/`session_key`/drop-ключ `(run,t,userid)` строятся в полнодневном проходе и **не зависят от окна** (N-M1). Фронт **сливает** оконный ответ с сохранённым coarse: `client_identities`/`sessions`/`drops`/`events`/`config`/`host_caveats`/`clients`/`anomalies` остаются из coarse; заменяются `series`/`client_series`/`host_series`/`focus_points`/`window`/краткая сводка. **Сводка оконного ответа (N-M2):** скаляры (`period`, counts_by_type, players, sessions, drop_count, top_drop_reasons, hitch/lag, log_end, anomalies count) — **кэшированная полнодневная** (из индекса `t_lo/t_hi`), **НЕ** пересчитывается из записей окна; `period` = `[t_lo, t_hi]` всего spool. Фронт **не** обновляет `state.fullRange` при `coarse:false` (иначе главный brush схлопнется к окну). Точная таблица полей — §29.5.
11. **Селективные секции (REQ-I10/R2-N-M3/R3-m1/m2/m3/O15).** Параметр `sections=` (comma-separated) на **обеих** формах; **default = все секции** (O15), выбор персистится **глобально** (`hldsmon.ui.*`). **Канонический набор ключей (R3-m1)** (ключ = имя поля payload; `identities` **не** используется): `digest`, `series`, `host_series`, `client_series`, **`client_identities`**, `clients`, `sessions`, `drops`, `anomalies`, `events`, `config`, `focus_points`. Сервер **гейтит** соответствующие аккумуляторы в `StreamAggregator.add`/`finish`; `_payload_from_agg` включает только запрошенные. **Обязательное ядро** (для мгновенного окна) — spool+handle+индекс+`t_lo/t_hi`; `series`/`client_series`/`host_series` включены по умолчанию.
    Re-enable ранее выключенной секции:
    - **Лёгкая** (сводка/бакеты, вкл. `series`/`client_series`/`host_series`/`focus_points`) при активном окне → **оконный запрос `?handle&from&to`** тех же `[from,to]` с расширенным `sections`; сервер обслуживает seek'ом **O(окна)** (INV-I3) — **НЕ** полный проход по файлу (R3-m3; «пересчёт одного прохода по spool» в R2 относится только к первичному построению ядра, а не к повторному оконному запросу лёгкой секции).
    - **Тяжёлая** (`drops`/`clients`/`sessions`/`client_identities`/`anomalies`/`events`/`config`) не бывает в оконном ответе → **coarse-recompute**: фронт шлёт handle-запрос **без `from`/`to`** с расширенным `sections`; сервер выполняет **один O(файла) проход** и возвращает **`coarse:true`** с полными списками (R3-m2). Тяжёлые **не кэшируются** (INV-I6). **Направление merge (R3-m2):** сохраняются текущие **оконные** `series`/`client_series`/`host_series`/`focus_points`/`window`/период; **заменяются только тяжёлые таблицы** (`drops`/`clients`/`sessions`/`client_identities`/`anomalies`/`events`/`config`). Оконный ответ списки не несёт.
12. **Двухуровневый таймлайн (REQ-I11/R2-N-M2).** Главный brush — домен `state.fullRange`; его выбор = `[from,to]` (окно) и подсвечивается на главном. Детальный brush — домен `[from,to]`; его ручки задают поддиапазон; commit сужает `[from,to]`, детальный re-доменизируется; повторный drag = вложенный зум. Синхронизация: главный показывает окно (highlight), детальный — окно-домен + подвыбор; правка любого ⇒ handle-запрос окна (мгновенно); **оконный ответ (`coarse:false`) НЕ трогает `state.fullRange`** — домен главного brush остаётся полнодневным `[t_lo,t_hi]` (иначе он схлопнется к окну). Композиция с бакетировкой: короче span ⇒ мельче бакеты (адаптивная схема, п.7) → детальнее, вплоть до сырого разрешения. **Автопоказ детального brush (O14, решено):** показывается автоматически, когда `span(window) < DETAIL_TIMELINE_RATIO · span(fullRange)` (`DETAIL_TIMELINE_RATIO = 0.05` — конфигурируемая константа); иначе — только главный brush. Повторный detail-drag = вложенный зум.
13. **Обратная совместимость и FTP (R1-M4).** Телесная форма `/api/analyze` — без изменений (первый запрос, возвращает `handle`). `/api/export`,`/api/bundle` — без изменений (уже handle-based). **FTP: ПЕРИОД остаётся клиентским** (индекс/сессия для FTP не строятся), **НО форма рядов меняется на тройки везде** — FTP-remote идёт через тот же `_payload_from_agg` (`_ftp_analyze` `:1621`), поэтому FTP-ответ также отдаёт бакетные `series`/`client_series`/`host_series`; фронт рисует тройки и для FTP. `PROTOCOL_VERSION` 1→2. Схема P1/`KNOWN_TYPES` без изменений.
14. **Конкурентность.** Построение индекса — под per-session `threading.Lock` (он же memo). Оконные запросы поверх неизменяемого индекса + независимых seek безопасны. Sweeper: `spool_get`/`_spool_remove`/`spool_sweep` берут `_SPOOL_LOCK`; eviction во время чтения ⇒ `session_expired`/перехват `OSError`/`FileNotFoundError`, не 500.
15. **`job`/прогресс.** Первый (телесный) запрос: прежний byte-offset прогресс разбора (`job`). Handle-запрос: загрузки нет; принимает опциональный `job` и эмитит терминальный `_progress_finish` (или ничего); фронт **пропускает** `localProgressStart` на handle-пути.
16. **Ошибки.** Unknown/expired handle → 410 `session_expired`; handle+тело → 400 `handle_body_conflict` (R1-m2); невалидные `from`/`to`/`window`/`sections` → 400; пустое окно → 200 с пустыми рядами. Фронт: `session_expired` ⇒ сброс `state.handle` + reset (как `downloadCsv` `:1459`).
17. **Миграция фронт-потребителей (R1-M3/R2-N-m1).** Единая точка нормализации — `computeView` (`index.html:1032`): для каждой метрики он режет тройку по индексам и **проецирует в плоский массив** выбранного режима (`view.series[key]` = `avg|min|max`) + строит `view.band[key]={lo,hi}` (min/max) при band-режиме. Все прочие потребители остаются на плоских массивах; меняются только `computeView` (+bands), `buildChart` (принимает `band`) и демо-payload `selfTest`. **`selfTest` (`:4452`/`:4454`)** сначала прогоняет демо-payload через `computeView` (нормализация) и лишь затем зовёт `buildCharts`/`buildClientCharts` — иначе тройки ломают билдеры (N-m1). Явный список потребителей — §29.6.
18. **Безопасность / модель угроз.** Локальный инструмент, аутентификации нет (§28). `handle` — непрозрачный `secrets.token_urlsafe(24)` (`_spool_create` `:342`); сессия/индекс **не добавляют** поверхности сверх spool: индексируется ровно загруженный клиентом spool, отдаётся по handle, loopback по умолчанию. Per-handle бюджет (п.2) — ресурсный предохранитель. Индекс **только в RAM** (не персистится) ⇒ нет утечки между запусками; вытесняется по TTL. Остаточные риски идентичны §28.

### 29.4 Альтернативы (по развилкам, trade-offs)

**Развилка (a) — смена периода на локальном файле:**

| Вариант | Механика | Память сервера | Плюсы | Минусы |
|---|---|---|---|---|
| **A. Handle-сессия + индекс (рек., решение владельца)** | Первый запрос — тело; далее — только `?handle` + окно | O(окна)+O(индекса) | Нет повторной загрузки и полного разбора; O(окна) по времени | Новая форма запроса; сессия живёт по TTL |
| B. Поднять cap/ничего не менять | Каждый период — тело | — | 0 кода | Повторная загрузка 740 МБ + полный разбор (текущий дефект) |
| C. Держать весь payload на клиенте | Клиентская фильтрация полного ответа | — | Просто на фронте | Полный payload (гигабайты) — против §28/REQ-L2 |
| D. Держать `records[]` на сервере | Разбор один раз, хранение всех записей | O(records) → OOM | Быстро | Нарушает INV-L2/INV-I1 |

**Развилка (b) — способ обслуживания окна:**

| Вариант | Механика | Время запроса | Плюсы | Минусы |
|---|---|---|---|---|
| **A. Разрежённый индекс + seek (рек.)** | Бинарный поиск + `seek` + ранний стоп | O(окна + T) | Точные границы окна; память O(точек) | Зависит от неубывания `t` (митигация — fallback, INV-I8) |
| B. Полный скан на запрос (текущее) | `_analyze_iter` по всему spool | O(файла) | 0 кода | ~740 МБ разбора на каждое движение периода |
| C. Пред-нарезка фиксированных окон | Материализовать окна заранее | — | Быстро | Жёсткая сетка; лишняя память/диск; не совпадает с произвольным brush |
| D. Полный по-записный offset-индекс | `(t,offset)` на каждую запись | O(1)-поиск | Точно | O(records) память — против INV-I1 |

**Развилка (c) — даунсэмплинг:**

| Вариант | Механика | Плюсы | Минусы |
|---|---|---|---|
| **A. min/max/avg бакеты, адаптивные (рек., решение владельца)** | Тройка+`n` на бакет; слияние удвоением | Сохраняет пики; потоково; режим — UI | Больше полей в контракте |
| B. Прореживание шагом (текущий `_Decimator`) | `rows[::stride]` | Просто | Теряет пики/выбросы — главный визуальный дефект |
| C. Всегда полное разрешение окна | Полный ряд на окно | Точно | Огромный payload на широком окне |
| D. Фиксированный `bucket_s = span/CHART_MAX_POINTS` | Ширина из span | Просто | span неизвестен в однопроходном coarse (R1-M2) |

**Развилка (d) — учёт секций (REQ-I10):**

| Вариант | Механика | Плюсы | Минусы |
|---|---|---|---|
| **A. Сервер гейтит вычисления по `sections` (рек.)** | Аккумуляторы строятся только для запрошенных | Экономит CPU/память/трафик; UI-тогглы | Гейтинг в `StreamAggregator`; cache-miss на позднюю секцию |
| B. Считать всё, отбрасывать на выходе | Полный агрегат, фильтр payload | Просто | Не экономит CPU/память — против цели |
| C. Статус-кво (всё всегда) | — | 0 кода | Нет селективности |

**Развилка (e) — политика кэша сессии (R1-B2):**

| Вариант | Механика | Плюсы | Минусы |
|---|---|---|---|
| **A. Кэш = индекс+сводка+бакеты; списки не кэшируются (рек.)** | Тяжёлые списки — только в coarse-ответе | Память ограничена (INV-I6) | Оконный ответ — только эти поля (фронт сливает) |
| B. Кэшировать весь coarse payload | O(drops)+O(players)+O(sessions) на handle | Просто | Память × handles — против INV-I2/I6 |
| C. Ничего не кэшировать | Каждое окно — полный проход | Минимум памяти | Окна снова O(файла) — против REQ-I1/I3 |

**Развилка (f) — детальный таймлайн (REQ-I11):**

| Вариант | Механика | Плюсы | Минусы |
|---|---|---|---|
| **A. Двухуровневый brush (рек.)** | Главный = весь лог; детальный = окно | Точная правка малого окна; вложенный зум | Больше UI-состояния/синхронизации |
| B. Зум одного brush | Приближение главного brush | Меньше кода | При 5 мин из 24 ч ручки слишком близко |
| C. Только числовой ввод под-диапазона | Ручной ввод | Просто | Не решает «ручки далеко» — лишь дополнение |

### 29.5 Влияние на требования, схему, контракты

- **Требования (design §2 / §28):** усиливает **REQ-L5** (window/период — UX): смена периода становится **мгновенной** за счёт переиспользования, а не повторной загрузки; сохраняет **REQ-L2** (память стримингом). Вводит **REQ-I1..REQ-I11** (§29.3), в т.ч. два расширения владельца — селективные секции (REQ-I10) и двухуровневый таймлайн (REQ-I11). G4/G5/G6 — без изменений; N3 (web-UI non-goal v1) уже устарел (§28.5).
- **Схема P1:** **без изменений**; `KNOWN_TYPES` не трогается.
- **Контракты (аддитивно/совместимо):**

| Контракт | Изменение | Совместимость |
|---|---|---|
| `POST /api/analyze` (тело) | **Без изменений** (первый запрос); возвращает `handle`; **+** опц. `sections` (default `all`) | Полная (default `all`) |
| `POST /api/analyze?handle=…&from&to&window&sections` (NO body) | **Новый** вход: обслуживается из сессии; ответ = **краткая сводка** (**полнодневная**, N-M2) + оконные **бакетные** ряды + `focus_points`; `coarse:false`, `window`, `window_count`; удалённые секции не считаются | Аддитивно; `handle`+тело ⇒ 400 `handle_body_conflict`; вытеснение ⇒ `session_expired`; handle-запрос **без** `from`/`to` = **coarse-recompute** тяжёлых секций: возвращает **`coarse:true`** + полные списки; фронт сохраняет оконные ряды/`focus_points`, заменяет только тяжёлые таблицы (N-M3/R3-m2) |
| `series`/`client_series`/`host_series` | Метрика → `{min, max, avg, n}` + общий `t[]` (унифицированный бакетный контракт, адаптивная бакетировка); **отрисовка различается по типу графика** (REQ-I5/§29.3 п.8) | Меняет форму рядов **везде**, включая FTP (фронт обновляется); `PROTOCOL_VERSION` 1→2 |
| `POST /api/export`, `/api/bundle` | **Без изменений** (уже handle-based) | Полная |
| FTP-период / FTP-индекс | **Без изменений** (период — клиентский; индекс/сессия не строятся) | Полная; но **форма рядов FTP меняется на тройки** (см. ниже) |
| `hldsmon` API | **+** `iter_records_offsets` (бинарные offsets; аддитивно; `iter_records` не меняется); бакетер min/max/avg + гейтинг секций в `StreamAggregator` | Аддитивно для CLI/library |
| `webui/index.html` период | `applyPeriodValues`/`fetchAnalyze`: handle-путь без тела; смена режима — без запроса; двухуровневый таймлайн; селективные тогглы блоков; слияние оконного ответа с coarse; `session_expired` ⇒ reset | Меняет UX-поток периода |

**Полный (coarse, первый/телесный) vs оконный (handle) ответ — точный список полей (R1-M4):**

| Поле | Coarse (первый) | Оконный (handle) |
|---|---|---|
| `handle` | да | да (эхо) |
| `window`,`window_count`,`coarse` | да (`coarse:true`) | да (`coarse:false`) |
| `series`,`client_series`,`host_series` | бакетные, весь день | бакетные, окно `[from,to]` |
| `focus_points` | окно по умолчанию | окно `[from,to]` |
| `digest` — **скаляры** (counts_by_type, period, players, sessions, drop_count, top_drop_reasons, hitch_lines, lag_spikes, log_end, anomalies_excluded count) | да | да (**полнодневная** краткая сводка; `period` = `[t_lo,t_hi]`, **не** окно — N-M2) |
| `drops` (полный список, nested `window`) | да | **нет** (фронт хранит из coarse, фильтрует по периоду) |
| `clients` | да | **нет** |
| `sessions`/`player_groups` | да | **нет** |
| `client_identities` | да | **нет** |
| `anomalies`,`anomalies_excluded`,`events`/`exclusions`,`config`,`host_caveats` | да | **нет** |

Параметр `sections=` ограничивает **оба** ответа (выключенная секция не считается и не отдаётся). `run`/`session_key`/drop-ключ coarse-only списков (`drops`/`clients`/`sessions`/`client_identities`) строятся в полнодневном проходе и **не зависят от окна** (N-M1); оконные run-чувствительные артефакты — только `client_series`/`focus_points`. Оконную **сводку** даёт кэшированный полнодневный summary (индекс `t_lo/t_hi`), а не записи окна (N-M2). **Канонический набор ключей `sections` (R3-m1):** `digest`, `series`, `host_series`, `client_series`, `client_identities`, `clients`, `sessions`, `drops`, `anomalies`, `events`, `config`, `focus_points`.

- **Инварианты:** **INV-I1** (разрежённый индекс `(t,seg,off,run)`; O(точек), не O(records)); **INV-I2** (память O(окна)+O(бакетов), без полного списка — наследует INV-L2); **INV-I3** (время окна O(окна+T); fallback O(файла) при немонотонном `t`); **INV-I4** (корректность окна vs эталон, включая run/ключи); **INV-I5** (stdlib/schema/auth); **INV-I6** (per-handle кэш ограничен); **INV-I7** (реальный TTL-sweeper); **INV-I8** (валидация монотонности + fallback); **INV-I9** (истинные байтовые offsets); **INV-I10** (адаптивная бакетировка ≤ `CHART_MAX_POINTS`). Инварианты плагина INV-1..INV-6 не затрагиваются.

### 29.6 Влияние на компоненты

| Компонент | Изменение |
|---|---|
| `webui.py` | `_handle_analyze` (ветка `handle` — без тела, 400 при теле; `sections`; первый запрос строит сессию+индекс за один проход); **sweeper** (`spool_sweep` по `SPOOL_SWEEP_S`); `_IndexBuilder` (run+monotonic+бинарные offsets+адаптивный шаг); `_serve_window` (run seed, fallback при немонотонности); `_seek_aligned`; поле `analysis` + `ANALYSIS_SESSION_MB`; гейтинг секций в сборке payload; адаптивный бакетер; **coarse-recompute тяжёлых секций** (handle-запрос без `from`/`to`, N-M3); **полнодневная сводка окна** из индекса `t_lo/t_hi` (N-M2); `handle_body_conflict`; `PROTOCOL_VERSION` 1→2. `_read_body`/`_handle_export`/`_handle_bundle` — существенно без изменений |
| `hldsmon.py` | **+** `iter_records_offsets` (бинарный режим, offsets до `readline`; `iter_records` не трогается); замена/расширение `_Decimator` на адаптивный бакетер min/max/avg; **гейтинг секций** в `StreamAggregator.add`/`finish`; `SERIES_MAX_POINTS` → цель `CHART_MAX_POINTS` (алиас) |
| `webui/index.html` | Проекция троек + band-map в `computeView`; **отрисовка по типу графика (O12): общий «Все датасеты» — тоггл `min`↔`max` (default max, band запрещён); per-metric — `min–max` band + линия avg**; персист; **селективные тогглы блоков** + персист (тоггл тяжёлой секции при активном окне ⇒ **handle-coarse-запрос**, N-M3); **двухуровневый таймлайн** (главный/детальный brush, синхронизация, вложенный зум); **оконный `coarse:false` не обновляет `state.fullRange`** (N-M2); слияние оконного ответа с coarse; handle-путь без Blob; `session_expired` ⇒ reset |
| `tools/analyzer/tests/` | Python `unittest`: сэмплирование индекса/границы + `run`; корректность окна vs полный скан, **включая multi-run**; немонотонный `t` → fallback; адаптивные бакеты (`min≤avg≤max`, ≤ cap, корректное слияние); multibyte UTF-8/CRLF offsets; жизненный цикл/TTL/**sweeper**/`session_expired`/`handle_body_conflict`; **селективные секции** (выключенная не считается; поздний запрос — пересчёт); `iter_records_offsets` vs `iter_records`. UI (режим, тогглы, таймлайн) — ручной QA/page-`selfTest` (§28.8) |

**Миграция фронт-потребителей троек (R1-M3):**

| Потребитель | Роль | Миграция |
|---|---|---|
| `computeView` (`index.html:1032`) | срез периода + нормализация | **меняется**: режет тройки, проецирует в плоские `view.series[key]` + строит `view.band[key]` |
| `sliceByIdx`/`seriesIndices` (`:925`/`:929`) | индексация по общему `t` | без изменений |
| `aggregateSeries` (`:945`) | сводка client_series (прямой индекс) | без изменений (получает спроецированные плоские) |
| `mergeSessions` (`:1015`), player-group merge (`:1123-1167`) | слияние сессий | без изменений |
| `buildCharts` (`:3187`) | серверные **per-metric** ряды | **+** передаёт `band` (min–max) + линию avg в `buildChart` (O12) |
| `buildClientCharts` (`:3236`) | клиентские **per-metric** ряды | **+** передаёт `band` (min–max) + линию avg (O12) |
| `buildAllChart` (`:3370`) + `ALL_DATASETS` (`:3076-3092`) | **общий** «Все датасеты» | **+** читает выбранную проекцию `min`/`max` (**band запрещён**); тоггл `min↔max` (O12) |
| `alignToServerTime`/`alignHostToServerTime` (`:3296`/`:3324`) | ресемплинг | без изменений |
| `buildChart` (рендер) | линия/бар | **меняется**: band (fill lo..hi) + линия avg; для **общего** графика band не передаётся (только min **или** max) (O12) |
| `selfTest` (`:4444`) | демо-proвек | демо-payload переводится на тройки; **сначала прогоняется через `computeView`** (`:4452`/`:4454`), затем `buildCharts`/`buildClientCharts` (N-m1) |
| `downloadCsv` (`:1444`) | экспорт | без изменений |

### 29.7 Не-цели

- Изменения схемы JSONL P1, `KNOWN_TYPES`, `exports.whitelist`, Makefile, плагина/`src/`, ABI/хуков.
- Изменение FTP-**периода** (клиентская фильтрация сохраняется) и FTP-UX; FTP-индекс/сессия не строятся (форма рядов FTP при этом меняется на тройки — см. §29.5).
- Аутентификация / многопользовательская модель (web-UI остаётся локальным инструментом).
- Персистентный на диске индекс/сессия (индекс только в RAM, перестраивается при новой загрузке).
- Resumable/chunked-upload; внешние зависимости (stdlib-only); Windows-специфика как отдельная фаза.
- Кэширование полного списка записей (запрещено INV-L2/INV-I1).
- Кэширование тяжёлых полнодневных списков в сессии (запрещено INV-I6).
- Инкрементальный анализ «на лету» поверх растущего FTP-spool (сверх необходимого для локального spool).
- Селективные секции для `/api/export`,`/api/bundle` (они уже узкие: тип/окно/ключ drop).

### 29.8 Приёмочные качественные сценарии (QAS-I)

| ID | Проверка |
|---|---|
| QAS-I1 | Смена периода на файле ~740 МБ: 2-й и последующие запросы **не шлют тело**; отклик ≪ времени первого запроса. |
| QAS-I2 | Оконный запрос не читает весь файл: прочитанные из spool байты ~O(окна)+граница; кратно меньше полного скана. |
| QAS-I3 | Первый телесный запрос: coarse-overview и индекс построены за **один** проход; ответ содержит `handle`; coarse совпадает с **новым** эталоном (полный скан, бакетные тройки) — (R1-m3). |
| QAS-I4 | Оконная корректность: для `[from,to]` множество = эталон (полный скан); частичная строка на границе seek отброшена, первая полная не потеряна; `from>to`/за границами — детерминированно. |
| QAS-I5 | Границы: `from` до первого сэмпла / после последнего / `from>to` / пустое окно — пустой ряд, без ошибок и без тихих нулей (окно до первой записи ⇒ seed `run=0`/`offset=0`, N-m2). |
| QAS-I6 | Бакеты: `min ≤ avg ≤ max`, `n` = число записей бакета; пустой бакет ⇒ `null` (не 0); число бакетов ≤ `CHART_MAX_POINTS`. |
| QAS-I7 | **Отрисовка по типу графика (O12):** общий «Все датасеты» показывает **один** из `min`/`max` (тоггл, default `max`, **без band**); per-metric — `min–max` band + линия `avg`; всё из **одного** payload, смена тоггла/режима не шлёт запрос. |
| QAS-I8 | Маркеры drop/hitch/event выровнены в любом режиме (эпохально-выровненные бакеты; `t` маркеров не снапится). |
| QAS-I9 | Вытесненный/TTL handle ⇒ HTTP 410 `{"code":"session_expired"}`; `handle`+тело ⇒ HTTP 400 `{"code":"handle_body_conflict"}` (R1-m2); фронт сбрасывает handle и пере-загружает. |
| QAS-I10 | Память сервера при N сменах периода не растёт: та же сессия/индекс; memo (LRU=8) ограничен; **тяжёлые списки не кэшируются** (INV-I6). |
| QAS-I11 | Конкурентность: параллельные оконные запросы к одному handle не портят индекс/memo; eviction во время чтения ⇒ `session_expired`, не 500. |
| QAS-I12 | **Churn:** N загрузок + смена периодов; по TTL **фоновый sweeper** освобождает файлы+budget+`analysis`; RSS плоский (R1-B2). |
| QAS-I13 | stdlib-only; схема P1/`KNOWN_TYPES` без изменений; существующие Python-тесты зелёные + новые; Windows-совместимо. |
| QAS-I14 | **(UI)** Тогглы режима/секций/таймлайна и их персист — ручной QA/page-`selfTest` (§28.8); данные/счётчики проверяемы Python-тестами. |
| QAS-I15 | **Multi-run (R1-B1/R2-N-M1):** окно, начинающееся **после** `shutdown`/`crash`, даёт оконные `client_series`/`focus_points` идентичными полному скану (run seed'нут); `drops`/`clients`/`sessions`/`client_identities` — **coarse-only** (из полнодневной сводки), в оконном ответе отсутствуют. |
| QAS-I16 | **Немонотонный `t` (R1-M1):** синтетический spool с «шагом назад» → оконный результат = полному скану (fallback на полный скан). |
| QAS-I17 | **Адаптивные бакеты (R1-M2):** полнодневный coarse ≤ `CHART_MAX_POINTS`; слитые `min`/`max` равны по-секундной истине. |
| QAS-I18 | **Offsets/multibyte (R1-m1):** имена/строки UTF-8 + CRLF → границы окна корректны (нет разрыва строки/порчи). |
| QAS-I19 | **Селективные секции (REQ-I10/O15/R3-m1/m3):** `sections=series` реально **пропускает** работу по drops/anomalies/clients (измеримо) и не отдаёт их; **default = все**; ключи канонически — `client_identities` (не `identities`); **лёгкая** секция в активном окне включается **оконным** запросом `?handle&from&to` (**O(окна)**, не полный проход); **тяжёлая** — **coarse-recompute** (handle без `from`/`to`, `coarse:true`) **без** кэша. |
| QAS-I20 | **Двухуровневый таймлайн (REQ-I11/O14):** детальный brush **авто-показывается** при `span(window) < 0.05·span(fullRange)`; его правка сужает окно и запрашивает более мелкие бакеты; главный brush отражает окно; маркеры выровнены. **Метод — ручной QA/`selfTest`.** |
| QAS-I21 | **Миграция троек (R1-M3/O12):** все потребители рендерят корректно — общий график как `min` **или** `max` (без band), per-metric как band+avg, player-group merge/сводные колонки; `selfTest` (через `computeView`) на тройках зелёный. |
| QAS-I22 | **Полный vs оконный поля (R1-M4/R2-N-M2):** оконный ответ не содержит тяжёлых списков; фронт сливает с coarse и сохраняет таблицы/drops/config; FTP-ряды рисуются из троек; оконная сводка (`period`) — **полнодневная**; оконный ответ **не** сбрасывает `state.fullRange`. |
| QAS-I23 | **N-M2 полнодневный домен:** после оконного запроса (`coarse:false`) `state.fullRange`/главный brush остаются полнодневными `[t_lo,t_hi]`; `digest.period` = весь лог, не окно. **Метод — ручной QA/`selfTest`.** |
| QAS-I24 | **N-M3 coarse-recompute:** включение тяжёлой секции при активном окне шлёт handle-запрос **без** `from`/`to` с расширенным `sections`; сервер отдаёт полные списки за один O(файла) проход; тяжёлые **не** кэшируются; фронт re-merge'ит таблицы. |

### 29.9 Открытые вопросы (владелец)

**Все владельческие вопросы Стадии 13 закрыты (O10–O15).** O10 (индекс: 5 с / 1 МиБ / 20000) и O13 (memo 8) решены ранее (§29.3 п.1/п.6); O11 (`CHART_MAX_POINTS=2000`, `min_resolution=1 с`), O12 (отрисовка по типу графика), O14 (`DETAIL_TIMELINE_RATIO=0.05`), O15 (default = все секции, персист глобально) — **решены владельцем**; зафиксированы в §29.3 п.7/п.8/п.11/п.12 и §16. **Открытых вопросов по стадии нет.**

### 29.10 План → `docs/plan.md`, «Стадия 13 — `instant-period`».

Обязательные артефакты плана: `tools/analyzer/webui.py` (handle-ветка `/api/analyze` без тела + `handle_body_conflict`, сессия/индекс `(t,seg,off,run)` + monotonic, `_serve_window` с run seed, **фоновый sweeper**, адаптивный бакетер, гейтинг секций, `ANALYSIS_SESSION_MB`, `PROTOCOL_VERSION`); `tools/analyzer/hldsmon.py` (`iter_records_offsets` бинарный, бакетер min/max/avg, гейтинг секций в `StreamAggregator`); `tools/analyzer/webui/index.html` (проекция троек + band в `computeView`/`buildChart`, режимы, селективные тогглы, двухуровневый таймлайн, слияние оконного ответа с coarse, handle-путь без Blob, `session_expired` reset); `tools/analyzer/tests/`. `src/`, схема P1/`KNOWN_TYPES`, `Makefile` — **не трогаются**.

### 29.11 Раздел ревью дизайна (oracle)

**R1 — первое ревью (oracle): PASS-WITH-REVISIONS.** Направление (B/C) верное; две блокирующие находки (run-корректность окна; утечка/неограниченный кэш) и набор major/minor сложены.

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| **B1** | Blocking | Оконные `client_series`/`focus_points`/drop-identity ≠ полному скану: `run` выводится из позиции потока и входит в `session_key`, а сэмпл индекса был `(t,seg,off)`. | **Внесено** — сэмпл = `(t,seg,off,**run**)` (INV-I1/INV-I9, п.5); `_serve_window` seed'ит `run` и инкрементит по shutdown/crash **для всех** прочитанных записей; REQ-I9, QAS-I15; `session_key`/drop-ключ зафиксированы. |
| **B2** | Blocking | `spool_sweep` сервером не вызывается (утёк); coarse-payload O(drops)+O(players)+O(sessions), кэш на handle размножает память. | **Внесено** — (i) реальный периодический sweeper (INV-I7, п.3, QAS-I12); (ii) кэш ограничен: только индекс+краткая сводка+бакеты, тяжёлые списки **не** кэшируются, per-handle `ANALYSIS_SESSION_MB` (INV-I6, п.2); оконный ответ не несёт списков, фронт сливает с coarse (п.10, §29.5, QAS-I22). |
| **M1** | Major | Неубывание `t` не обосновано (продюсер — `CLOCK_REALTIME`); лежит в основе бинарного поиска и раннего стопа. | **Внесено** — валидация монотонности в проходе; при нарушении — fallback на полный скан (INV-I8, п.1/п.5); QAS-I16; факт часов — §29.2. |
| **M2** | Major | `bucket_s = span/CHART_MAX_POINTS` невычислим в однопроходном coarse (span известен в конце). | **Внесено** — адаптивная эпохально-выровненная бакетировка со слиянием удвоением (ассоциативно min/max/avg/n); INV-I10, п.7; QAS-I17. |
| **M3** | Major | Многие фронт-потребители ожидают **плоские** массивы и сломаются на `{min,max,avg,n}`. | **Внесено** — единая проекция в `computeView` (+bands); таблица потребителей и per-consumer миграция — §29.6; п.17; QAS-I21. |
| **M4** | Major | Двусмысленность полного/оконного ответа (в т.ч. `focus_points`); форма рядов меняет и FTP. | **Внесено** — точная таблица полей (§29.5), п.10; `focus_points` — оконный; FTP: период клиентский, но **форма рядов — тройки** (п.13, §29.5/§29.7); QAS-I22. |
| **m1** | Low | Текстовый `fh.tell()` — непрозрачный cookie; не байтовый offset. | **Внесено** — `iter_records_offsets` в **бинарном** режиме; offsets до `readline` (INV-I9, п.1); QAS-I18. |
| **m2** | Low | handle-форма «осушивает/игнорирует» тело без semaphore/cap. | **Внесено** — `Content-Length>0`/непустое тело ⇒ HTTP 400 `handle_body_conflict` (п.4/п.16); QAS-I9. |
| **m3** | Low | QAS-I3 «coarse совпадает с прежним эталоном» неверно под тройками. | **Внесено** — QAS-I3 сравнивает с **новым** эталоном (полный скан, бакетные тройки). |
| **m4** | Low | O10/O13 — внутренняя настройка, не owner-level. | **Внесено** — решены в дизайне (индекс 5 с/1 МиБ/20000; memo 8); §16 оставляет только O11/O12 + новые O14/O15. |
| **m5** | Low | `_seek_aligned`-drop — мёртвый код, если offset снят до `readline`. | **Внесено** — capture-точка явно **до** `readline` (offset = начало строки); `_seek_aligned` — защитный fallback (п.1/п.5). |
| **m6** | Low | Адаптивное расширение шага при `INDEX_MAX_ENTRIES` не задано. | **Внесено** — **удвоение** обоих шагов; append-порядок сохраняет сортировку (INV-I1, п.1). |

**Отложено:** нет. **Два расширения владельца** (селективные секции, двухуровневый таймлайн) добавлены как REQ-I10/REQ-I11 с QAS-I19/I20. Повторное ревью — **R2** (ниже).

**R2 — второе targeted ревью (oracle): PASS-WITH-REVISIONS.** Все R1-блокеры закрыты; выявлены **3 новых major** (contract/state-sync) и **3 low** — все сложены.

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| **N-M1** | Major | REQ-I9/QAS-I15 требовали run-корректность для `drops`/`clients`/`sessions`/`client_identities`, но §29.3 п.10/§29.5 исключают их из оконного ответа (coarse-only). | **Внесено** — REQ-I9/QAS-I15/INV-I4 перескоупированы на оконные run-чувствительные артефакты (`client_series`, `focus_points`); `drops`/`clients`/`sessions`/`client_identities` явно **coarse-only**, их `run`/`session_key`/drop-ключ — из полнодневной сводки (REQ-I3, §29.3 п.10, §29.5). |
| **N-M2** | Major | Не определено: оконная сводка (`period`) — полнодневная или оконная; `_serve_window` читает только окно ⇒ `digest.period` стал бы окном, и фронт (`index.html:1425-1426`) сбросил бы `state.fullRange`. | **Внесено** — сводка/`period` оконного ответа = **кэшированная полнодневная** `[t_lo,t_hi]` (не пересчёт окна); фронт **не** обновляет `state.fullRange` при `coarse:false`; домен главного brush остаётся полнодневным (§29.3 п.10/п.12, §29.5, §29.6); QAS-I23. |
| **N-M3** | Major | Включение ранее выключенной тяжёлой секции при активном окне не обслуживается оконным ответом; «с кэшем» противоречило INV-I6. | **Внесено** — тяжёлая секция ⇒ **coarse-recompute**: фронт шлёт handle-запрос **без** `from`/`to` с расширенным `sections`; сервер — один O(файла) проход, полные списки **без кэша** (INV-I6); фронт re-merge (§29.3 п.11, REQ-I10, §29.5/§29.6); QAS-I19/I24. |
| **N-m1** | Low | `selfTest` зовёт `buildCharts`/`buildClientCharts` на демо-payload напрямую, минуя `computeView`; тройки ломают билдеры. | **Внесено** — `selfTest` сначала прогоняет демо-payload через `computeView` (нормализация), затем билдеры (§29.3 п.17, §29.6); QAS-I21. |
| **N-m2** | Low | Не определён seed при отсутствии сэмпла с `t ≤ from` (окно до первой записи). | **Внесено** — seed `run=0`, `offset=0` (начало spool) (§29.3 п.5, REQ-I3); QAS-I5. |
| **N-m3** | Low | Sweeper-eviction vs in-flight read на Windows (`PermissionError` → `_silent_remove` → `False`). | **Внесено** — note: eviction откладывается до `spool_startup_cleanup`; безвредно (spool-файлы открыты лишь на время запроса) (§29.3 п.3). |

**Отложено:** нет; все R2-замечания сложены.

**R3 — третье targeted ревью (oracle): PASS.** Все R1/R2-замечания подтверждены закрытыми; три low-clarification (ниже) сложены.

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| **R3-m1** | Low | Разнобой в ключе секции: `identities` (§29.3 п.11) vs поле `client_identities`. | **Внесено** — канонический набор ключей зафиксирован: **`client_identities`** (не `identities`); §29.3 п.11, §29.5, REQ-I10, QAS-I19. |
| **R3-m2** | Low | Не определены форма/флаг coarse-recompute и направление merge. | **Внесено** — handle-запрос **без `from`/`to`** ⇒ **`coarse:true`** + тяжёлые списки; merge: сохранить оконные ряды/`focus_points`/период, заменить **только тяжёлые таблицы** (§29.3 п.11, §29.5, QAS-I24). |
| **R3-m3** | Low | Запрос для re-enable **лёгкой** секции в активном окне не определён; формулировка «пересчёт одного прохода» намекала O(файла). | **Внесено** — лёгкая секция в активном окне = **оконный** `?handle&from&to` (**O(окна)**, INV-I3); «пересчёт одного прохода» относится только к первичному построению ядра (§29.3 п.11, REQ-I10, QAS-I19). |

**Отложено:** нет; все R3-замечания сложены. **Все владельческие вопросы (O10–O15) решены.** **DESIGN GATE: CLOSED.** Следующий шаг — план (`docs/plan.md`, «Стадия 13 — `instant-period`») и plan-review `momus`.
