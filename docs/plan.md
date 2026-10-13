# HldsMonitoring — план реализации (rev 2, plan-review пройден)

## Заголовок плана (bindings)

- **Design:** `docs/design.md` (rev 2, reviewed). **Plan-review:** `momus` (PASS-WITH-REVISIONS — замечания сведены в §«Disposition plan-review»). **Design-review:** `oracle`.
- **Язык документации:** русский; идентификаторы/код — английский.
- **Решения владельца (§16 design):** Linux-first; файлы JSONL с суточной ротацией (FTP); строго пассивный v1; внешний агент — не в v1; внутренний инструмент; коммит только по явной просьбе.
- **Открытые вопросы:** O1 (частота/объём), O2 (PII/retention), O3 (целевые сборки ReHLDS) — не блокируют v1.
- **Верификация:** `make` (сборка 32-бит), `make test` (хостовые C++ unit + Python-тесты аналитора), `make clean`.
- **Коммит:** запрещён без явной просьбы владельца.
- **Интеграционная проверка:** владелец предоставляет доступ к **тестовому ReHLDS-серверу** — используется для QAS-1 (overhead), QAS-3 (`kill -9`), QAS-6 (нет API) и ручного smoke (Фазы 3/4/6).
- **Канонический формат лога:** Приложение P1 настоящего плана (полная схема всех типов и вложенных срезов).

**Соглашения:**
- C++17, `-m32 -fPIC -shared`, `-Wall -Wextra`, `-Werror` в CI. Идентификаторы snake_case/lowerCamel согласованно; никаких `using namespace` в заголовках.
- Хостовое ядро (P1/P2) не включает движковые заголовки и компилируется нативным g++ без `-m32` (проверяется целью `test-host-core`).
- Движковая обвязка (P3/P4) компилируется только с вендоренными заголовками.

**Глобальные инварианты (действуют на всех фазах):**
- **INV-1 (пассивность):** ни один хук не короткозамыкает цепочку; всегда `callNext` + возврат его результата. `PreprocessPacket` никогда не возвращает `false`.
- **INV-2 (hot path без аллокаций):** в хуках и в пути сбора/сериализации нет `malloc/strdup/snprintf/ED_Alloc/Con_Printf` и нет роста `std::string`; все буферы предвыделены на `Meta_Attach`. Проверяется тестом «0 аллокаций» (переопределение `operator new`).
- **INV-3 (поток):** весь сбор — на server thread; один писатель/читатель, гонок нет.
- **INV-4 (ABI):** доступ к `client_t`/`netchan_t` только через `engine_layout` + проба на `Meta_Attach`; при несовпадении — fail-closed. Предикат пробы — чистая host-testable функция.
- **INV-5 (формат):** каждая строка лога — валидный JSON-объект с `type` и `t` по Приложению P1.
- **INV-6 (границы):** без БД; без внешнего агента/Windows в этом стадионе.

**Маппинг QAS → метод проверки (закрывает H3):**

| QAS | Где проверяется | Метод |
|---|---|---|
| QAS-1 (overhead <2% CPU / <0.5 ms p99) | P6 | ручной бенч на реальном сервере; метод задокументировать в journal |
| QAS-2 (drop-record <0.2 ms, окно ≥ N) | P1/P3 | unit-тест сборки записи + тест «0 аллокаций» + бенч сериализатора |
| QAS-3 (kill -9, целостность) | P2/P5/P6 | ручной smoke (вне CI) + unit-тест tolerant-парсера |
| QAS-4 (память <8 МБ) | P1/P6 | unit-тест размеров структур + ручной RSS |
| QAS-5 (суточная ротация) | P2 | unit-тест ротации по инъекции времени |
| QAS-6 (нет API → нет краха) | P3/P6 | review + ручной smoke (fail-closed) |
| QAS-7 (non-interference) | P3 | unit-тест обёрток хуков «возвращают вход / вызывают callNext» (host) |
| QAS-8 (make / make test) | P0/P6 | CI/локально |

---

## Фаза 0 — Скелет репозитория и сборка

**Цель:** пустой плагин собирается `make`; тестовый каркас запускается; whitelist экспортов проверяется.

**Файлы:** `Makefile`, `version_script.lds`, `exports.whitelist`, `include/` (вендоринг), `include/VENDORED.md`, `src/` (заглушки), `tests/run_tests.sh`, `.gitignore`.

**Границы / non-goals:** нет логики сбора; заголовки только вендорятся.

**Шаги:**
1. Клонировать `rehlds/Metamod-R` (`56590c5`) и `rehlds/ReHLDS` (`550f2d6`); скопировать в `include/`: `metamod-r/metamod/extra/example/include/{hlsdk,metamod}`; `rehlds/public/rehlds/{rehlds_api.h,hookchains.h,rehlds_interfaces.h}`; `rehlds/public/interface.h`; движковые `rehlds/engine/{server.h,net.h}` (+ транзитивные по мере необходимости). Зафиксировать коммиты и набор файлов в `include/VENDORED.md`.
2. `Makefile`: цели `all` (сборка `HldsMonitoring_mm_i386.so`), `test` (запускает `test-host-core`, `test-core`, python-тесты), `test-host-core` (компиляция `tests/` нативным g++ без движковых заголовков), `clean`; флаги `-m32 -fPIC -shared -Wall -Wextra -Werror -ldl`; `-Wl,--version-script=version_script.lds`.
3. `version_script.lds`: экспорт `GiveFnptrsToDll`, `Meta_Query`, `Meta_Attach`, `Meta_Detach`, `GetEntityAPI2`, `GetEntityAPI2_Post`, `GetNewDLLFunctions`, `GetEngineFunctions`, `GetEngineFunctions_Post`; `local: *`.
4. `exports.whitelist` — тот же список.
5. Заглушки `src/bootstrap.cpp` (Metamod-entrypoints, пустые `OnMetaAttach/Detach`), заглушки остальных модулей.
6. `tests/run_tests.sh` — компилирует/запускает хостовые тесты (пока пустой список).

**Инварианты:** сборка `-m32` проходит; экспортируется только whitelist.
**Acceptance:** `make` создаёт `.so`; множество определённых динамических символов **точно равно** whitelist; `make test` и `test-host-core` завершаются 0.
**Verify:** `make && nm -D --defined-only HldsMonitoring_mm_i386.so | awk '{print $3}' | sort -u > /tmp/got.txt && diff /tmp/got.txt exports.whitelist && make test`
**Риски:** multilib (проверено: установлен); движковые заголовки могут требовать доп. include-путей — решать по мере компиляции.

---

## Фаза 1 — Host-testable ядро

**Цель:** чистая логика, покрытая unit-тестами без движка, без аллокаций в пути записи.

**Файлы:** `src/metrics.h`, `src/ring_buffer.h`, `src/delta.h`, `src/serialization.{h,cpp}`, `tests/test_ring.cpp`, `tests/test_delta.cpp`, `tests/test_serialization.cpp`, `tests/test_no_alloc.cpp`.

**Границы / non-goals:** нет обращений к движку; нет файлового I/O.

**Шаги:**
1. `metrics.h`: POD-структуры `ClientSample`, `ServerSample`, `DropRecord`, `RejectRecord`, `ConnectRecord`, `MapChangeRecord`, `HitchRecord`, `PacketStats`, `NetCmdRecord`, `MassDropRecord`, `VoiceRecord`; фиксированный размер/паккинг (`#pragma pack`), `static_assert(sizeof(...))` для каждой.
2. `ring_buffer.h`: шаблон bounded ring (fixed capacity), `push/size/at/last(n,out)`; без аллокаций.
3. `delta.h`: `CounterDelta` — Δ для кумулятивных счётчиков с обработкой 32-битного wrap; `compute_delta(prev, cur)`.
4. `serialization.{h,cpp}`: **API без аллокаций** — `size_t write_*_json(char* out, size_t cap, const Record&)`; возвращает длину; при нехватке `cap` — возврат 0 (не пишет частичную строку). JSON-экранирование строк. `std::string`-обёртки — только для host-тестов.
5. Тесты: ring wrap/`last(n)`; delta с wrap/сбросом; JSON-экранирование; `sizeof` всех структур; **test_no_alloc** — переопределить `operator new` и assert, что сборка drop-record и client/server-строк не вызывает аллокаций.
6. Полная схема — Приложение P1; тесты сериализации проверяют каждое поле.

**Инварианты:** INV-2 (в пути записи), INV-5.
**Acceptance:** все unit-тесты зелёные; test_no_alloc — 0 аллокаций; `sizeof` укладывается в бюджеты (ClientSample < 160 B, DropRecord window — динамическое).
**Verify:** `make test`.
**Риски:** расхождение формата с анализатором — фиксируется Приложением P1 и тестами Фазы 5.

---

## Фаза 2 — LogWriter (суточная ротация)

**Цель:** надёжная запись JSONL с суточной ротацией и правами 0600.

**Файлы:** `src/log_writer.{h,cpp}`, `tests/test_log_writer.cpp`.

**Границы / non-goals:** нет движка; принимает готовые JSON-строки.

**Шаги:**
1. `LogWriter::open(dir, now_utc)` — создать директорию (`0700`); открыть `hldsmon-YYYY-MM-DD.jsonl` (`0600`); fallback `/tmp/hldsmon`.
2. `write(const char* line, size_t len)` — `fwrite` + `fputc('\n')` + учёт объёма; при смене UTC-даты (передаётся `now`) — ротация (закрыть/открыть).
3. `flush()`; счётчик ошибок; при превышении `max_mb` — предупреждение и **отключение записи** с флагом (сбор в память продолжается).
4. Инъекция времени (параметр `now`) для тестируемости.

**Инварианты:** INV-5; файл либо валидно открыт, либо запись отключена с флагом.
**Acceptance:** тест ротации (две даты → два файла); тест прав (0600/0700); тест «диск недоступен → нет краха, флаг ошибки»; тест «превышение лимита → запись отключена».
**Verify:** `make test`.

---

## Фаза 3 — Движковая обвязка (коллектор)

**Цель:** плагин регистрирует хуки, собирает метрики, пишет лог; компилируется против вендоренных заголовков.

**Файлы:** `src/bootstrap.{h,cpp}`, `src/hook_manager.{h,cpp}`, `src/hooks.cpp`, `src/sampler.{h,cpp}`, `src/events.{h,cpp}`, `src/engine_layout.{h,cpp}`, `src/platform_linux.cpp`, `src/plugin.h`.

**Границы / non-goals:** без Control-команд (фаза 4); без внешнего агента/Windows.

**Шаги:**
1. **bootstrap:** Metamod-entrypoints; `Meta_Query`/`Meta_Attach`/`Meta_Detach`; получение `IRehldsApi` через `Sys_GetModuleHandle`+`Sys_GetFactory`; версия-гейт; инициализация контекста.
2. **bootstrap (H2):** **регистрация всех cvar §12 design** со списком/defaults (`hldsmon_enable`, `_client_hz`, `_disk_hz`, `_flush_sec`, `_client_ring`, `_server_ring`, `_drop_window`, `_dir`, `_max_mb`, `_ip_table`); кэш указателей на cvar в контекст; Sampler читает только через них.
3. **engine_layout:** доступ `GetId()→GetClient_t()`; **чистый host-testable предикат пробы** `probe_ok(const ClientProbeView&)`, проверяющий `remote_address == GetRemoteAdr()`, диапазон `connect_time`, согласованность `userid`; тонкий движковый адаптер вызывает предикат; при несовпадении `Meta_Attach` возвращает false (fail-closed).
4. **hook_manager:** `register_hooks()`/`unregister_hooks()`; политика «всегда `callNext`+propagate»; приоритет `SV_DropClient` — в голове цепочки (числа фиксированы и задокументированы); обработка **частичной** регистрации (M5): неудача одного хука → флаг деградации, продолжение с доступными.
5. **hooks.cpp:** обработчики по §5 design; `SV_DropClient` — снимок ДО `callNext`; fallback-детект (`Steam_NotifyClientDisconnect`, `HandleNetCommand`); `PreprocessPacket` — только счётчики O(1) и `return chain->callNext(...)`.
6. **sampler:** `SV_Frame` — кадровые метрики; раз в секунду агрегация → `ServerSample` + `LogWriter`; staggered опрос клиентов в `ClientRings`; **парсинг userinfo в предвыделенный буфер** (`rate/cl_updaterate/cl_cmdrate/name`, без аллокаций); `rel_fill_pct`; поведения `hldsmon_enable=0` (полная остановка записи).
7. **events:** `drop/reject/connect/netcmd/massdrop/packet/hitch/mapchange/voice` по Приложению P1; при диск-full — запись отключена, сбор продолжается.
8. Все аллокации — на `Meta_Attach`; в hot path запрещены (INV-2).

**Инварианты:** INV-1..INV-4, INV-6.
**Acceptance:** компилируется `make` (`-m32`, `-Wall -Wextra -Werror`) без предупреждений; `Meta_Attach` возвращает false при отсутствии API/провале пробы; **unit-тест предиката пробы** (совпадает/не совпадает) зелёный; **unit-тест QAS-7** — обёртки возвращают результат `callNext`/вход; **unit-тест сборки DropRecord** из фиктивных данных (host) зелёный.
**Verify:** `make && make test`.
**Риски:** точные сигнатуры/поля заголовков — сверять по `rehlds_api.h`; включение движковых `server.h`/`net.h` только в `engine_layout.cpp`.

---

## Фаза 4 — Control (RCON-команды)

**Цель:** `hldsmon_status` (M1: точный набор полей) и `hldsmon_flush_log`.

**Файлы:** `src/control.{h,cpp}`.

**Границы / non-goals:** только серверные команды; без HTTP/сети.

**Шаги:**
1. Регистрация команд через серверный API движка (уточнить при вендоринге: `AddServerCommand`/cvar-callback); зарегистрировать `hldsmon_status`, `hldsmon_flush_log`.
2. `hldsmon_status` выводит: путь текущего файла, размер за сутки, `enabled`, размеры/заполнение колец, счётчики drop/reject/hitch/massdrop, счётчик ошибок записи, пройдена ли проба layout.
3. `hldsmon_flush_log` — `flush()` без блокирующей сериализации большого объёма.

**Инварианты:** INV-3.
**Acceptance:** компилируется; обе команды зарегистрированы; вывод содержит все перечисленные поля; verify — ручной smoke (вне CI) + review на отсутствие блокировок.
**Verify:** `make`; ручной smoke на сервере (задокументировать в journal).

---

## Фаза 5 — Инструменты анализа

**Цель:** CLI/библиотека парсинга и агрегации JSONL по Приложению P1.

**Файлы:** `tools/analyzer/hldsmon.py`, `tools/analyzer/tests/{fixtures/*.jsonl,test_hldsmon.py}`, `tools/analyzer/README.md`.

**Границы / non-goals:** только stdlib; без сети; не изменяет входные файлы.

**Шаги:** подкоманды `summary`, `drops`, `window`, `clients`, `anomalies`, `digest`, `export-csv` (§8 design); tolerant-парсер (игнор обрезанной строки и неизвестных `type`); фикстуры покрывают **все типы P1**, включая `netcmd/massdrop/packet/voice` и вложенные срезы drop.
**Инварианты:** INV-5.
**Acceptance:** `python3 -m unittest` зелёный; `summary`/`window`/`digest` дают детерминированный вывод на фикстурах (сравнение со snapshot).
**Verify:** `make test`.

---

## Фаза 6 — Верификация и документация

**Шаги:** `make clean && make && make test`; проверка whitelist экспортов; `test-host-core` (ядро не тянет движковые заголовки); обновить `include/VENDORED.md`; задокументировать метод интеграционной проверки (QAS-1/3/6).
**Acceptance:** `make`/`make test` зелёные; whitelist совпадает; метод ручных проверок задокументирован.

---

## Фаза 7 — Запись стадии

**Файлы:** `docs/journal.md`. **Acceptance:** контекст, что сделано, решения и почему, затронутые артефакты, открытые темы (O1/O2/O3), следующие шаги; индексы `AGENTS.md` актуальны.

---

## Фаза 8 — Коммит (гейт)

Один связный коммит — **только по явной просьбе владельца**. Иначе: «готово к коммиту».

---

## Definition of Done (стадия)

- [ ] `make` собирает `.so` (32-бит, `-Wall -Wextra -Werror`) без предупреждений.
- [ ] `make test` зелёный (C++ core incl. `test_no_alloc` + Python analyzer) и `test-host-core` зелёный.
- [ ] Whitelist экспортов точно соблюдён (`exports.whitelist`).
- [ ] INV-1: ни один хук не короткозамыкает; покрыто QAS-7 unit-тестом.
- [ ] INV-2: сериализация/сбор не аллоцируют; покрыто `test_no_alloc`.
- [ ] INV-3: сбор на server thread (review).
- [ ] INV-4: предикат пробы host-testable и покрыт тестами; fail-closed на attach.
- [ ] INV-5: все типы P1 сериализуются и парсятся; покрыто тестами.
- [ ] INV-6: без БД/агента/Windows (review).
- [ ] Суточная ротация и права 0600/0700 подтверждены тестом.
- [ ] Все cvar §12 зарегистрированы; `hldsmon_enable=0` останавливает запись.
- [ ] `docs/journal.md` заполнен; индексы обновлены.
- [ ] Коммит — только при явной просьбе.

---

## Приложение P1 — полная схема JSONL (каноническая)

Общие поля: `type` (string), `t` (float, wall-clock epoch seconds).

```
server:  {type, t, frame_ms:{min,avg,max}, hitch:{gt50,gt100}, cpu_pct, rss_kb, minflt, majflt,
          players, real, proxy, fake, in_kbps, out_kbps, edicts, edicts_churn, map, uptime_s, fps, maxclients}
client:  {type, t, id, gen, ping, loss, dloss, choke, dchoke, delta_seq, in_seq, in_ack, out_seq,
          rel_seq, rel_fill, rate, cleartime, updaterate, cmdrate, state, fragbuf_total,
          incomingready_mask, flow_in_kbps, flow_out_kbps, flow_in_avg, flow_out_avg, msg_flags,
          idle_ms, jit_min_ms, jit_avg_ms, jit_max_ms, name, authid, steamid64, ip, port, userid,
          slot, conn_s, fake, proxy}
drop:    {type, t, id, gen, reason, category, crash, observed_via, detector,
          name, authid, steamid64, ip, port, userid, slot, conn_s, fake, proxy,
          client:{active,spawned,fully_connected,connected,proxy,fake,userid,connecttime,cmdtime,connection_started},
          netchan:{in_seq,in_ack,in_rel_ack,in_rel_seq,out_seq,rel_seq,last_rel_seq,rel_len,rel_fill,rate,cleartime,msg_flags,last_received,connect_time,now_real,idle_ms,sv_timeout},
          window:[{client-sample compact}], server:{server-sample}, neighbors:{count,avg_loss,avg_ping,max_loss}}
reject:  {type, t, ip, steamid, stage, detail, stage_ms|null, total_ms|null}
connect: {type, t, id, ip, steamid, name, auth_ms|null, steam_ms|null}
netcmd:  {type, t, id, cmd, count}
massdrop:{type, t, window_s, count, reason, category}
packet:  {type, t, pps, bytes, sizes:{lt128,lt512,lt1024,ge1024}, unique_ips, top_ip, top_ips:[{ip,count}]}
hitch:   {type, t, duration_ms}
mapchange:{type, t, map, load_ms|null}
voice:   {type, t, id, last_voice_time}
shutdown:{type, t, reason}
crash:   {type, t, signal}
degraded:{type, t, error_count}
retention:{type, t, removed}
```

Все числовые значения — неотрицательные; отсутствующие — `null`; строки JSON-экранируются. Поля идентичности (`name`/`authid`/`steamid64`/`ip`) — `""`, `port`/`userid` — `-1`, `conn_s` — `0` при отсутствии данных; `fake`/`proxy` — булевы.

Расширения схемы:
- `server.edicts_churn` — знаковый Δ `pfnNumberOfEntities()` за интервал (может быть отрицательным); первый интервал после attach/mapchange — `null` (`edicts_churn_present`); вложенный `drop.server` несёт то же поле (Стадия 9, EN-5).
- `drop.detector` — best-effort детектор `rehlds_security` по подстроке reason: `movecmd`/`stringcmd`/`dlfile`/`speedhack`; промах — `""` (не влияет на drop; тип `filter` не вводится; Стадия 9, EN-1/EN-3).
- `packet.top_ips` — top-3 `{ip,count}` (фиксированный массив из 3; count `0` — пустой слот); `top_ip` сохранён (top-1) для совместимости (Стадия 9, EN-4).
- `degraded` — деградация сбора (Δ `error_count` `LogWriter`), эмитится 1 Гц только при `log->enabled()`; hard-failure/кап — console + `hldsmon_status`, тип `write_error` не вводится (Стадия 9, EN-2/EN-7).
- `retention` — Δ числа файлов, удалённых `prune()` (`removed`), pull-модель из sampler (Стадия 9, EN-P1).

---

## Раздел ревью плана (momus)

**Вердикт: PASS-WITH-REVISIONS.** Замечания и их disposition:

| ID | Severity | Учтено |
|---|---|---|
| H1 | High | INV-2 + `serialization` API в фикс-буфер без аллокаций; `test_no_alloc` (Фаза 1). |
| H2 | High | Регистрация всех cvar §12 в Bootstrap (Фаза 3, шаг 2). |
| H3 | High | Таблица QAS→метод; QAS-7 unit-тест обёрток (Фаза 3). |
| M1 | Med | Точные поля `hldsmon_status` + verify (Фаза 4). |
| M2 | Med | Полная схема — Приложение P1; фикстуры всех типов (Фаза 5). |
| M3 | Med | Добавлены `NetCmdRecord/MassDropRecord/VoiceRecord/ConnectRecord` (Фаза 1). |
| M4 | Med | Whitelist через `diff` с `exports.whitelist` (Фаза 0). |
| M5 | Med | Частичная регистрация хуков, `enable=0`, диск-full — в шагах/acceptance (Фаза 3). |
| M6 | Med | Предикат пробы host-testable + unit-тесты (Фаза 3). |
| L1 | Low | Нумерация фаз плана — согласована с дизайном (плановые 6/7/8). |
| L2 | Low | Парсинг userinfo — в предвыделенный буфер (Фаза 3, шаг 6). |

---

# Стадия 3 — `modnet` — план реализации (rev 1 (modnet) — plan-review folded; implementation gate open)

> Новый subsystem: per-`.so` атрибуция сетевого трафика чужих модулей. Авторитетный дизайн — `docs/design.md` §19 (rev 3). Настоящий план **не повторяет** прозу дизайна, а разворачивает её в исполнимые фазы; семантика — по ссылкам на §19.

## Заголовок стадии (bindings)

- **Design:** `docs/design.md` §19 — **rev 3 (modnet), design re-reviewed after revision; residuals folded**; открытые вопросы §19.11 **закрыты**. Индекс дизайна — `AGENTS.md`.
- **Design-review:** `oracle` — **PASS с остатками R1–R6; все остатки сложены (folded)** в rev 3 (§19.13). Plan-гейт разблокирован.
- **Plan-review:** `momus` — **PASS-WITH-REVISIONS (plan-review пройден); замечания MN-1..MN-8 сложены (folded)** — см. раздел «Раздел ревью плана (momus) — стадия `modnet`» (там же disposition). **Гейт имплементации открыт.**
- **Язык документации:** русский; идентификаторы/код — английский.
- **Решения владельца (§19.4/§19.11):** master-switch `hldsmon_modnet` default **OFF**; пустой `hldsmon_modnet_targets` ⇒ **встроенный список** {`easy_http_amxx_i386.so`, `sockets_amxx_i386.so`}; патчатся **только чужие модули**, GOT движкового/игрового объекта — hard-exclude; коммит только по явной просьбе.
- **Верификация:** `make` (сборка 32-бит), `make test` (host C++ core + Python analyzer), `make test-modnet-harness` (32-бит синтетический `.so`-харнесс; запускается в i686-образе/CI), `make verify-exports` (whitelist **не меняется** — всё `local: *`).
- **Коммит:** запрещён без явной просьбы владельца; один связный коммит на стадию (код + тесты + доки + запись журнала).
- **Канонический формат:** Приложение P1 настоящего плана, **расширяемое** типом `modnet` (см. «Приложение P1 — расширение» ниже).
- **Интеграционная проверка:** владелец предоставляет доступ к тестовому ReHLDS-серверу с задеплоенными `easy_http_amxx_i386.so`/`sockets_amxx_i386.so` — боевой smoke QAS-M5/M7/M10 и фиксация `sha256sum` модулей (§19.2 H4) в журнале.

**Соглашения (наследуются из базы):**
- C++17, `-m32 -fPIC -shared`, `-Wall -Wextra`, `-Werror` в CI; snake_case/lowerCamel согласованно.
- Host-ядро стадии (`src/modnet.{h,cpp}`, `ModnetRecord`, `write_modnet_json`) не включает движковые заголовки и компилируется нативным g++ (цель `test-host-core`).
- `src/modnet_got.{h,cpp}` — ELF/GOT-адаптер: использует libc/`dl`/`sys/mman` (`dl_iterate_phdr`/`dladdr`/`mprotect`) **напрямую** и **не включает `platform_linux.h` (или любой движковый заголовок)** (MN-3), чтобы линковаться и в плагин, и в 32-битный харнесс; `src/platform_linux.cpp` остаётся **без изменений**.

**Границы стадии (scope, §19.1/§19.9):** атрибуция только **прямых** сокетных вызовов в **целевых foreign-`.so`**; engine/game/`libc`/`ld.so`/сам плагин — hard-exclude. **Вне стадии:** resolver-внутренний DNS libc (N10); `read`/`write` через fd-registry (N7); интерфейс-тотал/residual (N6); активное вмешательство — запрещено (N8); обход GOT (`syscall`/inline-asm/указатели через `dlsym`, N9) не покрывается (`-fno-plt` покрывается через `R_386_GLOB_DAT`).

## Глобальные инварианты стадии `modnet`

Базовые INV-1..INV-6 продолжают действовать; ниже — расширения и новые INV-M\*. Формулировки — из `docs/design.md` §19.6.

- **INV-1 (пассивность; расширяется):** (база) ни один хук не короткозамыкает цепочку, всегда `callNext`+возврат его результата; `PreprocessPacket` никогда не `false`. **(modnet)** прозрачность распространяется и на modnet-обёртки: реальный вызов + реальный возврат + сохранение `errno`; никаких дропов/задержек на игровом пути.
- **INV-2 (hot path без аллокаций; расширяется):** (база) без `malloc/strdup/snprintf/ED_Alloc/Con_Printf` и без роста `std::string` в хуках/пути сбора. **(modnet)** путь обёртки без аллокаций: hot-path — только атомарные счётчики и сравнение диапазона; кэш диапазонов — фиксированный предвыделенный массив (INV-M5).
- **INV-3 (поток; аменд):** формулировка «весь сбор — на server thread» **ослабляется для modnet**: обёртки исполняются на потоке-вызывателе (включая пул EasyHTTP); счётчики **атомарные**; **эмиссия JSONL остаётся на server thread** (читает атомарные снимки). Базовые хуки §5 — по-прежнему на server thread.
- **INV-4 (ABI; без изменений):** доступ к `client_t`/`netchan_t` только через `engine_layout` + проба; modnet их не читает.
- **INV-5 (формат; расширяется):** каждая строка лога — валидный JSON-объект с `type` и `t` по Приложению P1; тип `modnet` — по расширению P1.
- **INV-6 (границы):** без БД; без внешнего агента/Windows.
- **INV-M1 (modnet, прозрачность):** каждая обёртка транзитивна и сохраняет `errno`/возврат (QAS-M1).
- **INV-M2 (modnet, безопасность):** fail-closed probe до включения (в т.ч. отказ `mprotect`); master-off по умолчанию; таблица оригиналов **постоянна**; восстановление — атомарная запись **после пере-валидации** слота против `dl_iterate_phdr`; периодический rescan (QAS-M2/M3/M7/M10).
- **INV-M3 (modnet, охват):** патчатся только GOT-слоты целевых символов у выбранных foreign-объектов; `libc`/`ld.so`/главный exe/сам плагин — hard-exclude (QAS-M5/M6).
- **INV-M4 (modnet, учёт):** счётчики атомарные, без потерь при конкурентных вызовах; эмиссия — согласованный снимок (QAS-M4).
- **INV-M5 (modnet, кэш диапазонов):** кэш `адрес→объект` — фиксированный предвыделенный массив с bounded-overflow (превышение ёмкости ⇒ fail-closed); публикуется lock-free атомарным обменом указателя (обёртки читают его конкурентно).

## Новые cvar'ы стадии `modnet`

| Имя | Тип | Назначение | Default | Регистрация |
|---|---|---|---|---|
| `hldsmon_modnet` | cvar | master-switch modnet | **0 (OFF)** | `bootstrap.cpp::register_cvars()` |
| `hldsmon_modnet_targets` | cvar | список целевых `.so` (basename через пробел); пусто ⇒ встроенный список | `""` | там же |
| `hldsmon_modnet_rescan_sec` | cvar | период rescan на 1 Гц server-tick, сек | **5** | там же |

## Маппинг QAS-M1..M10 → метод проверки (design §19.10; M10 включён)

| QAS | Где проверяется | Метод |
|---|---|---|
| QAS-M1 (transparency) | M-P1/M-P2/M-P5 | host-тест обёртки на фейке (сохранение `errno`/возврата) + CI-харнесс round-trip через `socketpair` |
| QAS-M2 (fail-closed probe) | M-P2/M-P5 | CI-харнесс (сломать резолв релокаций / отказ `mprotect`) + review fail-closed |
| QAS-M3 (restore-on-disable) | M-P2/M-P5 | CI-харнесс (GOT до/после disable, compare-then-store); счётчик обёртки не растёт (кроме benign in-flight) |
| QAS-M4 (thread-safe) | M-P1 | host-тест конкурентного счётчика (`std::thread`) — точная сумма |
| QAS-M5 (attribution) | M-P1/M-P2/M-P5 | host-тест range-lookup + CI-харнесс атрибуции синтетического `.so` (+ боевой smoke) |
| QAS-M6 (zero when disabled) | M-P2/M-P5 | CI-харнесс: при off GOT идентичен baseline, обёрток нет на пути |
| QAS-M7 (rescan) | M-P2/M-P5 | CI-харнесс: `dlopen` целевого `.so` после enable → подхват на rescan; немедленно на `SV_Spawn_f` |
| QAS-M8 (volume) | M-P1 | review бюджета + unit-тест `sizeof(ModnetRecord)` + правило «эмиссия только при активности» |
| QAS-M9 (lazy binding, C1) | M-P2/M-P5 | CI-харнесс двойного вызова: ≥2 инкремента, нет one-shot; `original` из `dlsym` |
| QAS-M10 (conflict/denial) | M-P2/M-P5 | review + CI-харнесс (эмуляция чужой записи в слот / отказа `mprotect`) — статус, не крах |

---

## M-P0 — Bootstrap: cvar'ы, контекст, гейт (OFF по умолчанию)

**Цель:** три cvar modnet зарегистрированы и закэшированы; состояние modnet живёт в контексте; master-switch OFF ничего не меняет; probe вызывается, но патч не выполняется, пока `hldsmon_modnet=0`.

**Файлы:** `src/plugin.h` (`CvarRefs`, `ModnetState`, `PluginContext`), `src/bootstrap.cpp`.

**Границы / non-goals:** без ELF/GOT-патча (M-P2), без эмиссии (M-P3); на этой фазе `modnet_enable()` может быть заглушкой, возвращающей fail-closed; **отдельного вызова probe при OFF нет** (MN-5).

**Шаги:**
1. `plugin.h`: в `CvarRefs` добавить `cvar_t* modnet; cvar_t* modnet_targets; cvar_t* modnet_rescan_sec;`.
2. `plugin.h`: добавить POD `ModnetState` (поле в `PluginContext`): `bool enabled=false; bool probe_ok=false; bool lazy_bound=false; uint32_t patched_objects=0; uint32_t patched_slots=0; uint32_t restored_slots=0; uint32_t conflicts=0; int target_count=0; double next_rescan_t=0.0; char targets[512]={0}; char last_error[64]={0};` — фиксированные буферы, без указателей в hot-path; `lazy_bound` фиксирует ленивую привязку объекта (политика (b), MN-1).
3. `bootstrap.cpp`: cvar-строки `s_val_modnet[]="0"`, `s_val_modnet_targets[]=""`, `s_val_modnet_rescan_sec[]="5"`; объекты `cvar_t s_cvar_modnet*`.
4. `bootstrap.cpp::register_cvars()`: `reg_cvar(... FCVAR_ARCHIVE)` для всех трёх + `engine_cvar(...)` в `c.cvars.*` (по образцу существующих).
5. `plugin_init()` (после layout-probe и `open_log()`): разобрать `hldsmon_modnet_targets` в `c.modnet.targets` (непусто ⇒ явный список; пусто ⇒ встроенный список + **стартовое уведомление** в консоль/лог, §19.4 H3). **Только если** `cvar_f(c.cvars.modnet,0.0f) > 0.5f` — вызвать `modnet_enable()` (M-P2), который сам выполняет probe. При OFF `modnet_probe()` **не вызывается** и ни один слот не изменяется даже транзиентно (QAS-M6, MN-5).
6. `plugin_shutdown()`: перед `release_state()`/закрытием лога вызвать `modnet_disable()` (полное восстановление слотов, M-P2) — гарантирует полное восстановление и на `Meta_Detach`.

**Инварианты:** INV-M2 (master-off, fail-closed probe), INV-6.
**Acceptance:** cvar'ы видны в консоли и читаются через `cvar_f`; при `hldsmon_modnet=0` ни один слот не патчится (проверяется в M-P5/QAS-M6); стартовое уведомление выводится ровно один раз при enable.
**Verify:** `make` (компилируется `-m32 -Wall -Wextra -Werror`); `make verify-exports` (whitelist без изменений).
**Риски:** уведомление не должно выводиться повторно на каждом `SV_Spawn_f`; пустой/битый список targets → fail-closed с сообщением, не крах.

---

## M-P1 — Host-testable ядро modnet (records + counters + range cache)

**Цель:** чистая host-testable логика modnet: POD-запись, no-alloc сериализация, атомарные счётчики и кэш диапазонов — покрыты host-тестами без движка.

**Файлы:** `src/metrics.h`, `src/serialization.{h,cpp}`, `src/modnet.{h,cpp}`, `src/events.{h,cpp}`, `Makefile` (`CORE_SRCS`), `tests/test_modnet.cpp`, `tests/test_modnet_serialization.cpp`, `tests/test_modnet_concurrency.cpp`, `tests/test_no_alloc.cpp`.

**Границы / non-goals:** нет ELF/GOT, нет движковых заголовков, нет файлового I/O.

**Шаги:**
1. `metrics.h`: POD `ModnetRecord` (`#pragma pack`) — поля §19.8 в порядке P1: `double t; char module[64]; char so[64]; uint64_t conns; uint64_t conn_fail; uint64_t connect_attempts; uint64_t dns_calls; uint64_t in_bytes; uint64_t out_bytes; float in_kbps; float out_kbps;` + `static_assert(sizeof(ModnetRecord))` рядом с остальными.
2. `serialization.{h,cpp}`: `size_t write_modnet_json(char* out, size_t cap, const ModnetRecord&)` — по образцу `emit_*` (measure-then-write, no-alloc): `type`,`t`(`kv_fixed(...,3)`),`module`,`so`(`kv_str`),`conns`,`conn_fail`,`connect_attempts`,`dns_calls`,`in_bytes`,`out_bytes`(`kv_u64`),`in_kbps`,`out_kbps`(`kv_fixed(...,3)`); при недостатке `cap` — вернуть 0 (без частичной строки).
3. `src/modnet.h` (host-free, без движковых заголовков):
   - `struct ModnetCounters { std::atomic<uint64_t> in_bytes, out_bytes, conns, conn_fail, connect_attempts, dns_calls; }` + методы-инкременты. **Storage (MN-7):** file-static массив `g_counters[HLDSMON_MODNET_MAX_OBJECTS]` в `src/modnet.cpp`, индексируемый объектом; **обёртки НЕ обращаются к `ctx()`** (server-thread-only) — только к своим file-static атомикам + range-cache (INV-3 amend);
   - `enum class ConnectClass { Blocking, Pending, Failed };` и `ConnectClass modnet_classify_connect(int rc, int err)` — **чистая** функция **R1**: `rc==0` ⇒ `Blocking` (учитывается в `conns`); `rc<0 && err∈{EINPROGRESS,EALREADY}` ⇒ `Pending` (только `connect_attempts`); иначе ⇒ `Failed` (`conn_fail`);
   - `class RangeCache` — фиксированный массив `{uintptr_t lo,hi; uint16_t obj;}` + `reset()`, `add(lo,hi,obj)`, `lookup(uintptr_t) -> int16_t (-1 = miss)`; флаг `overflowed` ⇒ fail-closed; публикация lock-free атомарным обменом указателя на снимок (`std::atomic<RangeCacheView*>`);
   - `struct ModnetSnapshot { uint64_t in_bytes,out_bytes,conns,conn_fail,connect_attempts,dns_calls; };` и `void modnet_snapshot(const ModnetCounters&, ModnetSnapshot&)`;
   - `void modnet_diff(const ModnetSnapshot& prev, const ModnetSnapshot& cur, double dt, ModnetRecord& out)` — Δ за интервал (wrap-safe), `kbps = Δbytes/dt/1024`, при `dt<=0` — `0.0`;
   - `bool modnet_has_activity(const ModnetRecord&)` — предикат эмиссии (любой ненулевой Δ) — единственный гейт «эмитить/пропустить».
4. `src/modnet.cpp`: реализации счётчиков (атомарные; storage — file-static массив из п.3), range-lookup (чтение опубликованного указателя), snapshot/diff. Без аллокаций в increment/lookup/diff.
5. `events.{h,cpp}`: `void write_modnet(const ModnetRecord&)` через `emit(r, &write_modnet_json)` (по образцу остальных).
6. `Makefile`: `CORE_SRCS` — добавить `src/modnet.cpp` (**M1**: иначе ядро не попадёт в host-тесты); в `HOST_FLAGS`/`test-host-core` добавить `-pthread` (для `test_modnet_concurrency.cpp`, MN-6).
7. Тесты:
   - `test_modnet.cpp` — `modnet_classify_connect` (**R1**: успех/pending/fail; `EINPROGRESS`/`EALREADY` ≠ fail; `ECONNREFUSED`/`ETIMEDOUT` ⇒ fail); `RangeCache` (попадание/промах/перекрытие/bounded-overflow ⇒ fail-closed); `modnet_diff` (обычный случай, сброс кумулятива, переполнение `uint64`); `modnet_has_activity`.
   - `test_modnet_serialization.cpp` — каждое поле; null/zero-правила; экранирование `module`/`so`; короткий `cap` ⇒ 0.
   - `test_modnet_concurrency.cpp` — N `std::thread` конкурентно инкрементят; сумма точная (QAS-M4); линкуется с `-pthread` (п.6, MN-6).
   - `test_no_alloc.cpp` — расширить: `modnet_diff` + `write_modnet_json` без аллокаций.

**Инварианты:** INV-2 (extend), INV-M1, INV-M4, INV-M5.
**Acceptance:** host-тесты зелёные; `sizeof(ModnetRecord)` закреплён; конкурентный тест даёт точную сумму; `test_no_alloc` — 0 аллокаций на пути modnet.
**Verify:** `make test`.
**Риски:** правило «эмитировать только активные» должно жить в ядре (`modnet_has_activity`), иначе QAS-M8/M6 непроверяемы host-тестом.

---

## M-P2 — ELF/GOT-адаптер (`modnet_got`)

**Цель:** находить целевые объекты, разбирать релокации, устанавливать/пере-проверять/восстанавливать GOT-слоты, проходить fail-closed probe; 32-бит; линкуется и в плагин, и в харнесс (M-P5).

**Файлы:** `src/modnet_got.{h,cpp}`, `src/plugin.h` (состояние из M-P0); `src/platform_linux.{h,cpp}` — **без изменений** (MN-3).

**Границы / non-goals:** без движковых заголовков; без эмиссии/каденции (M-P3); тесты — харнесс (M-P5).

**Шаги (порядок обязателен):**
1. **Обход объектов (L2):** `dl_iterate_phdr`; колбэк читает `struct dl_phdr_info` (`dlpi_addr`, `dlpi_phdr`, `dlpi_name`), находит `PT_DYNAMIC` сканированием `dlpi_phdr`. **Eligible** = `dlpi_name` начинается с `/`; **hard-exclude**: `libc.so*`/glibc, `ld-*.so*`, главный exe, сам плагин (§19.4, INV-M3).
2. **Разбор `PT_DYNAMIC` (L3):** `DT_SYMTAB`/`DT_STRTAB`/`DT_STRSZ`, `DT_JMPREL`/`DT_PLTRELSZ`/`DT_PLTREL`, `DT_REL`/`DT_RELSZ`/`DT_RELENT`; PLT-релокации — `DT_REL` с типом `R_386_JMP_SLOT`; data (в т.ч. `-fno-plt`) — `R_386_GLOB_DAT` (M4).
3. **Набор символов (§19.3, reconciled M7):** байты — `send`,`recv`,`sendto`,`recvfrom`,`sendmsg`,`recvmsg`; соединения — `connect`,`accept`; DNS — `getaddrinfo` (только счётчик, **R3**). `socket`/`close` **исключены** (fd-registry отложен, N7).
4. **Истинный оригинал (C1/R4):** `original = dlsym(RTLD_DEFAULT, name)` — **никогда** из текущего значения слота; только `RTLD_DEFAULT` (без `RTLD_NEXT`). Пары `(slot, original)` — в **постоянную** таблицу (никогда не освобождается, H2).
5. **BIND_NOW (R5) — политика зафиксирована (MN-1):** определить `DT_BIND_NOW`/`DF_BIND_NOW`/полный RELRO. Для **лениво-связанного** объекта применяется **политика (b): re-verify-on-rescan** — на каждом `modnet_rescan()` проверять, что слот всё ещё содержит нашу обёртку, и **перепатчивать при clobber** (именно это требует харнесс M-P5/QAS-M9, собирающий ленивый target и ожидающий работающий патч с ≥2 инкрементами). Fail-closed остаётся **только** для случая, когда GOT объекта **невозможно сделать writable** (`mprotect` отказан PaX/SELinux) — объект не патчится, `last_error` в статусе. Иной ветки «не патчить ленивый объект» нет; факт ленивой привязки отражается в статусе (`lazy_bound`).
6. **mprotect (M3):** перед первым `mprotect` — снимок прав страницы из `/proc/self/maps`; `mprotect` **минимальной** страницы в RW; после записи — вернуть **ровно** исходные права; **никогда RWX**.
7. **Установка:** атомарная 32-бит запись адреса обёртки в слот; инкремент `patched_slots`/обновление `patched_objects`.
8. **Обёртки (§19.3):** для byte-символов — вычислить длину возврата (`>=0`), определить владельца по адресу возврата (`RangeCache::lookup`, INV-M5), инкремент `in_bytes`/`out_bytes`, вызов `original`, возврат результата с сохранением `errno` (INV-M1). Для `connect`: `rc==0` ⇒ `conns`; `rc<0 && errno∈{EINPROGRESS,EALREADY}` ⇒ **pending** (ни `conns`, ни `conn_fail`, **R1**); иначе ⇒ `conn_fail`. `connect_attempts` — **все** `connect()`. `accept` — успех ⇒ `conns`. `getaddrinfo` — только `dns_calls` (**R3**), без байт. При промахе range-lookup — не считать (bounded).
9. **Probe fail-closed (§19.4):** (a) релокации целевых символов найдены; (b) real round-trip `socketpair` через `send`/`recv` (M6; покрывает только это подмножество, R6); (c) счётчик растёт при **≥2** последовательных вызовах (C1, QAS-M9); (d) `mprotect` разрешён (PaX/SELinux). Любой провал ⇒ `probe_ok=false`, `enabled=false`, предупреждение, плагин грузится нормально. **Жизненный цикл (MN-5):** проба **устанавливает обёртки временно** для socketpair-round-trip и **восстанавливает слоты до возврата** (steady-state OFF = без изменённых слотов, QAS-M6); вызывается **только из `modnet_enable()`**, не из `plugin_init()` при OFF.
10. **Восстановление (R2, §19.3 п.7):** `modnet_restore_slot(...)`: пере-валидировать маппинг против свежего `dl_iterate_phdr` (объект по тому же `base`, слот в границах); **compare-then-store** — писать оригинал **только если** текущее значение слота == наша обёртка; `mprotect` RW → атомарная 32-бит запись → возврат исходных прав; инкремент `restored_slots`.
11. **Rescan:** `modnet_rescan()` — повторный обход → патч новых слотов; при unload инвалидация записи (защищается compare-then-store); повторная **lock-free** публикация `RangeCache` атомарным обменом указателя (INV-M5); обновление `patched_objects/slots`; детект чужой записи в слот ⇒ `conflicts++` (QAS-M10, не крах).
12. **Kill-switch:** `modnet_enable()` — `modnet_probe()` (**единственная точка вызова пробы**), затем патч; `modnet_disable()` — восстановить **все** слоты (п.10), обнулить `enabled`; `Meta_Detach` вызывает disable. Рантайм-синхронизация master-switch — M-P3 п.1 (MN-2).
13. **`modnet_got.cpp` — header-free 32-bit TU (MN-3):** использует libc/`dl`/`sys/mman` (`dl_iterate_phdr`/`dladdr`/`mprotect`, чтение `/proc/self/maps`) **напрямую**; **не включает `platform_linux.h` и движковые заголовки**; `platform_linux.{h,cpp}` **не расширяется**. Компилируется и в плагин, и в 32-битный харнесс (M-P5), что и обеспечивает линковку без движковых заголовков.
14. **`modnet_got.h` API:** `bool modnet_probe(); void modnet_enable(); void modnet_disable(); void modnet_rescan(); void modnet_request_rescan();` + аксессоры состояния для control.

**Инварианты:** INV-M1..INV-M5; INV-2 (обёртка no-alloc).
**Acceptance:** компилируется `-m32`; probe fail-closed на искусственно сломанных кейсах; слоты ставятся/восстанавливаются; `errno` сохраняется; ≥2 вызова ⇒ растущий счётчик (QAS-M9); при off — 0 изменённых слотов (QAS-M6); чужой слот ⇒ conflict/status (QAS-M10).
**Verify:** `make`; `make test-modnet-harness` (M-P5).
**Риски:** корректность разбора 32-бит ELF (границы `DT_SYMTAB`/`DT_RELENT`); ленивая привязка — слот может быть перезаписан in-flight резолвером, покрыто re-verify+перепатчем на каждом rescan (политика (b), MN-1); чужие GOT-патчеры (ReUnion/ReChecker) — фиксируются в статусе, не крах.

---

## M-P3 — Движковая интеграция (эмиссия, rescan-каденция, control)

**Цель:** на 1 Гц server-tick эмитить `modnet`-records для активных целевых объектов; rescan по каденции и немедленно на `SV_Spawn_f`; `hldsmon_status` отражает состояние modnet.

**Файлы:** `src/sampler.cpp`, `src/hooks.cpp` (`h_SV_Spawn_f`), `src/control.cpp`, `src/plugin.h`, `src/events.{h,cpp}` (готово в M-P1).

**Границы / non-goals:** новых hookchain-хуков нет; rescan-триггеры — в sampler/hooks, **не** в hook_manager (L1); никаких HTTP/сети.

**Шаги:**
1. **Синхронизация master-switch (MN-2):** на каждом server-tick (`sampler_on_frame()`) сравнивать `cvar_f(c.cvars.modnet,0.0f) > 0.5f` с `c.modnet.enabled`; при `off→on` — `modnet_enable()`, при `on→off` — `modnet_disable()` (атомарный restore GOT-слотов). Это live-триггер QAS-M3 в рантайме; зеркалит существующий гейт `plugin_enabled()`.
2. `sampler.cpp`: функция `modnet_emit_interval(double now_epoch)`: если `c.modnet.enabled` — для каждого пропатченного целевого объекта снять атомарный снимок, построить `ModnetRecord` (`modnet_diff`, `kbps = Δ/dt/1024`, `dt` = интервал между вызовами эмиссии), эмитить **только при `modnet_has_activity`** (`events::write_modnet`); обновить prev-снимки. Пропуск объекта без активности = «gap = zero» для анализатора (QAS-M8).
3. `sampler.cpp::do_server_tick()`: после `emit_netcmd_interval` вызвать `modnet_emit_interval(now_epoch)`.
4. `sampler.cpp::sampler_on_frame()` (server tick): если `c.modnet.enabled` и `now_real >= c.modnet.next_rescan_t` — `modnet_rescan()`; `c.modnet.next_rescan_t = now_real + max(1.0, cvar_f(c.cvars.modnet_rescan_sec,5.0f))` (Q1).
5. `hooks.cpp::h_SV_Spawn_f()`: после обработки mapchange и **до** `callNext` вызвать `modnet_request_rescan()` (немедленный rescan); `callNext` и возврат сохраняются (INV-1).
6. `control.cpp::cmd_status()`: добавить строку modnet: `enabled`, `probe_ok`, `lazy_bound`, `targets`, `patched_objects`, `patched_slots`, `restored_slots`, `conflicts`, `last_error`.

**Инварианты:** INV-3 (amend: обёртки на потоке-вызывателе, эмиссия на server thread из атомарного снимка), INV-4 (modnet не читает `client_t`), INV-M2 (rescan + runtime restore), INV-M4.
**Acceptance:** при `hldsmon_modnet=1` — один `modnet`-record/с на активный объект; при `0` — ноль строк; рантайм `hldsmon_modnet 0` восстанавливает GOT (QAS-M3, MN-2); `dlopen`-нутый позже объект подхватывается ≤ `rescan_sec`; `SV_Spawn_f` даёт немедленный rescan; `hldsmon_status` содержит modnet-поля.
**Verify:** `make && make test`; ручной smoke на тестовом сервере (QAS-M3/M5/M7, задокументировать в journal).
**Риски:** эмиссия на server thread читает только атомарные снимки; не читать неатомарные структуры из обёрток (INV-3).

---

## M-P4 — Инструменты анализа (парсер + ряды + корреляция)

**Цель:** `hldsmon.py` парсит `modnet`, строит per-`.so` ряды, коррелирует с `drop`/`hitch`, включает сводку в digest.

**Файлы:** `tools/analyzer/hldsmon.py`, `tools/analyzer/tests/fixtures/modnet.jsonl`, `tools/analyzer/tests/test_hldsmon.py`, `tools/analyzer/README.md`.

**Границы / non-goals:** только stdlib; без сети; не изменяет входные файлы; residual/N6 не считается.

**Шаги:**
1. `KNOWN_TYPES` += `"modnet"` (иначе parser отбросит записи).
2. `modnet_series(records)` — `{ "module|so|run": {t,conns,conn_fail,connect_attempts,dns_calls,in_bytes,out_bytes,in_kbps,out_kbps:[...]} }`; разрез ряда по `(module, so, run)`. **`run` (MN-8) — процесс-уровневый** и переиспользует существующий `_attach_runs` (границы — `shutdown`/`crash`); отдельного object-reset маркера **нет**, поэтому unload/reload объекта внутри одного `run` **не разрезается** (известное ограничение; `(module, so)` всё равно разделяет модули).
3. `modnet_summary(records)` — агрегаты по модулям: Σ bytes, Σ conns/conn_fail/connect_attempts/dns_calls, peak kbps.
4. `correlate_modnet(records, window_s=5.0)` — в окне каждого `drop`/`hitch` искать `modnet`-ряд(ы) с аномалией (всплеск `in_kbps`/`out_kbps` ≥ порога-константы, либо ненулевые `conn_fail`); результат — список `{event_t, kind, module, so, metric, value}` (детерминированная сортировка).
5. `build_digest`: добавить `modnet`: Σ in/out bytes, per-module top, число `conn_fail`, число `modnet`-строк.
6. `cmd_summary`: строка `modnet: N records, modules: ...`.
7. `tests/fixtures/modnet.jsonl`: `modnet` для ≥2 модулей (`easy_http_amxx_i386`, `sockets_amxx_i386`), включая активные и пустые интервалы, `conn_fail`, `dns_calls`; плюс `drop`/`hitch` для корреляции.
8. Тесты: `modnet_series` (разрез по run/module/so); `modnet_summary`; `correlate_modnet`; `digest` детерминирован (двойной прогон); tolerant-парсер пропускает `modnet` без падения на старых данных.
9. `tools/analyzer/README.md`: добавить `modnet` в список типов и подкоманд.

**Инварианты:** INV-5 (расширение P1).
**Acceptance:** `python3 -m unittest` зелёный; `summary`/`digest` на `modnet.jsonl` детерминированы; корреляция находит `modnet`-всплеск рядом с drop/hitch.
**Verify:** `make test` (часть `test-python`).
**Риски:** «gap = zero» — пропуск интервала не трактуется как `null`/сброс; ряд разрезается только по `run`.

---

## M-P5 — 32-битный синтетический CI-харнесс (attribution/restore/rescan/BIND_NOW)

**Цель:** детерминированная проверка GOT-патча на **синтетическом** 32-битном `.so` в i686-окружении без реального сервера.

**Файлы:** `tests/modnet_harness/target.c`, `tests/modnet_harness/harness.cpp`, `tests/modnet_harness/README.md`, `Makefile` (цель `test-modnet-harness`), `.github/workflows/ci.yml` (job `plugin`).

**Границы / non-goals:** не запускает реальный ReHLDS; не покрывает EasyHTTP/`sockets` как таковые (боевой smoke — M-P6); не host-тест (нужен `-m32`).

**Шаги:**
1. `target.c`: минимальный `-shared -fPIC -m32` объект, экспортирующий `modnet_target_send(int fd)`, `modnet_target_recv(int fd)`, `modnet_target_connect(...)`, `modnet_target_getaddrinfo(...)`; собирается в двух вариантах — **lazy** (default) и **BIND_NOW** (`-Wl,-z,now`). Вызовы должны идти через PLT/GOT (не `-fno-plt`), чтобы покрыть `R_386_JMP_SLOT`. lazy-вариант проверяется по **политике (b)** (M-P2 п.5); BIND_NOW-вариант — как отдельный успешный патч (MN-1).
2. `harness.cpp` (линкует `src/modnet_got.cpp` + `src/modnet.cpp`, без движковых заголовков) проверяет:
   - **QAS-M5 attribution:** `dlopen` target → патч → вызов через обёртку (socketpair) → байты атрибутированы target, не libc.
   - **QAS-M1 round-trip:** реальные `send`/`recv` через обёртку возвращают то же; `errno` не искажён (в т.ч. `EAGAIN` на неблокирующем fd).
   - **QAS-M9 lazy (C1, policy (b), MN-1):** lazy-target **патчится** (не fail-closed); ≥2 вызова → счётчик растёт (не one-shot); слот не уходит в `_dl_runtime_resolve` поверх обёртки; после искусственного clobber слота (запись чужого значения) — `modnet_rescan()` **перепатчивает** и счётчик снова растёт (re-verify-on-rescan).
   - **R1:** `modnet_target_connect` на неблокирующем fd → `EINPROGRESS` = pending: `connect_attempts++`, `conns`/`conn_fail` без изменений; блокирующий успех → `conns++`; отказ → `conn_fail++`.
   - **QAS-M3 restore:** `modnet_disable()` → слот == оригинал; повторные вызовы не инкрементят обёртку (допускается один benign in-flight).
   - **R2:** слот с чужим значением → `modnet_restore_slot` не пишет (compare-then-store).
   - **QAS-M6 off:** при `enabled=0` GOT идентичен baseline (снимок до/после).
   - **QAS-M7 rescan:** `modnet_rescan()` после `dlopen` второго target подхватывает его.
   - **QAS-M2 fail-closed:** симулировать отсутствие релокаций / отказ `mprotect` → `probe_ok=false`, без краха.
   - **QAS-M10 conflict:** записать в слот чужое значение → conflict фиксируется, restore не пишет поверх.
   - **R3:** вызов `getaddrinfo` через target → `dns_calls` инкрементируется, байты не растут.
3. `Makefile`: цель `test-modnet-harness`: собрать `target_lazy.so`/`target_now.so` (`-m32 -shared -fPIC`) и `harness` (`-m32`, линкует `src/modnet_got.cpp src/modnet.cpp -ldl`), затем запустить; ненулевой код при провале. Цель **не** входит в `make test` (host) — запускается в i686-окружении.
4. `ci.yml`: в job `plugin` шаг `make test-modnet-harness` выполняется **внутри того же `docker run … manylinux2014_i686 … bash -lc '… make && make verify-exports && make test-modnet-harness'`**, что и `make`/`verify-exports` (plain-шаг `ubuntu-latest` без i686-multilib упадёт — MN-4).
5. `tests/modnet_harness/README.md`: как локально прогнать в i686-образе.

**Инварианты:** INV-M1..INV-M5 (проверяются вживую).
**Acceptance:** `make test-modnet-harness` зелёный в i686-образе; lazy-ветка проверяет re-verify+перепатч (policy (b)); CI job `plugin` запускает харнесс внутри `manylinux2014_i686` и падает при регрессии.
**Verify:** `docker run --rm -v "$PWD":/src -w /src quay.io/pypa/manylinux2014_i686 bash -lc 'make test-modnet-harness'`; CI job `plugin` (шаг в docker-wrap, MN-4).
**Риски:** target обязан проходить через GOT (иначе QAS-M9/M5 не проверяются); линковка `modnet_got.cpp` в харнесс требует отсутствия движковых заголовков (соблюдено в M-P2).

---

## M-P6 — Верификация, документация, остатки R1/R2/R3

**Цель:** сквозная верификация стадии, фиксация H4/H5-фактов и закрытие остатков R1–R3, боевой smoke.

**Файлы:** `docs/journal.md` (запись — M-P7), `tools/analyzer/README.md`, `README.md`, `docs/plan.md` (Приложение P1 — расширение сделано), `include/VENDORED.md` (если менялся).

**Обязательные шаги — остатки (residual items):**
- **R1 (неблокирующий `connect`):** реализовано в **M-P1 п.3** (`modnet_classify_connect`) и **M-P2 п.8**; проверено харнессом **M-P5** (`EINPROGRESS`/`EALREADY` = pending; `conns` = успешные блокирующие `connect()`+`accept()`; best-effort для неблокирующих модулей EasyHTTP).
- **R2 (compare-then-store при restore):** реализовано в **M-P2 п.10**; проверено харнессом **M-P5** (слот с чужим значением не перезаписывается).
- **R3 (поле `dns_calls`):** реализовано в **M-P1 п.1/п.2** и **M-P2 п.8** (`getaddrinfo` — только счётчик); поле в `ModnetRecord`/P1 и в анализаторе **M-P4**; проверено `make test` + харнессом.

**Прочие шаги:**
1. **H4/H5 (боевой smoke):** на тестовом сервере выполнить `ldd` + `nm -D` проверки не-динамичности `libcurl`/`c-ares`/`OpenSSL`/`zlib` и семейства `send`/`recv` (§19.2); зафиксировать путь + `sha256sum` модулей в журнале.
2. **QAS-M5/M7/M10 (боевой):** `hldsmon_modnet 1` → `modnet`-строки per-`.so`; `sockets` атрибутируется `sockets_amxx_i386.so`; `hldsmon_modnet 0` восстанавливает; `hldsmon_status` отражает.
3. **Hygiene:** `make clean && make && make verify-exports && make test`; обновить `README.md` (тип `modnet`, три cvar'а) и `tools/analyzer/README.md`; при изменениях вендора — `include/VENDORED.md`.
4. **QAS-M8 (объём):** на активном объекте оценить прирост строк/сутки.

**Acceptance:** `make`/`make test`/`make verify-exports` зелёные; `make test-modnet-harness` зелёный; whitelist без изменений; боевой smoke задокументирован; H4/H5 зафиксированы; R1/R2/R3 закрыты со ссылками на тесты.
**Verify:** `make clean && make && make verify-exports && make test`; `docker run … manylinux2014_i686 … make test-modnet-harness`; CI job `plugin` + `tests`.
**Риски:** боевой smoke требует реального сервера с модулями (вне CI); при их отсутствии — пометить как открытый пункт, не блокирующий code-часть.

---

## M-P7 — Запись стадии (journal)

**Файлы:** `docs/journal.md`. **Acceptance:** контекст; что сделано (M-P0..M-P6); решения и почему (BIND_NOW-политика, fail-closed, встроенный список targets); H4/H5-факты (`sha256sum` модулей); затронутые артефакты; остатки R1–R3 со ссылками; открытые темы (N6/N7); следующие шаги; индексы `AGENTS.md` актуальны.

---

## M-P8 — Коммит (гейт)

Один связный коммит (код + тесты + доки + журнал) — **только по явной просьбе владельца**. Иначе: «готово к коммиту».

---

## Definition of Done (стадия `modnet`)

- [ ] `make` собирает `.so` (32-бит, `-Wall -Wextra -Werror`) без предупреждений; `make verify-exports` — whitelist **без изменений**.
- [ ] `make test` зелёный: host-ядро modnet (`src/modnet.cpp` в `CORE_SRCS`) + `test_no_alloc` + Python analyzer.
- [ ] `make test-modnet-harness` зелёный (32-бит): attribution/restore/rescan/lazy(C1)/fail-closed/conflict.
- [ ] INV-M1: прозрачность обёрток (возврат/`errno`) — host-тест + харнесс.
- [ ] INV-M2: fail-closed probe; master-off; постоянная таблица оригиналов; compare-then-store restore — харнесс/review.
- [ ] INV-M3: hard-exclude libc/ld.so/main-exe/plugin; eligible только `/`-пути — харнесс/review.
- [ ] INV-M4: атомарные счётчики без потерь — host-тест конкурентности (exact sum).
- [ ] INV-M5: фиксированный range-cache, lock-free публикация, bounded-overflow ⇒ fail-closed — host-тест + харнесс.
- [ ] Расширение P1 (`modnet`) сериализуется и парсится; null/zero-правила покрыты тестами.
- [ ] cvar'ы `hldsmon_modnet`(0)/`_targets`("")/`_rescan_sec`(5) зарегистрированы; `=0` ничего не патчит.
- [ ] R1/R2/R3 закрыты с трассировкой на тесты/харнесс.
- [ ] `hldsmon_status` показывает состояние modnet; rescan по каденции + немедленно на `SV_Spawn_f`.
- [ ] `docs/journal.md` заполнен; индексы обновлены.
- [ ] Коммит — только при явной просьбе.

---

## Приложение P1 — расширение: тип `modnet` (стадия `modnet`)

Дополняет каноническую схему P1 (стадия «база v1», выше). Общие поля/правила P1 сохраняются (`type`,`t`; числа неотрицательны; строки JSON-экранируются).

```
modnet: {type, t, module, so, conns, conn_fail, connect_attempts, dns_calls,
         in_bytes, out_bytes, in_kbps, out_kbps}
```

| Поле | Тип | Семантика |
|---|---|---|
| `type` | string | `"modnet"` |
| `t` | float | wall-clock epoch seconds (момент эмиссии) |
| `module` | string | короткая метка модуля (напр. `easy_http_amxx_i386`) |
| `so` | string | basename загруженного объекта |
| `conns` | uint | Δ успешных **блокирующих** `connect()` (return 0) + успешных `accept()` за интервал; **best-effort** для неблокирующих модулей (R1) |
| `conn_fail` | uint | Δ `connect()` с ошибкой (`return -1`, `errno ∉ {EINPROGRESS,EALREADY}`) (R1) |
| `connect_attempts` | uint | Δ **всех** вызовов `connect()` (любой результат; включая pending, R1) |
| `dns_calls` | uint | Δ вызовов `getaddrinfo` (без байт, R3) |
| `in_bytes` | uint64 | Δ принятых байт за интервал |
| `out_bytes` | uint64 | Δ отправленных байт за интервал |
| `in_kbps` | float | `in_bytes / Δt / 1024` |
| `out_kbps` | float | `out_bytes / Δt / 1024` |

**Правила null/zero (modnet):**
- Все счётчики и `in_bytes`/`out_bytes` — неотрицательные целые; **`null` для них не используется**.
- Запись эмитится **только для объектов с активностью** за интервал (любой ненулевой Δ); объект без активности **не даёт строки** — анализатор трактует пропуск как **ноль** (не как `null`/сброс ряда). Ряд разрезается анализатором по `(module, so, run)`.
- Внутренние счётчики — **кумулятивные атомарные `uint64`**; в JSONL эмитятся **Δ за интервал**; при unload/reload объекта счётчики сбрасываются (§19.8).
- `in_kbps`/`out_kbps` — float (3 знака); при `Δt <= 0` — `0.0`.
- `module`/`so` — строки; `""` при неизвестности (на практике не встречается).
- `in_bytes`/`out_bytes` — только **прямые** сокетные вызовы объекта (scope §19.1); resolver-внутренний DNS libc не входит (N10); `dns_calls` — только счётчик вызова `getaddrinfo`, без байт.

---

## Раздел ревью плана (momus) — стадия `modnet`

**Ревьюер:** `momus` (независимый). **Основание:** `docs/design.md` §19 (rev 3), базовый план §1/Приложение P1, реальный репозиторий (`src/`, `Makefile`, `.github/workflows/ci.yml`, `tools/analyzer/`). **Plan rev:** 1.

**Вердикт: PASS-WITH-REVISIONS.** План исполним и достоверно разворачивает §19: фазы M-P0..M-P8 покрывают компоненты §19.7, критичный C1 и остатки R1/R2/R3 замаплены на конкретные шаги, а QAS-M1..M10 имеют непустой метод проверки. Decision-completeness нарушен тремя High-пунктами (MN-1..MN-3): их надо сложить до имплементации; остальное — локальные уточнения, перепроектирования не требующие.

**Таблица замечаний (ID | Severity | Замечание (расположение) | Рекомендуемый disposition):**

| ID | Severity | Замечание (расположение) | Рекомендуемый disposition |
|---|---|---|---|
| MN-1 | High | **BIND_NOW-политика не зафиксирована.** M-P2 п.5 оставляет выбор «(a) fail-closed для лениво-связанного объекта **либо** (b) re-verify + перепатч при clobber» исполнителю. Но M-P5/QAS-M9 строит **ленивый** target и проверяет его как **пропатченный** (≥2 инкремента, слот не уходит в `_dl_runtime_resolve`). При выборе (a) ленивый target не патчится, и проверки M-P5/QAS-M9/M5/R1 над ним невыполнимы — внутреннее противоречие фаз. | Зафиксировать политику в M-P2 п.5 (рекомендуется (b): re-verify + перепатч — именно её требует харнесс M-P5), **либо** сделать lazy-ветку M-P5 условной и добавить отдельный harness-кейс политики (a). |
| MN-2 | High | **Рантайм kill-switch не реализован шагом.** §19.4 требует «`hldsmon_modnet 0` в рантайме атомарно восстанавливает слоты». В плане M-P0 п.5 гейтит enable только при `plugin_init()`, а M-P3 проверяет лишь флаг `c.modnet.enabled` и **нигде не перечитывает cvar**. QAS-M3 (disable→restore) не имеет исполнимого триггера; мастер-switch фактически действует только на старте. | Добавить в M-P3 явный шаг синхронизации: на server-tick сравнивать `cvar_f(c.cvars.modnet)` c `c.modnet.enabled` и вызывать `modnet_enable()`/`modnet_disable()` при 0↔1 (по образцу live-гейта `plugin_enabled()`); сослаться на QAS-M3. |
| MN-3 | High | **Противоречие о `src/modnet_got.cpp`.** «Соглашения стадии» и M-P5 п.2 требуют, чтобы `modnet_got.{h,cpp}` был **без движковых заголовков** и линковался в 32-бит харнесс; дизайн §19.7 называет `modnet_got.cpp` **engine TU**, а M-P2 п.13 размещает `dl_iterate_phdr`/`dladdr`/`mprotect` в `platform_linux` (engine TU — тянет `interface.h`/`extdll.h`). Если `modnet_got.cpp` включает `platform_linux.h`, харнесс M-P5 без движковых заголовков не соберётся/не слинкуется. | Разрешить однозначно: `modnet_got.cpp` вызывает libc/`dl` напрямую и **не** включает `platform_linux.h` (харнесс-совместимо), а «engine TU» в §19.7 — метка, которую надо уточнить; **либо** вынести общие helper'ы в header-free TU, компилируемый и плагином, и харнессом. Переформулировать/убрать M-P2 п.13, чтобы не дублировать путь. |
| MN-4 | Med | **Шаг CI для `make test-modnet-harness`.** M-P5 п.4 предписывает «добавить шаг» в job `plugin`, но существующий job собирает **внутри** `docker run … manylinux2014_i686` (ci.yml, шаг «Build in manylinux2014_i686»). Простой runner-шаг на `ubuntu-latest` без i686-multilib упадёт; docker-wrap подразумевается неявно. | Явно указать, что шаг `make test-modnet-harness` выполняется в том же `manylinux2014_i686`-образе (обернуть `docker run … bash -lc '… make test-modnet-harness'`), как `make`/`verify-exports`. |
| MN-5 | Med | **Проба vs master-off / QAS-M6 и двойной probe.** M-P0 п.5 вызывает `modnet_probe()` безусловно (в т.ч. при OFF). По §19.4 проба делает реальный `socketpair`-round-trip «через обёртку» и проверяет рост счётчика ≥2× — т.е. временно устанавливает обёртку; не сказано, восстанавливает ли проба слот, что задевает QAS-M6 («при off ни один слот не изменён»). Плюс M-P2 п.12 вызывает probe повторно внутри `enable()`. | Уточнить жизненный цикл: проба обязана восстановить слот до возврата (steady-state OFF = без изменённых слотов) **либо** выполняется только как часть `enable()`. Согласовать с QAS-M6, убрать двойной вызов probe. |
| MN-6 | Low | **`std::thread` без `-pthread`.** M-P1 п.7 добавляет `test_modnet_concurrency.cpp` с `std::thread`, но `Makefile` `HOST_FLAGS`/`test-host-core` не добавляют `-pthread` (линкуется не на всех toolchains/glibc). | Добавить `-pthread` в `HOST_FLAGS` (или в компиляцию `test_modnet_concurrency`) в рамках M-P1 п.7. |
| MN-7 | Low | **Хранилище атомарных счётчиков.** Обёртки исполняются на потоке-вызывателе (INV-3 amend), но `PluginContext`/`ctx()` документированы «server thread only» (`plugin.h`). План объявляет `ModnetCounters` (atomic), но не указывает объект, из которого обёртки инкрементируют, не трогая server-thread-only `PluginContext`. | Зафиксировать storage (напр. file-static массив счётчиков в `modnet.cpp`, индексируемый объектом); явно запретить обёрткам обращаться к `ctx()` из чужих потоков. |
| MN-8 | Low | **Граница ряда `modnet` в анализаторе.** M-P4 п.2 разрезает ряд по `_attach_runs` (границы — `shutdown`/`crash`), тогда как §19.8 сбрасывает счётчики при **unload/reload** объекта; границы могут не совпасть. | Уточнить в M-P4, что `run` в ключе — прокси процесса, а не объекта, **либо** определить явный маркер сброса счётчиков объекта при rescan/unload. |

**Что проверено и корректно (не переусердствовать):**
- **QAS-M1..M10** — все десять имеют непустой, конкретный метод (host-тест / 32-бит харнесс / документированный боевой smoke); формулировок вида «verify it works» нет.
- **Остатки R1/R2/R3** замаплены на конкретные шаги и тесты: R1 — `modnet_classify_connect` (M-P1 п.3) + M-P2 п.8 + M-P5; R2 — compare-then-store (M-P2 п.10) + M-P5; R3 — `dns_calls` (M-P1 п.1/п.2 + M-P2 п.8 + P1 + M-P4). **C1** закреплён корректно: истинный `original` = `dlsym(RTLD_DEFAULT)` и только `RTLD_DEFAULT` (M-P2 п.4); probe зовёт символ ≥2× (M-P2 п.9c).
- **Инварианты INV-M1..M5** перенесены согласованно с §19.6; аменд INV-3 явно ослабляет базовую формулировку для modnet (обёртки на потоке-вызывателе, счётчики атомарные, эмиссия на server thread) и не противоречит базовому INV-3; DoD ссылается на все INV-M\*.
- **Приложение P1 (расширение `modnet`)** содержит все поля §19.8, включая `conn_fail`/`connect_attempts`/`dns_calls`; правила null/zero заданы; каждая строка сохраняет `type`+`t` — INV-5 не нарушается.
- **cvar-трио** (`hldsmon_modnet`(0)/`_targets`("")/`_rescan_sec`(5)) регистрируется в `bootstrap.cpp::register_cvars()` с кэшем указателей в `CvarRefs` и чтением через `cvar_f` — совпадает с реальным кодом и конвенцией базы (H2).
- **Анализатор** M-P4 следует конвенциям репо (`KNOWN_TYPES`, `_attach_runs`, `build_digest`, `cmd_summary`, фикстуры в `tests/fixtures/`) и покрывает парсинг/ряды/корреляцию с фикстурой и тестами.
- **Обязательные элементы не пропущены:** строка `hldsmon_status` (M-P3 п.5; функция `cmd_status()` в `control.cpp` — имя верное), restore на `Meta_Detach` (M-P0 п.6 через `plugin_shutdown()`, реально вызываемый из `Meta_Detach`; M-P2 п.12), пере-валидация при rescan/unload (M-P2 п.11), снимок прав `mprotect` из `/proc/self/maps` (M-P2 п.6), изменение `CORE_SRCS` (M-P1 п.6), rescan-триггер в `hooks`/`sampler` (L1), hard-exclude и N6/N7/N10 (INV-M3, M-P2 п.1) как явно вне стадии.
- **Верификация:** цели `make`/`verify-exports` реально существуют в `Makefile`; `make test-modnet-harness` — явное **обязательство** плана (M-P5 п.3) и отсутствует в репо сейчас (корректно отражено); имена CI-job `tests`/`plugin` реальны (ci.yml).
- **Risks** отражают реальные риски (ленивая привязка, чужие GOT-патчеры, PaX/SELinux `mprotect`, неблокирующий connect, верификация в CI) и привязаны к fail-closed/статусу.

**Гейт имплементации: ЗАКРЫТ** до складывания High-пунктов **MN-1..MN-3** (обязательно) и, желательно, Med MN-4/MN-5. Замечания — локальные уточнения плана; **перепроектирования и повторного ре-ревью дизайна не требуется**, дизайн §19 не оспаривается. После внесения правок гейт имплементации **открывается**; MN-6..MN-8 допустимо закрыть по ходу реализации.

**Disposition — folded в rev 1 (modnet); гейт имплементации ОТКРЫТ:**

| ID | Severity | Куда сложено |
|---|---|---|
| MN-1 | High | M-P2 п.5 — политика **(b)** зафиксирована (re-verify-on-rescan + перепатч); fail-closed только при отказе `mprotect`; M-P5 п.1/п.2 (lazy-ветка) утверждает поведение (b) + clobber/re-patch. |
| MN-2 | High | M-P3 п.1 — runtime-синхронизация `cvar_f(c.cvars.modnet)` ↔ `c.modnet.enabled` → `modnet_enable()`/`modnet_disable()`; Acceptance ссылается на QAS-M3. |
| MN-3 | High | M-P2 Файлы/п.13 + «Соглашения стадии» — `modnet_got.cpp` = header-free 32-bit TU (libc/`dl`/`sys/mman`), `platform_linux` без изменений; одна фраза в `docs/design.md` §19.7 уточнена. |
| MN-4 | Med | M-P5 п.4/Verify — `make test-modnet-harness` внутри того же `docker run … manylinux2014_i686`. |
| MN-5 | Med | M-P0 п.5 (probe не вызывается при OFF) + M-P2 п.9/п.12 (probe только в `modnet_enable()`, restore до возврата, без дубля). |
| MN-6 | Low | M-P1 п.6/п.7 — `-pthread` в host-флагах для `test_modnet_concurrency.cpp`. |
| MN-7 | Low | M-P1 п.3/п.4 — file-static atomic storage в `modnet.cpp`; обёртки не трогают `ctx()`. |
| MN-8 | Low | M-P4 п.2 — `run` = процесс-уровневый proxy `_attach_runs`; object reload внутри run не разрезается (зафиксировано). |

---

# Стадия 3b — `retention` — план реализации (rev 1 — ожидает plan-review)

> Небольшая предрелизная стадия: авто-удаление старых дневных логов поверх суточной ротации §7. Авторитетный дизайн — `docs/design.md` §20 (rev 4, после design-review oracle R-1..R-8 folded; вопросы владельца §20.10 закрыты). Настоящий план не повторяет прозу дизайна, а разворачивает её в исполнимые фазы; семантика — по ссылкам на §20.

## Заголовок стадии (bindings)

- **Design:** `docs/design.md` §20 — **rev 4 (retention)** — revised after oracle design-review; **R-1..R-8 folded**; вопросы §20.10 закрыты. Индекс дизайна — `AGENTS.md`.
- **Design-review:** `oracle` — **PASS-WITH-REVISIONS; R-1..R-8 сложены (folded)** в §20 (§20.12). Design-гейт открыт.
- **Plan-review:** `momus` — **PLACEHOLDER (ожидает ревью)**; вердикт и disposition — раздел «Раздел ревью плана (momus) — стадия `retention`» ниже.
- **Язык документации:** русский; идентификаторы/код — английский.
- **Решения владельца (§20.10, закрыты):** `hldsmon_retention_days <= 0` ⇒ retention выключен (keep-all); prune **только** на `open()` + суточной ротации (не на `hldsmon_flush_log`); отдельного флага паузы нет — владелец правит cvar, эффективно на ближайшей ротации (R-1). Коммит — только по явной просьбе.
- **Верификация:** `make` (сборка 32-бит), `make test` (host C++ ядро + Python analyzer), `make verify-exports` (whitelist **не меняется** — всё `local: *`).
- **Коммит:** запрещён без явной просьбы владельца; один связный коммит на стадию (код + тесты + доки + запись журнала).
- **Интеграционная проверка:** тестовый ReHLDS-сервер (владелец) — боевой smoke prune на старте и на смене UTC-даты; вне CI.

**Соглашения (наследуются из базы):**
- C++17, `-m32 -fPIC -shared`, `-Wall -Wextra`, `-Werror` в CI; snake_case/lowerCamel согласованно.
- Host-ядро стадии (`src/retention.{h,cpp}`) не включает движковые заголовки и компилируется нативным g++ (цель `test-host-core`); `Makefile` `CORE_SRCS` **обязан** включить `src/retention.cpp` (**R-3**, ловушка M1 — иначе `make test` не слинкует ядро retention).
- `log_writer.{h,cpp}` остаётся host-testable (без движковых заголовков); prune — server thread, open/ротация, **не** hot path.

**Границы стадии (scope, §20.3):** удаляются **только** файлы точного шаблона `hldsmon-YYYY-MM-DD.jsonl` в `hldsmon_dir`; маркер краша и любые не-дневные имена — никогда; текущий файл — никогда; файлы за пределами каталога — не трогаются. **Вне стадии:** сжатие/архивация, квота по объёму, prune из `hldsmon_flush_log`/`hldsmon_status`, внешние/чужие файлы, Windows.

## Глобальные инварианты стадии `retention`

Базовые INV-1..INV-6 продолжают действовать без изменений; новых INV не вводится. Стадия опирается на:
- **INV-5** — формата JSONL не касаемся (новых типов нет).
- **INV-2** — применяется только к hot path; prune (open/ротация) вне hot path (§20.4 R-6).
- **INV-3** — prune только на server thread.

## Новые cvar'ы стадии `retention`

| Имя | Тип | Назначение | Default | Регистрация |
|---|---|---|---|---|
| `hldsmon_retention_days` | cvar | максимум дневных файлов логов (вкл. текущий); `<=0` — выкл | **3** | `bootstrap.cpp::register_cvars()` |

**Хелпер (R-5):** в коде есть только `cvar_f`; план добавляет `cvar_i(cvar_t*, int def)` (либо читает `(int)cvar_f(...)`) и поле `retention_days` в `CvarRefs` (`src/plugin.h`).

## Маппинг QAS-R1..R11 → метод проверки (design §20.9)

| QAS | Где | Метод |
|---|---|---|
| QAS-R1 (N=3 → удаляются T-3 и старше) | R-P0 | host-тест `test_retention.cpp` |
| QAS-R2 (текущий не удаляется) | R-P0/R-P1 | host-тест + FS-тест (`test_log_writer`) |
| QAS-R3 (N<=0 keep-all) | R-P0 | host-тест |
| QAS-R4 (не-дневные имена) | R-P0 | host-тест `parse_log_date` |
| QAS-R5 (N=1 после ротации) | R-P1 | host-тест (`test_log_writer`, temp dir) |
| QAS-R6 (open на недоступном/пустом каталоге) | R-P1 | host-тест |
| QAS-R7 (make/make test; whitelist) | R-P2 | CI/локально |
| QAS-R8 (out_cap мал ⇒ `false` ⇒ 0 unlink) | R-P0 | host-тест |
| QAS-R9 (симлинк ⇒ удаление ссылки) | R-P0 | host-тест (FS) |
| QAS-R10 (unlink открытого файла безопасен) | R-P0 | host-тест (open fd → prune → read) |
| QAS-R11 (маркер краша не удаляется) | R-P0 | host-тест |

---

## R-P0 — Host-testable ядро retention

**Цель:** чистая логика отбора, покрытая host-тестами, без движка и без ФС.

**Файлы:** `src/retention.{h,cpp}`, `tests/test_retention.cpp`, `Makefile` (`CORE_SRCS`).

**Границы / non-goals:** нет ФС, нет движковых заголовков, нет prune.

**Шаги:**
1. `src/retention.h`: `parse_log_date(name,y,m,d)` и `retention_select(names,count,y,m,d,N,out,out_cap,out_n)` — сигнатуры/контракт по §20.5.
2. `src/retention.cpp`: `parse_log_date` — точный матч шаблона + календарная проверка (R-4, високос); внутренний чистый `days_from_civil(y,m,d)`; `retention_select` — отбор по предикату `day_num(name) <= today_num - N`; `N<=0` ⇒ успех и `*out_n=0`; переполнение `out_cap` ⇒ `false`, ноль записей, `*out_n` не трогается (R-2). Без аллокаций.
3. `Makefile`: `CORE_SRCS += src/retention.cpp` (R-3).
4. `tests/test_retention.cpp`: QAS-R1..R4, R8..R11 — границы месяца/года, високос, `N=0/1/3`, будущая дата, неверные имена, переполнение буфера, симлинк, unlink-while-open (FS-часть), маркер краша.

**Инварианты:** INV-5 (не затрагивается); контракт §20.5.
**Acceptance:** `make test` линкует и прогоняет `test_retention` зелёным; поведение по QAS-R1..R4/R8..R11.
**Verify:** `make test`.
**Риски:** ошибки календарной арифметики — покрыть границы; забыть `CORE_SRCS` ⇒ тесты не линкуются (R-3) — убедиться, что `test_retention` реально собирается.

---

## R-P1 — LogWriter: prune + runtime-перечитывание cvar

**Цель:** реальное удаление на `open()`/ротации; значение cvar берётся в рантайме.

**Файлы:** `src/log_writer.{h,cpp}`, `src/bootstrap.cpp`, `src/plugin.h` (`CvarRefs`), `src/sampler.cpp`, `tests/test_log_writer.cpp`.

**Границы / non-goals:** без движковых заголовков в `log_writer`; prune — только server thread, open/ротация.

**Шаги:**
1. `log_writer.h`: `void set_retention_days(int days)` (хранение; `<=0` = off) + приватный `void prune()`; вызов `prune()` в конце успешного `open_day()`.
2. `log_writer.cpp`: `prune()` — `opendir(dir_)`/`readdir`, собрать basenames, вызвать `retention_select(...)`; при `true` — `unlink` каждого из `*out_n` (никогда не путь, равный `current_path()`); при `false` — **ничего**; `opendir`/`unlink` failure ⇒ предупреждение + `++error_count_`, без краха (§20.6).
3. `plugin.h`: поле `retention_days` в `CvarRefs`; `bootstrap.cpp`: `reg_cvar`+`engine_cvar` для `hldsmon_retention_days` (значение `"3"`, `FCVAR_ARCHIVE`); `cvar_i()` (R-5).
4. `bootstrap.cpp::open_log()`: `c.log->set_retention_days(retention_value())` перед `open()`.
5. **R-1:** `sampler.cpp` на 1 Гц server-tick перечитать cvar и вызвать `c.log->set_retention_days(...)` до `maybe_rotate()` (live-гейт по образцу `plugin_enabled()`); изменение cvar действует на ближайшей ротации.
6. `tests/test_log_writer.cpp`: расширить — QAS-R2/R5/R6 (temp dir; `N=1` после `maybe_rotate`; пустой/недоступный каталог; текущий файл не удалён).

**Инварианты:** INV-3, INV-5; §20.6.
**Acceptance:** `make && make test` зелёные; prune при `open()` и ротации; runtime-изменение `hldsmon_retention_days` применяется на ближайшей ротации; ошибки считаются, краха нет.
**Verify:** `make && make test`.
**Риски:** рассинхрон `set_retention_days` и ротации — перечитывать до `maybe_rotate()`; guard `current_path()` от удаления текущего файла.

---

## R-P2 — Документация и верификация

**Цель:** обновить README/конфиг, сквозная верификация стадии.

**Файлы:** `README.md` (cvar + абзац retention), `docs/design.md` §12 (строка cvar).

**Шаги:** добавить `hldsmon_retention_days` в список cvar'ов и краткий абзац про retention в README; синхронизировать §12 design; `make clean && make && make verify-exports && make test`; при доступе — боевой smoke prune на старте/смене UTC-даты (в журнал).

**Acceptance:** `make`/`make test`/`make verify-exports` зелёные; whitelist без изменений; README/§12 отражают cvar.
**Verify:** `make clean && make && make verify-exports && make test`.

---

## R-P3 — Запись стадии (journal)

**Файлы:** `docs/journal.md`. **Acceptance:** контекст; что сделано (R-P0..R-P2); решения и почему (`N<=0` disabled; prune на open/ротации; R-1 runtime-перечитывание; R-2 fail-safe контракт); затронутые артефакты; открытые темы (объём/квота, сжатие); следующие шаги; индексы `AGENTS.md` актуальны.

---

## R-P4 — Коммит (гейт)

Один связный коммит (код + тесты + доки + журнал) — **только по явной просьбе владельца**. Иначе: «готово к коммиту».

---

## Definition of Done (стадия `retention`)

- [ ] `make` собирает `.so` (32-бит, `-Wall -Wextra -Werror`) без предупреждений; `make verify-exports` — whitelist **без изменений**.
- [ ] `make test` зелёный: `test_retention` собирается (через `CORE_SRCS`) и проходит; `test_log_writer` (prune) зелёный; Python analyzer без изменений.
- [ ] `retention_select` — `bool`+`size_t* out_n`; `out_cap` мал ⇒ `false` ⇒ ноль `unlink` (QAS-R8, R-2).
- [ ] `parse_log_date` проверяет календарную дату с високосом (QAS-R4, R-4).
- [ ] Prune на `open()` и на ротации; текущий файл и не-дневные имена (маркер краша) — никогда (QAS-R2/R11).
- [ ] `N<=0` ⇒ keep-all; `N=1` ⇒ только текущий после ротации (QAS-R3/R5).
- [ ] `hldsmon_retention_days`(3) зарегистрирован и закэширован; изменение в рантайме применяется на ближайшей ротации (R-1).
- [ ] `CvarRefs::retention_days` + `cvar_i` (или `(int)cvar_f`) — R-5.
- [ ] README/§12 отражают cvar; `docs/journal.md` заполнен; индексы обновлены.
- [ ] Коммит — только при явной просьбе.

---

## Раздел ревью плана (momus) — стадия `retention`

**Ревьюер:** `momus` (независимый). **Основание:** `docs/design.md` §20 (rev 4, R-1..R-8 folded), базовый план (инварианты/Приложение P1), реальный репозиторий (`src/log_writer.{h,cpp}`, `src/bootstrap.cpp`, `src/plugin.h`, `src/sampler.cpp`, `Makefile`, `tests/test_log_writer.cpp`). **Plan rev:** 1.

**Вердикт: PASS-WITH-REVISIONS (гейт имплементации ОТКРЫТ).** План исполним и достоверно разворачивает §20 в фазы R-P0..R-P4: критичные R-1 (рантайм-перечитывание cvar) и R-2 (`bool retention_select(..., size_t* out_n)`) замаплены на конкретные шаги, ловушка M1 (`CORE_SRCS += src/retention.cpp` + `tests/test_retention.cpp`) закрыта, расширение `tests/test_log_writer.cpp` задано, QAS-R1..R11 имеют непустой конкретный метод, команды верификации (`make`/`make test`/`make verify-exports`) реальны. Замечания ниже — Low/Med-локальные уточнения; блокеров нет, перепроектирования §20 не требуется.

**Таблица замечаний (ID | Severity | Замечание (расположение) | Рекомендуемый disposition):**

| ID | Severity | Замечание (расположение) | Рекомендуемый disposition |
|---|---|---|---|
| RET-1 | Med | **Трассировка QAS-R9/R10 на R-P0 при границе «нет prune/ФС».** QAS-R9 (симлинк ⇒ удаление ссылки) и QAS-R10 (unlink открытого файла) в маппинге отнесены к R-P0 (`R-P0`), а граница R-P0 — «нет ФС, нет prune»; удаляющую семантику даёт `LogWriter::prune()` (R-P1 п.1/п.2). Неоднозначно, где и как эти FS-сценарии проверяются (реальный `unlink` через `prune` vs чистая POSIX-проверка). | Выбрать явно: либо отнести QAS-R9/R10-проверки к R-P1 (через `prune`), либо пометить их в R-P0 как POSIX-семантику (`unlink` ссылки / открытого fd) без `prune`. |
| RET-2 | Low | **Источник «сегодня» для `prune()` и `retention_value()`.** R-P0 п.2 оставляет `prune()` без параметров и не говорит, откуда берётся текущая UTC-дата (y,m,d); R-P1 п.4 вызывает неопределённый `retention_value()`. | Зафиксировать: `prune()` берёт дату из `year_/month_/day_` (заполняются `open_day`); `retention_value()` ≡ `cvar_i(c.cvars.retention_days, 3)` (или `(int)cvar_f(...)`). |
| RET-3 | Low | **Точка вставки R-1 в sampler.** R-P1 п.5 «1 Гц server-tick … до `maybe_rotate()`» — в коде `maybe_rotate()` вызывается внутри `do_server_tick()` (`src/sampler.cpp:318`); место вставки не названо. | Указать `do_server_tick()` (вызов `set_retention_days` непосредственно перед `maybe_rotate`) — тогда R-1 однозначно реализуем. |
| RET-4 | Low | **Выбор хелпера R-5.** «Соглашения» и R-P1 п.3 допускают «либо `cvar_i()`, либо `(int)cvar_f`» — выбор оставлен исполнителю. | Выбрать одно (рекомендуется inline `(int)cvar_f(c.cvars.retention_days, 3)`; новый хелпер не вводить). Тривиально, не блокирует. |

**Что проверено и корректно (не переусердствовать):**
- **Decision-completeness R-1/R-2.** R-1 (runtime cvar re-read) замаплен на R-P1 п.5 (server-tick → `set_retention_days` до `maybe_rotate`) и R-P1 п.4 (старт); R-2 закреплён и в сигнатуре (`bool retention_select(..., size_t* out_n)`), и в поведении (переполнение `out_cap` ⇒ `false`, ноль записей, `*out_n` не трогается) — совпадает с §20.5.
- **Ловушка M1 закрыта.** R-P0 п.3 добавляет `src/retention.cpp` в `CORE_SRCS` (реально — `$(wildcard …)` в `Makefile:38`); R-P0 Файлы содержит `tests/test_retention.cpp`, а `TEST_SRCS := $(wildcard tests/test_*.cpp)` подхватит его автоматически.
- **Расширение `tests/test_log_writer.cpp` задано** (R-P1 п.6: temp dir, `N=1` после `maybe_rotate`, пустой/недоступный каталог, текущий файл не удалён) — согласуется с существующими хелперами `mkdtemp`/`day_name`/`file_exists` в тесте.
- **cvar H2/R-5.** R-P1 п.3: `reg_cvar`+`engine_cvar` для `hldsmon_retention_days` (value `"3"`, `FCVAR_ARCHIVE`), поле `retention_days` в `CvarRefs`, `cvar_i`/`(int)cvar_f`; соответствует `register_cvars()` в `bootstrap.cpp:101`.
- **Триггеры/семантика.** prune в конце успешного `open_day()` покрывает обе точки (`open()`→`open_day`; `maybe_rotate`→`open_day`, `log_writer.cpp:154/215`); `N<=0` ⇒ keep-all; guard `current_path()`; не-дневные имена (маркер краша) — никогда; ошибки `opendir`/`unlink` ⇒ `++error_count_`, без краха и без выключения записи (§20.6). Инварианты базы не меняются (новых INV нет); INV-2 ограничен hot path (R-6).
- **Верификация/DoD.** `make`, `make test`, `make verify-exports`, `test-host-core` реально существуют в `Makefile`; DoD стадии чек-листный и проверяемый; whitelist экспортов не меняется (новых экспортов нет).
- **QAS-R1..R11** — все одиннадцать имеют непустой конкретный метод (host-тест / FS-тест / CI), формулировок «verify it works» нет.

**Гейт имплементации: ОТКРЫТ.** High/Critical-замечаний нет; design-critical R-1 и R-2 замаплены на конкретные, недвусмысленные шаги. Замечания RET-1..RET-4 — локальные уточнения (Low/Med); допустимо сложить по ходу реализации. Перепроектирования §20 не требуется; повторное ре-ревью дизайна не нужно.

**Disposition — folded в rev 1 (retention); гейт имплементации ОТКРЫТ:**

| ID | Severity | Куда сложено |
|---|---|---|
| RET-1 | Med | (ожидает складывания) — уточнить трассировку QAS-R9/R10: R-P1 через `prune` **либо** явная POSIX-проверка в R-P0. |
| RET-2 | Low | (ожидает) — источник даты для `prune()` (`year_/month_/day_`) и определение `retention_value()`. |
| RET-3 | Low | (ожидает) — точка вставки `set_retention_days` = `do_server_tick()` перед `maybe_rotate()`. |
| RET-4 | Low | (ожидает) — выбрать `(int)cvar_f` / `cvar_i`. |

---

# Стадия 4 — `ping-fix` — план реализации (rev 1 — сокращённый багфикс-трек)

> Корректирующий багфикс единиц `latency` (секунды→мс): per-client `ping` и `drop.neighbors.avg_ping` сейчас всегда `0`. Авторитетный дизайн — `docs/design.md` §21 (rev 5, после oracle design-review PG-1..PG-7 folded). **Трек сокращённый** (design §21.12): независимое plan-ревью `momus` для этой стадии **не проводится** — изменений ≈3 строки + один host-тест, без развилок/схемы/ABI.

## Заголовок стадии (bindings)

- **Design:** `docs/design.md` §21 — **rev 5 (ping-fix)**; oracle design-review — **PASS-WITH-REVISIONS; PG-1..PG-7 сложены** (§21.12). Индекс дизайна — `AGENTS.md`.
- **Design-review:** `oracle` — PASS-WITH-REVISIONS (закрыто).
- **Plan-review:** **не требуется** (сокращённый багфикс-трек; обоснование — design §21.12).
- **Язык документации:** русский; идентификаторы/код — английский.
- **Решения владельца (§21.10):** fix A (конвертер в месте чтения); `avg_ping` чинится тем же конвертером; `ping=0` трактуется как «не измерено»; NT-1 (устаревший вендоренный интерфейс) — отдельной стадией. Коммит — только по явной просьбе.
- **Верификация:** `make` (сборка 32-бит), `make test` (host C++ ядро + Python analyzer), `make verify-exports` (whitelist **не меняется**).
- **Коммит:** запрещён без явной просьбы владельца; один связный коммит на стадию (код + тест + доки + журнал).
- **Интеграционная проверка:** тестовый ReHLDS-сервер (владелец) — smoke: реальный клиент даёт `ping>0` в `client` и `drop.neighbors.avg_ping`; вне CI.

**Границы стадии:** только конверсия единиц `latency`; схема P1/ключи/типы/экспорты/cvar — **без изменений**. **Вне стадии (non-goals):** поле-различитель «бот/не измерено»; ре-вендоринг заголовков (NT-1); смена источника `packet_loss`; переход на `pfnGetPlayerStats`/`GetLatency`.

## Глобальные инварианты стадии `ping-fix`

Базовые INV-1..INV-6 действуют без изменений; новых INV не вводится.
- **INV-2** — конвертер чисто вычислительный (стек, без аллокаций).
- **INV-4** — пробу **не меняем**; корректность фикса наследует покрытие раскладки `client_t` (§21.5, PG-3).
- **INV-5** — формат JSONL не меняется.

## Маппинг QAS-P1..P5 → метод проверки (design §21.9)

| QAS | Где | Метод |
|---|---|---|
| QAS-P1 (границы конвертера: NaN/0/отриц/clamp) | PF-P0 | host-тест `tests/test_ping.cpp` |
| QAS-P2 (`sampler` вызывает конвертер) | PF-P1 | review-only (логика покрыта QAS-P1) |
| QAS-P3 (`neighbors.avg_ping` в мс) | PF-P1 | review-only (логика покрыта QAS-P1) |
| QAS-P4 (контракт/CI: ключи, whitelist, `make`/`make test`) | PF-P2 | CI/локально |
| QAS-P5 (бот/first-2s → `ping=0`) | — | документировано (§21.8); не тестируется как «должно быть >0» |

---

## PF-P0 — Host-testable конвертер + тест

**Цель:** чистый конвертер секунды→мс, покрытый host-тестом.

**Файлы:** `src/metrics.h` (inline-хелпер), `tests/test_ping.cpp`.

**Границы / non-goals:** без движковых заголовков; без изменения полей/размеров POD.

**Шаги:**
1. `src/metrics.h`: добавить `inline uint16_t ping_ms_from_latency(float latency_s)` — код и numeric-края по §21.3 (PG-1: `if (!(ms > 0.0))` для NaN; PG-2: clamp по округлённому `r = ms + 0.5`).
2. `tests/test_ping.cpp` (включает только `metrics.h`): QAS-P1 — `0.040→40`; `0.0`/отрицательное/`NaN→0`; `65.535→65535` и `70.0→65535`; краевой `65.5347→65535`; `0.0005→1`.
3. **Makefile не трогаем (PG-4):** `TEST_SRCS := $(wildcard tests/test_*.cpp)` подхватывает файл; новый `.cpp` ядра не нужен (хелпер inline).

**Инварианты:** INV-2 (без аллокаций), INV-4 (структуры не меняются).
**Acceptance:** `make test` собирает и прогоняет `test_ping` зелёным.
**Verify:** `make test`.
**Риски:** numeric-края (NaN/clamp) — покрыты тестом; убедиться, что тест реально собран (не тихо пропущен).

---

## PF-P1 — Замена кастов в точках чтения

**Цель:** обе точки чтения `latency` конвертируют секунды→мс.

**Файлы:** `src/sampler.cpp`, `src/hooks.cpp`.

**Шаги:**
1. `src/sampler.cpp:101` → `cs.ping = ping_ms_from_latency(ct->latency);`
2. `src/hooks.cpp` (сбор `neighbors`): `sum_ping += static_cast<double>(ping_ms_from_latency(o->latency));` (сумма в мс; `rec.neighbors.avg_ping = sum_ping / count` — без иных правок).
3. `packet_loss`/`loss`/`dloss` — **не трогать** (уже корректны).

**Инварианты:** INV-1 (только чтение), INV-2, INV-3 (server thread).
**Acceptance:** сборка `-Wall -Wextra -Werror` без предупреждений; `ping`/`avg_ping` — мс.
**Verify:** `make`.
**Риски:** случайно задеть соседние поля — покрыть диффом/review.

---

## PF-P2 — Верификация и документация

**Файлы:** (опц.) `README.md` — уточнение, что `ping` в мс.

**Шаги:** `make clean && make && make verify-exports && make test`; убедиться, что whitelist экспортов без изменений и Python-тесты зелёные; при доступе — боевой smoke (клиент с `ping>0` в `client` и `neighbors.avg_ping`).

**Acceptance:** `make`/`make test`/`make verify-exports` зелёные; whitelist без изменений.
**Verify:** `make clean && make && make verify-exports && make test`.

---

## PF-P3 — Запись стадии (journal)

**Файлы:** `docs/journal.md`. **Acceptance:** контекст (жалоба владельца «ping=0»); что сделано (PF-P0..PF-P2); решения и почему (fix A; побочный `avg_ping`; `ping=0`=`не измерено`; NT-1 отдельно); затронутые артефакты; открытые нити (NT-1: ре-вендоринг ReHLDS-интерфейсов); следующие шаги (Стадия 5 = F2 и т.д.); индексы `AGENTS.md` актуальны.

---

## PF-P4 — Коммит (гейт)

Один связный коммит (код + тест + доки + журнал) — **только по явной просьбе владельца**. Иначе: «готово к коммиту».

---

## Definition of Done (стадия `ping-fix`)

- [ ] `ping_ms_from_latency` в `metrics.h`; NaN/отрицательное→0; clamp по округлённому (PG-1/PG-2).
- [ ] `sampler.cpp`/`hooks.cpp` используют конвертер (не сырой каст `static_cast<uint16_t>(ct->latency)`).
- [ ] `tests/test_ping.cpp` собран и зелёный (подхват через `wildcard`).
- [ ] `make`/`make test`/`make verify-exports` зелёные; схема P1 и whitelist экспортов — **без изменений**.
- [ ] Бот/first-2s/высокий разброс → `ping=0` (ожидаемо, документировано §21.8) — не регресс.
- [ ] `docs/journal.md` заполнен; индексы обновлены.
- [ ] Коммит — только при явной просьбе.

---

## Раздел ревью плана (momus) — стадия `ping-fix`

**Не проводится** (сокращённый багфикс-трек; обоснование — oracle design-review, design §21.12: нет развилок/схемы/ABI). Гейт имплементации открыт на основании пройденного дизайн-ревью и закрытия PG-1..PG-7.

---

# Стадия 5 — `config-context` — план реализации (rev 2 — plan-review пройден; CM-1..CM-5 folded)

> Контекст запуска и rate-снимок: новый тип JSONL `config` (boot + при изменении) и вложенный снимок конфигурации в `drop`. Авторитетный дизайн — `docs/design.md` §22 (rev 6, после oracle design-review CFG-1..CFG-8 folded). План разворачивает §22 в фазы CFG-P0..CFG-P6; семантика — по ссылкам на §22.

## Заголовок стадии (bindings)

- **Design:** `docs/design.md` §22 — **rev 6 (config-context)**; design-review `oracle` — **PASS-WITH-REVISIONS; CFG-1..CFG-8 folded** (§22.11). Индекс — `AGENTS.md`.
- **Plan-review:** `momus` — **PASS-WITH-REVISIONS; CM-1..CM-5 сложены (folded)** — раздел ниже.
- **Язык:** русский; идентификаторы/код — английский.
- **Решения владельца:** набор полей — дефолт §22.3; снимок в `drop` — да; список плагинов — отложен (§22.7). Коммит — только по явной просьбе.
- **Верификация:** `make`, `make test` (host C++ + Python analyzer), `make verify-exports` (whitelist **без изменений**).
- **Интеграционная проверка:** тестовый ReHLDS-сервер — smoke: boot `config`-запись, изменение `sv_maxrate` → новая `config`-запись ≤1 с, `drop.config` заполнен; вне CI.

**Границы стадии:** новый тип `config` + `drop.config` + анализатор/UI-поддержка + эмиттер `null`. **Вне стадии:** список плагинов; дамп всех cvar; история каждого изменения; Windows/внешний агент.

## Глобальные инварианты стадии `config-context`

Базовые INV-1..INV-6 без изменений; новых INV не вводится.
- **INV-1** — только чтение cvar (пассивность).
- **INV-2** — поллинг/diff/сравнение на server-tick, фиксированные POD, без аллокаций; снимок `memset` перед заполнением (CFG-5).
- **INV-3** — только server thread.
- **INV-4** — не затрагивается.
- **INV-5** — `config` содержит `type`+`t`; отсутствующие cvar — литерал `null`.

## Новые cvar'ы стадии

**Нет** (набор отслеживаемых cvar фиксирован дизайном §22.3).

## Маппинг QAS-C1..C5 → метод проверки (design §22.8)

| QAS | Где | Метод |
|---|---|---|
| QAS-C1 (boot `config`; отсутствующий cvar → `null`) | CFG-P1/P2 | host-тест сериализатора + smoke |
| QAS-C2 (изменение cvar → новая запись ≤1 с) | CFG-P0/P2 | host-тест diff-хелпера + smoke |
| QAS-C3 (`drop.config` заполнен) | CFG-P1/P2 | unit-тест сборки drop (`test_serialization`) |
| QAS-C4 (без аллокаций; bounded размер) | CFG-P0 | unit-тест размеров + тест «0 аллокаций» |
| QAS-C5 (P1 расширен; whitelist без изменений; make/make test) | CFG-P4 | CI/локально |

---

## CFG-P0 — Host-testable ядро: `ConfigRecord`/`ConfigSnapshot` + diff

**Цель:** POD-структуры и чистая логика «изменилось ли» на хосте.

**Файлы:** `src/metrics.h`, `tests/test_config.cpp` (новый).

**Шаги:**
1. `metrics.h`: `ConfigSnapshot` (числовой rate/timeout-набор + tickrate; `present`-битмаска для отсутствующих cvar, CFG-2); `ConfigRecord` (`type`+`t`+версии+map+maxclients+`ConfigSnapshot`). Обновить `static_assert` размеров.
2. `metrics.h` (inline, host-testable): `bool config_changed(const ConfigSnapshot& a, const ConfigSnapshot& b)` — сравнение по POD (после `memset`), без аллокаций.
3. `tests/test_config.cpp`: равенство/неравенство снимков, `present`-битмаска, размеры; расширить `tests/test_no_alloc.cpp` — `config_changed` без аллокаций (**CM-3**).

**Инварианты:** INV-2, INV-5.
**Acceptance:** `make test` собирает/проходит `test_config`; размеры bounded.
**Verify:** `make test`.

---

## CFG-P1 — Serialization: `write_config_json` + эмиттер `null` + `drop.config`

**Цель:** JSONL-эмиссия нового типа и вложенного снимка; поддержка `null`.

**Файлы:** `src/serialization.{h,cpp}`, `src/events.{h,cpp}`, `src/metrics.h` (`DropRecord` +`ConfigSnapshot`), `tests/test_serialization.cpp`.

**Шаги:**
1. `serialization.cpp`: **явный эмиттер `null`** для отсутствующего cvar (CFG-2); `write_config_json`; расширить `write_drop_json` вложенным `config` (общий подсет с типом `config`, CFG-8).
2. `events.{h,cpp}`: `write_config`.
3. `metrics.h`: `DropRecord` +`ConfigSnapshot config`; обновить `static_assert(sizeof(DropRecord))`.
4. `tests/test_serialization.cpp`: `config` (полный/с `null`); `drop` содержит `config`; расширить `tests/test_no_alloc.cpp` — `write_config_json` без аллокаций (**CM-3**).

**Инварианты:** INV-5; ключи/типы по P1.
**Acceptance:** host-тесты зелёные; валидный JSON (в т.ч. `null`).
**Verify:** `make test`.

---

## CFG-P2 — Bootstrap/Sampler/Hooks: кэш cvar, poll/diff, boot, drop-снимок

**Цель:** сборка снимков в движке.

**Файлы:** `src/plugin.h` (`CvarRefs` + `last_config` + флаг валидности), `src/bootstrap.cpp`, `src/sampler.cpp`, `src/hooks.cpp`.

**Шаги:**
1. `bootstrap.cpp`: закэшировать `cvar_t*` отслеживаемых cvar (`g_engfuncs.pfnCVarGetPointer`) в `CvarRefs` (H2-паттерн); отсутствующий ⇒ nullptr.
2. `sampler.cpp::do_server_tick` (**CM-5**): заполнить `ConfigSnapshot` (с `memset`, CFG-5) из кэша + версии/map/maxclients; сравнить с `last_config` (diff-хелпер CFG-P0); при отличии — **или на первом тике с невалидным `last_config` (baseline; инициатор boot-эмиссии — `sampler`, а не `bootstrap`)** — эмитировать `config` и обновить `last_config`. Всё под `plugin_enabled()`.
3. `hooks.cpp::build_drop_record` (**CM-1**): заполнить `rec.config` живым снимком; **сохранить** `rec.netchan.sv_timeout`, заполняя его из **кэша** `cvar_t*` (не `pfnCVarGetPointer` по имени) — совместимость P1; авторитет — `drop.config` (CFG-3).

**Инварианты:** INV-1/2/3.
**Acceptance:** `make` (32-бит, `-Werror`); boot + изменение дают `config`; drop содержит `config`.
**Verify:** `make`.
**Риски:** строковый lookup на пути дропа — использовать кэш `cvar_t*`, не `pfnCVarGetPointer` по имени (CFG §22.3 п.3).

---

## CFG-P3 — Анализатор/UI: `KNOWN_TYPES` + фикстура + панель

**Цель:** `config` видим анализатором и UI (CFG-1).

**Файлы:** `tools/analyzer/hldsmon.py`, `tools/analyzer/webui.py`, `tools/analyzer/webui/index.html`, `tools/analyzer/tests/fixtures/*.jsonl`, `tools/analyzer/tests/test_hldsmon.py`, `tools/analyzer/tests/test_webui.py`.

**Шаги:**
1. `hldsmon.py`: `KNOWN_TYPES += "config"`; строка в `summary`/`digest`; толерантность `null` (уже есть в `_num`).
2. Фикстуры: новый `config` + расширить `types_ok.jsonl`; тесты на наличие `config` в сводке.
3. UI: маркер/панель конфигурации; `webui.py` pass-through при необходимости.

**Acceptance:** Python-тесты зелёные; `config` учитывается в `counts_by_type`/`summary`.
**Verify:** `make test` (python-часть).

---

## CFG-P4 — Верификация и документация

**Файлы:** `README.md` (описание типа `config`/`drop.config`), `docs/design.md` (при необходимости).

**Шаги:** `make clean && make && make verify-exports && make test`; whitelist без изменений; README — задокументировать тип `config`/`drop.config` **и схлопывание/потерю изменений <1 с** (**CFG-6**, **CM-4**); при доступе — smoke.

**Acceptance:** три команды зелёные; whitelist без изменений.

---

## CFG-P5 — Запись стадии (journal)
**Файлы:** `docs/journal.md`. Контекст/сделано/решения (новый тип; `present`+`null`; дубль `sv_timeout`; гейт baseline; diff; **CFG-6** схлопывание <1 с; **CFG-7** результаты сверки имён cvar/build-геттера — CM-4)/открытые нити/индексы.

## CFG-P6 — Коммит (гейт)
Один связный коммит — **только по явной просьбе владельца**.

---

## Definition of Done (стадия `config-context`)

- [ ] `ConfigRecord`/`ConfigSnapshot` + `config_changed` (host-testable, `memset`, CFG-5).
- [ ] `write_config_json` + эмиттер `null` (CFG-2); `drop.config` (общий подсет, CFG-8); `static_assert(DropRecord)` обновлён.
- [ ] Boot `config` + при изменении (≤1 с); baseline при включении mid-run (CFG-4); под `plugin_enabled()`.
- [ ] `KNOWN_TYPES += "config"` + фикстура + `types_ok.jsonl` + `summary`/`digest` (CFG-1); UI-панель.
- [ ] P1 расширен (`config`, `drop.config`, правило `null`); whitelist экспортов без изменений.
- [ ] `make`/`make test`/`make verify-exports` зелёные; `test_config` и расширенный `test_serialization` зелёные.
- [ ] `docs/journal.md` заполнен; индексы обновлены. Коммит — только при явной просьбе.

---

## Приложение P1 — расширение: тип `config` и `drop.config` (стадия `config-context`)

```json
{"type":"config","t":1755000000.0,"rehlds_major":3,"rehlds_minor":14,
 "map":"de_dust2","maxclients":32,"sys_ticrate":100.0,"host_framerate":0.0,
 "sv_timeout":65.0,"sv_maxrate":0.0,"sv_minrate":0.0,
 "sv_maxupdaterate":101.0,"sv_minupdaterate":10.0,
 "sv_maxcmdrate":101.0,"sv_mincmdrate":10.0}
```
- Отсутствующий cvar ⇒ значение `null` (эмиттер `null`, CFG-2).
- `drop` получает вложенный `config` = **числовой подсет** (`sys_ticrate`, `host_framerate`, `sv_timeout`, `sv_maxrate`/`sv_minrate`, `sv_maxupdaterate`/`sv_minupdaterate`, `sv_maxcmdrate`/`sv_mincmdrate`) — тот же суб-эмиттер, что у `config`, без `type`/`t`/`rehlds_*`/`map`/`maxclients` (CFG-8, **CM-2**).
- `drop.netchan.sv_timeout` сохраняется для совместимости P1 (заполняется из кэша `cvar_t*`, не by-name lookup); авторитет по `sv_timeout` — у `drop.config` (CFG-3, **CM-1**).

---

## Раздел ревью плана (momus) — стадия `config-context`

**Ревьюер:** `momus` (независимый). **Plan rev:** 1 → folded в rev 2. **Вердикт: PASS-WITH-REVISIONS; гейт имплементации ОТКРЫТ** (Critical/High нет).

План исполнимо развёрнут из §22: фазы CFG-P0..P6 покрывают компоненты §22.6, все ссылки на файлы реальны, QAS-C1..C5 имеют конкретный метод, команды верификации существуют. Замечания CM-1..CM-5 (Med/Low) сложены:

| ID | Severity | Учтено |
|---|---|---|
| CM-1 | Med | CFG-P2 п.3: явно сохранён `rec.netchan.sv_timeout` (заполняется из кэша `cvar_t*`), авторитет — `drop.config` (CFG-3); исключена регрессия P1. |
| CM-2 | Med | Приложение P1: hedge «уточняется при реализации» заменён решённым составом `drop.config` (числовой подсет, CFG-8). |
| CM-3 | Low | CFG-P0/P1: добавлен шаг расширения `tests/test_no_alloc.cpp` (`config_changed`/`write_config_json`). |
| CM-4 | Low | CFG-P4/P5: CFG-6 (схлопывание <1 с) — в README; CFG-7 (сверка имён cvar/build-геттера) — в журнал. |
| CM-5 | Low | CFG-P2 п.2: инициатор boot-эмиссии — `sampler` (baseline), формулировка согласована с §22.6 (design-level). |

**Fine (не менять):** новый тип `config`; поллинг 1 Гц + diff с кэшем `cvar_t*`; `present`-битмаска + эмиттер `null`; расширение `DropRecord`; общий суб-эмиттер; `Makefile`/`CORE_SRCS`/`exports.whitelist` без изменений; `KNOWN_TYPES += "config"` обязателен; `null` уже переживается анализатором (`_num`).

---

# Стадия 6 — `host-metrics` — план реализации (rev 1 — ожидает plan-review)

> Best-effort хост/ядро-телеметрия: новый тип JSONL `host` (1 Гц), cvar `hldsmon_host` (**default 0/OFF**), engine-free модуль `src/host_metrics.{h,cpp}` (парсеры + POSIX-I/O + чистая детекция окружения). Авторитетный дизайн — `docs/design.md` §23 (rev 7, oracle PASS-WITH-REVISIONS; H1/H2/M1..M5 folded). **Зависимость:** использует эмиттер `null` из Стадии 5 (`config`) — реализовывать после Стадии 5 или включить эмиттер в эту стадию (design §23 Q4/L5).

## Заголовок стадии (bindings)

- **Design:** `docs/design.md` §23 — **rev 7**; design-review `oracle` — **PASS-WITH-REVISIONS; H1/H2/M1-M5 folded** (§23.11); повторное ревью — PASS.
- **Plan-review:** `momus` — раздел ниже (ожидает).
- **Язык:** русский; код — английский.
- **Решения владельца:** default `hldsmon_host = 0` (opt-in; §23.9 Q1). Коммит — только по явной просьбе.
- **Верификация:** `make`, `make test` (host C++ + Python), `make verify-exports` (whitelist **без изменений**).
- **Интеграционная проверка:** тестовый ReHLDS-сервер — smoke: `hldsmon_host 1` даёт записи `host` с флагами достоверности; на shared-netns — caveated; вне CI.

**Границы стадии:** чтение `/proc` + `statvfs` best-effort, тип `host`, детекция окружения, Δ-семантика, маска достоверности. **Вне стадии:** внешний агент; eBPF; `loadavg` в контейнерах; активные действия; `modnet`-атрибуция; Windows.

## Глобальные инварианты стадии `host-metrics`

- **INV-1** — только чтение (пассивность); **INV-2** — фикс. буфер, без аллокаций; **INV-3** — server thread; **INV-5** — валидный JSON (`type`+`t`), недоступное → `null`.

## Новые cvar'ы стадии

| Имя | Тип | Назначение | Default | Регистрация |
|---|---|---|---|---|
| `hldsmon_host` | cvar | мастер-выключатель host-телеметрии | **0** (OFF) | `bootstrap.cpp::register_cvars()` + `CvarRefs` |

## Маппинг QAS-H1..H8 → метод (design §23.8)

| QAS | Где | Метод |
|---|---|---|
| QAS-H1 (parse-by-name устойчив к отсутствующим/новым полям) | HM-P0 | host-тест `test_host_metrics` |
| QAS-H2 (shared netns ⇒ caveated) | HM-P0/P2 | host-тест детектора |
| QAS-H3 (нет файла ⇒ флаг, не 0-здоровье) | HM-P0 | host-тест |
| QAS-H4 (без аллокаций; недоступное → `null`) | HM-P0/P1 | тест «0 аллокаций» + сериализатор |
| QAS-H5 (`hldsmon_host=0` ⇒ ноль записей/накладных) | HM-P2 | host/review |
| QAS-H6 (`make`/`make test`/`verify-exports`; whitelist) | HM-P4 | CI/локально |
| QAS-H7 (первый сэмпл ⇒ Δ `null`; `steal` — %) | HM-P0 | host-тест |
| QAS-H8 (`EACCES`/неопределённость ⇒ caveated, без краха) | HM-P0 | host-тест детектора |

---

## HM-P0 — Engine-free ядро: парсеры, детекция, Δ

**Цель:** чистая host-testable логика (без движковых заголовков) + POSIX-I/O.

**Файлы:** `src/host_metrics.{h,cpp}` (новый), `tests/test_host_metrics.cpp`, `Makefile` (`CORE_SRCS += src/host_metrics.cpp` — ловушка M1).

**Шаги:**
1. `host_metrics.h`: структуры снимка + маска достоверности; чистые функции `parse_snmp_udp(buf,...)`, `parse_net_dev(buf,...)`, `parse_stat(buf,...)`, `parse_self_io(buf,...)` (parse-by-name, неизвестное = 0/отсутствует); `detect_env(inputs...)` (чистая, по cgroup/lxcfs/dockerenv/inode-флагам); enum-константы **битов маски** (HM-3: `present` — по-полям, `caveat` — по-источникам) ; `delta_u64(prev,cur)` — **wrap/reset-safe (HM-1): `cur<prev` ⇒ re-baseline (значение `null`), а не unsigned-гигант**; bounded `read_file(path, buf, cap)` (POSIX `open`/`read`/`close`, drain до EOF, `EINTR`), `statvfs_bavail(dir)`.
2. `Makefile`: `CORE_SRCS += src/host_metrics.cpp`.
3. `tests/test_host_metrics.cpp`: QAS-H1/H2/H3/H7/H8 (parse-by-name без `MemErrors`; детектор: shared/isolated/EACCES; отсутствие файла; первый сэмпл Δ=null; `steal` %); **HM-1** (reset/wrap); и расширить `tests/test_no_alloc.cpp` для QAS-H4 /**HM-4**.

**Инварианты:** INV-2/INV-5.
**Acceptance:** `make test` собирает/проходит `test_host_metrics`.
**Verify:** `make test`.
**Риски:** забыть `CORE_SRCS` (M1); парсинг по позиции — запрещён.

---

## HM-P1 — `HostRecord` + сериализация

**Файлы:** `src/metrics.h` (`HostRecord` + маска), `src/serialization.{h,cpp}` (`write_host_json`; недоступное → `null`), `src/events.{h,cpp}` (`write_host`), `tests/test_serialization.cpp`.

**Шаги:** POD `HostRecord`; `write_host_json` через существующий `null`-эмиттер (Стадия 5); `write_host`; тесты (полный/частичный/`null`).

**Acceptance:** host-тесты зелёные; валидный JSON.
**Verify:** `make test`.

---

## HM-P2 — Bootstrap/Sampler: cvar, детекция, 1 Гц сбор/эмиссия

**Файлы:** `src/plugin.h` (`CvarRefs::host`, состояние снимка), `src/bootstrap.cpp` (cvar), `src/sampler.{h,cpp}`.

**Шаги:** регистрация `hldsmon_host` (0); детекция окружения при старте (хранить маску); на 1 Гц server-tick при `hldsmon_host>0.5 && plugin_enabled()` — сбор, Δ, эмиссия `host`; OFF ⇒ ноль работы.

**Acceptance:** `make` (32-бит, `-Werror`); при OFF — ни одной `host`-записи.
**Verify:** `make`.

---

## HM-P3 — Анализатор/UI

**Файлы:** `tools/analyzer/hldsmon.py`, `webui.py`, `webui/index.html`, fixtures, tests.

**Шаги:** `KNOWN_TYPES += "host"`; ряды/панель; фикстура `host` (вкл. caveated/`null`).

**Acceptance:** Python-тесты зелёные.
**Verify:** `make test`.

---

## HM-P4 — Верификация и документация
`README.md` (тип `host`, cvar, caveats netns/драйвер); `make clean && make && make verify-exports && make test`.

## HM-P5 — Запись стадии (journal)
Контекст/сделано/решения (best-effort; default OFF; детекция окружения; Δ; net/dev caveated)/открытые нити (реальные caveats на хостинге владельца)/индексы.

## HM-P6 — Коммит (гейт)
Только по явной просьбе владельца.

## Definition of Done (стадия `host-metrics`)

- [ ] `src/host_metrics.{h,cpp}` engine-free; `CORE_SRCS += src/host_metrics.cpp`; `test_host_metrics` зелёный.
- [ ] Parse-by-name; отсутствие файла/`EACCES` ⇒ флаг/`null`, не 0-здоровье (QAS-H1/H3/H8).
- [ ] Δ-семантика: первый сэмпл `null`; `steal` — % (QAS-H7).
- [ ] Детекция окружения (контейнеризация; несостоятельное `self==PID1` не используется); shared ⇒ caveated (QAS-H2).
- [ ] `HostRecord` + `write_host_json` (общий `null`-эмиттер); `KNOWN_TYPES += "host"` + фикстуры (QAS-H4).
- [ ] Cvar `hldsmon_host` (0); OFF ⇒ ноль записей (QAS-H5).
- [ ] `make`/`make test`/`make verify-exports` зелёные; whitelist без изменений (QAS-H6).
- [ ] `docs/journal.md` заполнен; индексы обновлены. Коммит — только при явной просьбе.

## Приложение P1 — расширение: тип `host` (стадия `host-metrics`)

```json
{"type":"host","t":1755000000.0,
 "udp_rcvbuf_errors":0,"udp_in_errors":0,"udp_in_csum_errors":0,
 "net_rx_dropped":0,"net_rx_missed":0,"net_rx_overruns":0,
 "steal_pct":0.0,"io_read_bytes":0,"io_write_bytes":0,"disk_bavail_mb":12345,
 "present_mask":1023,"caveat_mask":3}
```
- Все счётчики — **Δ за интервал** (первый сэмпл → `null`). **Недоступное ⇒ `null` + бит `present` снят; недостоверное (caveated) ⇒ значение ПРИСУТСТВУЕТ + бит `caveat`** (design M4, «caveated ≠ null»; **HM-2**). Биты маски: `present` — по-полям, `caveat` — по-источникам; константы — в `host_metrics.h` (**HM-3**). `udp_in_csum_errors` — добавление сверх §23.2 (в духе parse-by-name; **HM-6**).

## Раздел ревью плана (momus) — стадия `host-metrics`

**Ревьюер:** `momus` (независимый). **Plan rev:** 1 → folded в rev 2. **Вердикт: PASS-WITH-REVISIONS; гейт имплементации ОТКРЫТ** (Critical/High нет).

План исполнимо разворачивает §23: фазы HM-P0..HM-P6 покрывают §23.6, ловушка `CORE_SRCS` присутствует, зависимость от Стадии 5 названа и удовлетворена, ссылки/команды реальны. Замечания HM-1..HM-6 (Med/Low) сложены:

| ID | Severity | Учтено |
|---|---|---|
| HM-1 | Med | HM-P0/DoD: задана **reset/wrap**-семантика `delta_u64` (cur<prev ⇒ re-baseline, значение `null`, не гигантское unsigned) + тест (design H2). |
| HM-2 | Med | Приложение P1: разведены **недоступное ⇒ `null`** и **caveated ⇒ значение присутствует + бит** (design M4 «caveated ≠ null»). |
| HM-3 | Med | HM-P0/P3: зафиксирована **раскладка битов** маски (enum-константы `present` по-полям, `caveat` по-источникам) в `host_metrics.h`; анализатор ссылается на неё. |
| HM-4 | Low | HM-P0: добавлен явный шаг расширения `tests/test_no_alloc.cpp` (QAS-H4). |
| HM-5 | Low | Приложение P1: пример `present_mask` приведён к числу полей (10 ⇒ 1023). |
| HM-6 | Low | Приложение P1: `udp_in_csum_errors` помечен как добавление сверх §23.2 (в духе parse-by-name). |

**Fine (не менять):** engine-free модуль; `platform_linux` без изменений; default OFF; регистрация через `register_cvars`+`CvarRefs`; зависимость от `null`-эмиттера Стадии 5; parse-by-name; «флаг, не 0-здоровье»; `steal` — %; первый сэмпл `null`; `statvfs` по `hldsmon_dir`; отсутствие новых экспортов; границы (без loadavg/eBPF/агента/Windows).

---

# Стадия 7 — `client-quality` — план реализации (rev 1)

> Jitter per-client + скользящий loss (анализатор). Авторитетный дизайн — `docs/design.md` §24 (rev 8; D1: `reliable_overflow` вынесен из scope; D2: окно по `loss`).

## Заголовок стадии (bindings)
- **Design:** §24 rev 8; oracle PASS-WITH-REVISIONS (H1/H2/M1-M4 folded). **Plan-review:** `momus` (ожидает).
- **Решения:** D1 — overflow вне scope; D2 — окно по `loss` в анализаторе. **Коммит** — только по явной просьбе.
- **Верификация:** `make`, `make test`, `make verify-exports` (whitelist без изменений).

## Глобальные инварианты
INV-1..6. **INV-2:** jitter — O(1) целочисленно; IP+port матч ≤`MAX_CLIENTS`(32). **INV-3:** server thread.

## Новые cvar'ы: **нет.**

## Маппинг QAS-CQ1..CQ5 → метод (design §24.8)
| QAS | Где | Метод |
|---|---|---|
| CQ1 (jitter min/avg/max) | CQ-P0 | host-тест |
| CQ2 (rolling loss корректен) | CQ-P1 | python-тест |
| CQ3 (`sizeof(ClientSample)<256`) | CQ-P0 | static_assert |
| CQ4/CQ5 (без аллокаций; make/make test/verify) | CQ-P2 | CI/локально |

## CQ-P0 — Jitter-ядро + IP+port матч
**Файлы:** `src/metrics.h` (`ClientSample` +`jit_min_ms`/`jit_avg_ms`/`jit_max_ms` — 3×uint16=6Б; host-testable inline-аккумулятор), `src/plugin.h` (`ClientRuntime`: `prev_pkt_real`, packed `ip`/`port`, `jit_min/sum/max/count`), `src/hooks.cpp` (`PreprocessPacket`: матч по IP+port ≤32 → накопление Δ через `GetRealTime`, guard; reset на connect/gen), `src/sampler.cpp` (reset per-client после сэмпла, под `client_hz>0`), `tests/test_client_quality.cpp`.
**Границы:** p99/гистограмма — вне (бюджет). **Acceptance:** `make test` (вкл. `test_client_quality`); `static_assert(sizeof(ClientSample)<256)` (247+6=253).
**Ripple/ревью (CQ-1..CQ-5):** обновить `static_assert(sizeof(DropRecord))` (`src/metrics.h`) + проверки размеров в `tests/test_serialization_events.cpp`/`tests/test_config.cpp` (рост `window[64]`: +6×64=384Б); `static_assert(sizeof(ClientSample)==247)` → `==253`; packed-IP называть без конфликта с `ClientRuntime::ip[64]` (напр. `ip_packed`/`port_packed`); short-circuit `client_hz>0` применяется и к hot-path матчу; уточнить дизайн §24 (убрать устаревшие упоминания overflow).
**Риски:** hot-path матч — только целочисленно; clamp uint16.

## CQ-P1 — Сериализация + анализатор
**Файлы:** `src/serialization.cpp` (поля jitter в `client`), `tools/analyzer/hldsmon.py` (скользящее окно loss по `loss`), fixtures/tests.
**Acceptance:** `make test` (python зелёные). **Verify:** `make test`.

## CQ-P2 — Верификация/документация
`make clean && make && make verify-exports && make test`; README (jitter).
## CQ-P3 — Запись стадии (journal).
## CQ-P4 — Коммит (гейт).

## Definition of Done
- [ ] `ClientSample` +3 jitter-поля, `<256` (CQ3); jitter host-testable (CQ1).
- [ ] IP+port матч в `PreprocessPacket`, reset per-client/gen, под `client_hz>0`.
- [ ] Скользящий loss в анализаторе по `loss` (CQ2); `reliable_overflow` вне scope.
- [ ] `make`/`make test`/`make verify-exports` зелёные; whitelist без изменений.
- [ ] `docs/journal.md` заполнен. Коммит — только по просьбе.

## Приложение P1 — расширение: `client` +`jit_min_ms`/`jit_avg_ms`/`jit_max_ms`.

## Раздел ревью плана (momus) — стадия `client-quality`
**Ревьюер:** `momus`. **Вердикт: PASS-WITH-REVISIONS; гейт ОТКРЫТ** (блокеров нет). CQ-1..CQ-5 сложены (ripple `DropRecord`/точный `static_assert`/имя packed-IP/`client_hz`-гейт/гигиена дизайна §24).
**Fine:** 3×uint16 min/avg/max; отказ от гистограммы/`clientq`; бюджет `247+6=253<256`; D1 (overflow вне scope)/D2 (окно по `loss`); `Makefile` авто-глоб тестов; `GetRealTime`+clamp; IP+port ≤32.

---

# Стадия 8 — `connect-timings` — план реализации (rev 1)

> Тайминги коннект-пайплайна + длительность загрузки карты. Дизайн — §25 (rev 9; Q1 решён). **Plan-review:** `momus`.

## Заголовок стадии (bindings)
- **Design:** §25 rev 9; oracle PASS-WITH-REVISIONS (CT-1..CT-9 folded). **Решения:** Q1 — готовность = первый server-tick после `SV_Spawn_f` с доступным миром/спавном, окно 30 тиков, иначе `null`. **Коммит** — по явной просьбе. **Верификация:** `make`/`make test`/`make verify-exports`.

## Глобальные инварианты
INV-1..6; единый контекст «коннект в процессе» (не per-slot); `GetRealTime()` + guard; short-circuit ⇒ `null`.

## Новые cvar'ы: **нет.**

## Маппинг QAS-CT1..CT4 → метод
| CT1/CT2 (длительности reject/connect) | CT-P0 | host-тест сборки записи |
| CT3 (`mapchange.load_ms`, `null`) | CT-P1 | unit + smoke |
| CT4 (без аллокаций; CI) | CT-P2 | CI |

## CT-P0 — Коннект-контекст + тайминги
**Файлы:** `src/plugin.h` (`struct ConnectCtx { double t0; double steam_enter; uint8_t active; }` + reset-точки), `src/hooks.cpp` (`SV_ConnectClient` → `t0`+`active=1`; валидаторы → `stage_ms` вокруг `callNext`; `reject` +`stage_ms`/`total_ms`; `Steam_NotifyClientConnect` → `steam_ms`; `ClientConnected` → `auth_ms`/`steam_ms`, `active=0`), `src/metrics.h` (`RejectRecord`/`ConnectRecord` поля), serialization.
**Ноль-семантика (CTP-1):** `uint32` не может быть `null` ⇒ **`present`-бит** (как в `ConfigSnapshot`/`HostRecord`) + `kv_u32_or_null`; нет метки/не наступило ⇒ `null`. **Тип:** `uint32` мс (как `HitchRecord.duration_ms`).
**Границы:** не менять логику валидаторов; **host-testable** вынести `delta→ms`-конвертер+guard в `metrics.h` (CTP-4); `static_assert(sizeof(RejectRecord))`/`(sizeof(ConnectRecord))` обновить (CTP-5).
**Acceptance:** `make`; host-тест конвертера/сборки. **Риски:** short-circuit/abandoned connect ⇒ `null`; **синхронность пайплайна (CTP-3) — проверить эмпирически по логу `stage`; иначе `null`**.

## CT-P1 — Загрузка карты (delayed-emission)
**Файлы:** `src/hooks.cpp` (`SV_Spawn_f` → `t_spawn`+pending-map+tick-counter, **не эмитить сразу**; reset massdrop остаётся), `src/plugin.h` (`MapCtx { double t_spawn; char pending_map[64]; uint8_t pending; uint16_t ticks; }`), `src/sampler.cpp` (эмиссия `mapchange` при готовности/окне), `src/metrics.h` (`MapChangeRecord` +`load_ms`+present), serialization.
**Критерий готовности (CTP-2, решён):** первый server-tick после `SV_Spawn_f`, на котором мир доступен — **конкретный предикат:** `g_engfuncs.pfnGetModelPtr() != nullptr` **или** непустой `current_map()`; окно **30 тиков**; не наступило ⇒ `load_ms=null` (present-бит снят). Только при смене карты (сохранить `strcmp`-гейт).
**Acceptance:** `load_ms` = от spawn до готовности; не наступило ⇒ `null`; `static_assert(sizeof(MapChangeRecord))` обновлён (CTP-5).

## CT-P2 — Верификация/docs; CT-P3 — journal; CT-P4 — commit gate.

## Definition of Done
- [ ] Единый контекст коннекта; `stage_ms`/`total_ms`/`auth_ms`/`steam_ms` (`uint32` мс, nullable); short-circuit ⇒ `null`.
- [ ] `mapchange.load_ms` delayed-emission; готовность по Q1 (окно 30 тиков); иначе `null`.
- [ ] `GetRealTime()` + guard; `DropRecord` не трогается.
- [ ] `make`/`make test`/`make verify-exports` зелёные; whitelist без изменений.
- [ ] `docs/journal.md`. Коммит — только по просьбе.

## Приложение P1 — расширение: `reject`+`stage_ms`/`total_ms`; `connect`+`auth_ms`/`steam_ms`; `mapchange`+`load_ms`.

Поля добавлены в **существующие** записи (в конце объекта; канонический блок выше обновлён). Все — `uint32` мс (как `HitchRecord.duration_ms`), **nullable**: измерение отсутствует (short-circuit стадии другим плагином, нет метки, guard `GetRealTime()` на `<=0`/отрицательную дельту) ⇒ JSON `null` (present-бит снят), а не `0` (CT-4/CT-6/CT-8).

| Поле | Семантика |
|---|---|
| `reject.stage_ms` | длительность стадии валидатора вокруг `callNext` (напр. `SV_CheckUserInfo`) |
| `reject.total_ms` | от входа `SV_ConnectClient` до отказа |
| `connect.auth_ms` | от входа `SV_ConnectClient` до `ClientConnected` (успех) |
| `connect.steam_ms` | длительность стадии `Steam_NotifyClientConnect` |
| `mapchange.load_ms` | от `SV_Spawn_f` до готовности мира (Q1: первый 1 Гц server-tick с непустым `current_map()`); не наступило за окно 30 тиков ⇒ `null` |

- Единый контекст «коннект в процессе» (server thread); новый `SV_ConnectClient` перезаписывает контекст (CT-1/CT-7). `DropRecord` не изменяется.
- `mapchange` — **delayed-emission** (CT-2): `SV_Spawn_f` только фиксирует метку спавна; запись эмитится на 1 Гц server-tick при готовности/окне (append-only JSONL не позволяет дополнить строку).

## Раздел ревью плана (momus) — стадия `connect-timings`
**Ревьюер:** `momus`. **Вердикт: PASS-WITH-REVISIONS; гейт ОТКРЫТ после сложения** (было 2 High). Сложено: CTP-1 (`present`-бит + `kv_u32_or_null` для «null»); CTP-2 (конкретный предикат готовности: `pfnGetModelPtr`/`current_map()` + окно 30 + хранение pending в `plugin.h`); CTP-3 (проверка синхронности эмпирически; иначе `null`); CTP-4 (host-testable конвертер+guard); CTP-5 (`static_assert`); CTP-6 (P1-схема полей).
**Fine:** поля в существующих записях; `GetRealTime()`; `stage_ms` вокруг `callNext`; `DropRecord` не трогается.

---

# Стадия 9 — `event-enrich` — план реализации (rev 1)

> Детектор-поле в `drop`; top-N; события деградации; edict churn. Дизайн — §26 (rev 10; EN-1..EN-7 folded, EN-7/Q2 решены). **Plan-review:** `momus`.

## Заголовок стадии (bindings)
- **Design:** §26 rev 10; oracle PASS-WITH-REVISIONS. **Решения (EN-7):** детектор — поле в `drop`; `top_ips` аддитивно; hard-failure — console. **Коммит** — по просьбе. **Верификация:** `make`/`make test`/`make verify-exports`.

## Глобальные инварианты
INV-1..6; гейт `plugin_enabled()`; `LogWriter` — sink (pull).

## Новые cvar'ы: **нет.** Новые типы: **`degraded`**, **`retention`**.

## Маппинг QAS-E1..E4 → метод
| E1 (детектор рядом с drop) | EN-P0 | host-тест матча |
| E2 (top-N) | EN-P0 | host-тест |
| E3 (`degraded`/`retention`) | EN-P1 | host-тест pull |
| E4 (edicts_churn; без аллокаций; CI) | EN-P0/P2 | CI |

## EN-P0 — Детектор-поле, top-N, edicts_churn
**Файлы:** `src/metrics.h` (`DropRecord` +`detector[32]`; `PacketStats` +`top_ips[3]` — POD `IpCount { char ip[64]; uint32_t count; }` (EV-5), keep `top_ip`; `ServerSample` +`int32 edicts_churn` + `uint8_t edicts_churn_present` (EV-2)), `src/plugin.h`/`src/hooks.cpp` (матч подстрок в `classify_reason`-стиле → `detector`; reset churn-baseline на `SV_Spawn_f`), `src/sampler.cpp` (one-pass top-3; churn Δ signed + present; reset baseline на mapchange), serialization (`kv_i64`/`kv_u32_or_null`), fixtures.
**Acceptance:** `make test`; тест на реальной строке детектора. **Риски:** substring хрупкость (best-effort).

## EN-P1 — События деградации (pull)
**Файлы:** `src/log_writer.{h,cpp}` (`last_prune_count()`+clear; `error_count()`/`size_capped()`/`enabled()` — уже есть), `src/plugin.h` (baseline `prev_error_count`/`prev_prune_count` в `PluginContext`), `src/sampler.cpp` (1 Гц: дельты → `degraded`/`retention` только при `log->enabled()`), `src/events.{h,cpp}`, `src/control.cpp` (`hldsmon_status` — hard-failure видимость), metrics/serialization.
**Схема (EV-1):** `degraded {type,t,error_count}` (Δ), `retention {type,t,removed}` (Δ). **Границы:** hard write-failure — console (`pfnServerPrint`)/`hldsmon_status`, **не** JSONL. **Acceptance:** `make`; `degraded`/`retention` эмитятся; callback нет.

## EN-P2 — Верификация/docs; EN-P3 — journal; EN-P4 — commit gate.

## Definition of Done
- [ ] `drop.detector` (best-effort, `""` при промахе, не влияет на drop); тип `filter` не вводится.
- [ ] `packet.top_ips` (N=3) + сохранён `top_ip`; `static_assert` правлены.
- [ ] `degraded`/`retention` (pull, при enabled); `write_error` не вводится; hard-failure — console.
- [ ] `edicts_churn` `int32` + present/`null` + reset на mapchange; `drop.server` учтён.
- [ ] P1/фикстуры/`KNOWN_TYPES` для `degraded`/`retention`; `make`/`make test`/`make verify-exports` зелёные.
- [ ] `docs/journal.md`. Коммит — по просьбе.

## Приложение P1 — расширение: `drop.detector`; `packet.top_ips`; `server.edicts_churn`; типы `degraded`/`retention`.

- `drop.detector` (`char[32]`) — best-effort подстрочный матч reason против набора детекторов `rehlds_security` (`movecmd`/`stringcmd`/`dlfile`/`speedhack`); промах — `""`; drop не меняется; тип `filter` не вводится (EN-1/EN-3/EV-1/EV-5).
- `packet.top_ips` — top-3 `{ip,count}` (const N=3), аддитивно к сохранённому `top_ip`; один линейный проход вне hot path (EN-4).
- `server.edicts_churn` — знаковый `int32` Δ `pfnNumberOfEntities()` за интервал + present-бит (`null` на первом интервале/после mapchange); baseline сбрасывается на `SV_Spawn_f`; вложенный `drop.server` наследует поле (EN-5).
- `degraded {type,t,error_count}` / `retention {type,t,removed}` — pull из 1 Гц sampler по флагам/счётчикам `LogWriter` (`enabled()`/`error_count()`/`last_prune_count()`+clear), только пока `log->enabled()`; hard write-failure/кап — console/`hldsmon_status` (EN-2/EN-7).

## Раздел ревью плана (momus) — стадия `event-enrich`
**Ревьюер:** `momus`. **Вердикт: OKAY (PASS); гейт ОТКРЫТ.** Сложены неблокирующие EV-1..EV-5: схема `degraded`/`retention` (`error_count`/`removed`); present-флаг `edicts_churn`; `plugin.h`/`control.cpp` в файлах; POD `IpCount`; актуализировать stale §26.5/§26.6.
**Fine:** детектор-поле в `drop`; `top_ip` сохранён; pull-модель без callback; hard-failure=console; точки вставки (`hooks.cpp:277,493`, `sampler.cpp:326`).

---

# Стадия 10 — `analyzer-ui` — план реализации (rev 1)

> Host-overlay, incident bundle (JSON-конверт), config-diff. Дизайн — §27 (rev 11; AU-1..AU-9 folded, Q1 решён). **Только `tools/analyzer`.** **Plan-review:** `momus` (или сокращённый для UI-стадии).

## Заголовок стадии (bindings)
- **Design:** §27 rev 11; oracle PASS-WITH-REVISIONS. **Решения (Q1):** bundle — JSON-конверт (zip опционален). **Коммит** — по просьбе. **Верификация:** `make test` (python).

## Глобальные инварианты
Плагин/схема не меняются; stdlib-only.

## Новые cvar'ы/типы: **нет** (только analyzer/UI).

## Маппинг QAS-U1..U4 → метод
| U1 (bundle, ре-инжестится) | UI-P1 | python-тест |
| U2 (config-diff) | UI-P1 | python-тест |
| U3 (host-overlay, caveats) | UI-P2 | python-тест + review |
| U4 (без плагинных изменений) | UI-P2 | CI |

## UI-P1 — Библиотека: `config_at`, `config_diff`, `incident_bundle`, `field→caveat`
**Файлы:** `tools/analyzer/hldsmon.py` (`config_at(records,t)`; `config_diff(before,after)` → `{changed,added,removed,baseline}` по `CONFIG_FIELDS`+`map`/`maxclients`/`rehlds_*`, null=`set`/`cleared`; **карта field→caveat-source рядом с `HOST_CAVEAT_BITS`** (AU-5/F3); `incident_bundle(records, drop, mins)` → JSON-конверт `{bundle_version, drop, window, config, host, records}` + ридер), fixtures/tests.
**Дедуп (AU-2/F1):** `drop` as-is **или** сырые `records` — **не оба безусловно** (дроп уже несёт `window[64]`); верхняя граница размера + предупреждение. **Acceptance:** bundle ре-инжестится в `iter_records`; граница размера; кейс «нет baseline». **Verify:** `make test`.

## UI-P2 — Webui payload + UI
**Файлы:** `tools/analyzer/webui.py` (`/api/analyze` +`host_series`/`config`; `/api/bundle`), `webui/index.html` (host-overlay: 3 состояния точки, caveat-маркер; config-diff панель; кнопка bundle; `ALL_DATASETS`/localStorage).
**Acceptance:** UI рисует host/caveat/config-diff; bundle скачивается.

## UI-P3 — Верификация/docs; UI-P4 — journal; UI-P5 — commit gate.

## Definition of Done
- [ ] `config_at`/`config_diff` (`{changed,added,removed,baseline}`); «нет baseline» — флаг.
- [ ] `incident_bundle` — JSON-конверт + ридер; ре-инжестится; граница размера.
- [ ] `/api/analyze` отдаёт `host_series`+`config`; `/api/bundle`; UI: overlay (null=разрыв, caveat=маркер), config-diff, bundle.
- [ ] Python-тесты зелёные; плагин/схема не тронуты. `docs/journal.md`. Коммит — по просьбе.

## Раздел ревью плана (momus) — стадия `analyzer-ui`
**Ревьюер:** `momus`. **Вердикт: OKAY (PASS); гейт ОТКРЫТ.** Сложены F1..F4: политика дедупа `drop`/`records`; карта field→caveat-source в `hldsmon.py`; набор `config_diff` (`CONFIG_FIELDS`+`map`/`maxclients`/`rehlds_*`, `set`/`cleared`); QA достаточно.
**Fine:** только `tools/analyzer`; реюз `host_series`/`iter_records`/`cmd_window`/`_config_digest_block`; три состояния host-точки (`None`≠0); точка интеграции «Все датасеты».

---

# Стадия 12 — `large-logs` — план реализации (rev 2 — plan-review пройден; plan-gate ЗАКРЫТ; **стадия исполнена и верифицирована — 2026-10-13**)

> Полный дневной лог (~700 МБ) сквозь веб-UI (локальная загрузка + FTP-live) без обрыва на 256 МиБ / 30-мин хвосте. Дизайн — §28 (rev 12; B1/B2/M1–M7/m1–m5 сложены; R2/R3 — PASS, design-gate закрыт). **Plan-review:** `momus` (PASS-WITH-REVISIONS — H1–H3/M1–M4/L1–L2 сложены, см. раздел ревью ниже). **Scope:** `tools/analyzer` (`webui.py`, `webui/index.html`, `hldsmon.py`, `tools/analyzer/tests/`) **+ плагинный дефолт продюсера** (`hldsmon_max_mb` → `0`). **JSONL-схема P1 / `KNOWN_TYPES` не меняются.**

## Заголовок стадии (bindings)
- **Design:** `docs/design.md` §28 (rev 12). **Design-review:** `oracle` (R1 PASS-WITH-REVISIONS; R2 PASS-WITH-REVISIONS; **R3 PASS — design-gate ЗАКРЫТ**, §28.11). **Plan-review:** `momus`. **Design doc:** `docs/design.md`; **Plan:** `docs/plan.md` (этот раздел); **Journal:** `docs/journal.md`; **Открытые вопросы:** `docs/design.md` §16.
- **Язык документации:** русский; идентификаторы/код — английский.
- **Решения владельца (закрыто 2026-10-13):** §16 O4–O9 закрыты; §28.9. Кратко: `large-logs` — **чтение и запись без лимита по умолчанию** (лимит — опциональный клапан, выключен); память ограничена **стримингом** (диск-spool + оконный анализ), а не размерным cap; **window/период — UX**, не потолок; серверный период/window + coarse-overview — обязательный scope; FTP-окно UX удобное/очевидное (REQ-L6); компактный «Период» (REQ-L7); опциональный sticky-тоггл с персистом (REQ-L8).
- **Верификация (точно как в `AGENTS.md`):** `make` (сборка 32-бит); `make test` (хостовые C++ unit + Python-тесты анализатора); для точечного прогона Python-тестов — `cd tools/analyzer && python3 -m unittest discover -s tests -v`. **Линтер — компилятор `-Wall -Wextra`** (отдельного нет).
- **Коммит:** только по **явной** просьбе владельца, один связный коммит на стадию (код + тесты + доки + запись журнала). Без авто-коммитов.
- **Интеграционная проверка:** FTP-путь требует реального FTP-хоста (вне CI); локальный путь и анализ дня покрываются хостовыми Python-тестами. UI-QAS-L16/L17/L18 — **вне `make test`** (метод — §28.8: ручной QA + page-`selfTest` `webui/index.html:4144`).

## Глобальные инварианты стадии `large-logs`
Дизайн §28.5. Действуют на всех фазах; проверяются картой QAS ниже.
- **INV-L1** — память приёма **O(чанк)**; память анализа **O(window)**; сервер **всегда** применяет окно (подставляет default-окно при пустых `from`/`to`); сегментный spool сам по себе O(n²) **не** снимает.
- **INV-L2** — **нет размерного лимита по умолчанию**; память ограничена стримингом (диск-spool + coarse-агрегатор без полного списка записей); клапан `>0` — только явно и с машиночитаемым отказом.
- **INV-L3** — ни один путь не даёт **молча 0 записей / тихую обрезку**.
- **INV-L4** — spool на **диске**, process/session-бюджет + crash-safe cleanup.
- **INV-L5** — **stdlib-only**; схема P1/`KNOWN_TYPES` без изменений; `ftp.bytes`/`reached_start` — семантика **удалённого** файла.
- **INV-5/INV-6 (сквозные):** формат JSONL P1 не меняется; границы (без БД/внешнего агента/Windows) сохраняются. `hldsmon.iter_records` (`hldsmon.py:163-187`) **не трогается** (BOM снимается при приёме на сервере).

## Новые флаги/ENV стадии (cvar плагина — см. LL-P5)
Все ручки **переопределяемы флагом/env**; значения ниже — **зафиксированные дефолты** (H1).
| Ручка | Где | Default | Семантика |
|---|---|---|---|
| `--max-log-mb` (+ env `HLDSMON_WEBUI_MAX_MB`) | `webui.py` `build_parser:1311` | **`0` = unlimited** | Опциональный клапан чтения (REQ-L3). `>0` — единый отказ `{"code":"log_too_large","limit_bytes":N}`; для local — на запрос, для FTP — **кумулятивно на сессию**. |
| `--spool-dir` (+ env `HLDSMON_SPOOL_DIR`) | `webui.py` `build_parser:1311`/`main` | `<tempdir>/hldsmon-spool` → при tmpfs-fallback `<cwd>/.hldsmon-spool` | Диск-backed каталог spool (INV-L4). fstype-проверка + fallback; free-space pre-check. |
| `--max-uploads` (+ env `HLDSMON_WEBUI_MAX_UPLOADS`) | `build_parser:1311`/`main` | **`4`** (константа `MAX_UPLOADS = 4`) | Семафор (`threading.BoundedSemaphore`) одновременных загрузок/сессий; превышение → HTTP **429** `{"ok":false,"code":"busy"}` (не неограниченные потоки). |
| `--read-timeout` (+ env `HLDSMON_WEBUI_READ_TIMEOUT`) | `build_parser:1311`/`main` | **`30.0` с** (константа `READ_TIMEOUT_S = 30.0`) | socket/read-timeout тела и FTP-чтения; недобор тела к EOF за таймаут → HTTP **408** `{"ok":false,"code":"request_timeout"}`. |
| `--spool-session-mb` (+ env `HLDSMON_WEBUI_SPOOL_SESSION_MB`) | `build_parser:1311`/`main` | **`2048` МиБ** (константа `SPOOL_SESSION_MB = 2048`) | Per-session бюджет spool (ресурсный предохранитель, **не** size-cap лога); исчерпание → `spool_full`. |
| `--spool-total-mb` (+ env `HLDSMON_WEBUI_SPOOL_TOTAL_MB`) | `build_parser:1311`/`main` | **`8192` МиБ** (константа `SPOOL_TOTAL_MB = 8192`) | Process-wide бюджет spool; исчерпание → `spool_full`. |
| `--spool-ttl-sec` (+ env `HLDSMON_WEBUI_SPOOL_TTL_SEC`) | `build_parser:1311`/`main` | **`3600` с** (константа `SPOOL_TTL_S = 3600`) | TTL age-sweeper'а: handle/сегменты старше TTL удаляются; плюс startup-cleanup на старте. |
| `--allow-nonloopback-unlimited` | `build_parser:1311` | **off** | Opt-in для безлимита на **не-loopback** bind — точная семантика (M4) ниже. |
| `hldsmon_max_mb` (плагин) | `src/bootstrap.cpp:55` / `:308` | **`0` = unlimited** | Продюсер без лимита записи (REQ-L4). Регистрация `:124` без изменений; `src/log_writer.cpp` **не трогается** (`:184`/`:225` уже `0`=unlimited). |

**Loopback/безлимит (M4, §28.3 п.14/п.7).** При bind на **не-loopback**: без `--allow-nonloopback-unlimited` сервер стартует (в лог — предупреждение), но при `--max-log-mb == 0` **любой** приём отклоняется HTTP **403** `{"ok":false,"code":"unlimited_nonloopback_disabled"}`; при `--max-log-mb > 0` приём работает. С `--allow-nonloopback-unlimited` — безлимит разрешён. На **loopback** безлимит разрешён всегда. QAS-L7 проверяет обе ветки.

## Не-цели стадии (границы)
- JSONL-схема P1, `KNOWN_TYPES`, `exports.whitelist`, Makefile, ABI/хуки — не трогаются. Плагинное изменение ограничено дефолтом `hldsmon_max_mb`.
- Аутентификация/многопользовательская модель; внешние зависимости (остаёмся stdlib-only); resumable/chunked-upload; Windows.
- Realtime-дашборд и кэш повторных окон/инкрементальный агрегатор **сверх** (ii-a)+(ii-b). Пред-агрегация coarse (ii-b) — **в scope**, не исключается.
- `hldsmon_retention_days` — не трогаем.

## Маппинг QAS-L1..L20 → фаза/метод (design §28.8)
| QAS-L (design §28.8) | Фаза | Метод проверки |
|---|---|---|
| **QAS-L1** (local >256 МиБ, RSS ∝ не файлу, без JS-строки) | LL-P1 (+P2/P4) | Python-тест (subprocess): приём 200; **удвоение входа (300→600 МиБ) ⇒ рост пика `ru_maxrss` ≤ `RSS_SLOPE_MAX_MIB = 64` МиБ** (не пропорционально) |
| **QAS-L2** (no cap по умолчанию; valve `>0` ⇒ 413 `log_too_large`; missing `Content-Length` ≠ 0) | LL-P1 | Python-тест (HTTP) |
| **QAS-L3** (FTP хвост → offset 0; RSS ~O(окно)) | LL-P3 (+P2) | Python-тест FTP-сегментов; **пик `ru_maxrss` ≤ `RSS_ANALYSIS_MAX_MIB = 256` МиБ и ≤ `RSS_WINDOW_SENS_MIB = 32` МиБ** над прогоном 5-мин окна |
| **QAS-L4** (`cap_hit`+`limit_bytes` при клапане; иначе не выставляется) | LL-P3 | Python-тест |
| **QAS-L5** (FTP без `SIZE`/`REST` → RETR→spool; без `_FtpTooLarge`) | LL-P3 | Python-тест (fake-FTP) |
| **QAS-L6** (export/bundle через handle local **и** FTP; ключ `(run,t,userid)`; `session_expired`; малый body-путь) | LL-P2 (+P3/P4) | Python-тест |
| **QAS-L7** (не-loopback ⇒ предупреждение; безлимит требует opt-in) | LL-P1 | Python-тест: non-loopback + безлимит без флага ⇒ **403 `unlimited_nonloopback_disabled`**; с флагом ⇒ 200; loopback ⇒ 200 |
| **QAS-L8** (spool на диске; free-space; бюджет; `spool_full`) | LL-P1 | Python-тест (fstype-resolver + budget) |
| **QAS-L9** (cleanup: startup + sweeper + `finally`/BrokenPipe) | LL-P1 | Python-тест |
| **QAS-L10** (память анализа дня: окно ≤ записей окна; coarse без полного списка) | LL-P2 | Python-тест: ответ без `records`; **пик `ru_maxrss` ≤ `RSS_ANALYSIS_MAX_MIB = 256` МиБ и ≤ `RSS_WINDOW_SENS_MIB = 32` МиБ** (30-мин vs 5-мин окно) |
| **QAS-L11** (BOM → первая строка не теряется; CRLF + порядок `Blob`↔`files=`) | LL-P1 (BOM) + LL-P4 (CRLF/порядок) | Python-тест + ручной QA |
| **QAS-L12** (лимит одновременных загрузок/сессий + read-timeout) | LL-P1 | Python-тест |
| **QAS-L13** (окно `from`/`to` включительно == эталонная клиентская фильтрация) | LL-P2 | Python-тест (эталон) |
| **QAS-L14** (смена суток в FTP: без потерь/дублей) | LL-P3 | Python-тест |
| **QAS-L15** (stdlib-only; схема P1 без изменений; старые Python-тесты + новые зелёные) | LL-P6 (сквозь P1–P4) | `make test` |
| **QAS-L16** (FTP-окно UX) | LL-P4 (+P3) | **ручной QA/`selfTest`** (вне `make test`) |
| **QAS-L17** (компактный «Период») | LL-P4 | **ручной QA/`selfTest`** (вне `make test`): высота sticky-блока **≤ `PERIOD_MAX_HEIGHT_PX = 96` px** |
| **QAS-L18** (sticky-тоггл персист в `localStorage`) | LL-P4 | **ручной QA/`selfTest`** (вне `make test`) |
| **QAS-L19** (продюсер `hldsmon_max_mb=0`=unlimited + клапан; `test_log_writer`) | LL-P5 | C++ unit (`set_max_mb(0)`) + **статическая проверка default-строки `bootstrap.cpp` + ручной `hldsmon_status`** (см. LL-P5) |
| **QAS-L20** (чтение без лимита по размеру; память не O(файл)) | LL-P1 (local) + LL-P3 (FTP) | Python-тест: приём произвольного размера; пик `ru_maxrss` ≤ `RSS_SLOPE_MAX_MIB = 64` МиБ при удвоении входа |

**Покрытие REQ-L:** REQ-L1 → P1(local)/P3(FTP); REQ-L2 → P1+P2; REQ-L3 → P1; REQ-L4 → P5; REQ-L5 → P2; REQ-L6 → P3(механика)+P4(UX); REQ-L7 → P4; REQ-L8 → P4.

---

## LL-P1 — Analyzer core: spool-приём, handle-реестр, единый over-cap-контракт, клапан
**Цель.** Оба пути приёма (local и — каркас — сессия) кладут байты в диск-spool, не в память; server-side handle с lifecycle/TTL/cleanup; опциональный клапан (`--max-log-mb` default `0`); loopback-guard; лимит параллелизма + read-timeout; BOM снимается при приёме.

**Файлы.**
- `tools/analyzer/webui.py`:
  - `_read_body:1049-1070` → **стрим в spool-файл** со счётчиком байт (не в `chunks`); убрать отклонение по `MAX_BODY_BYTES` **по умолчанию**. **Framing (M3, детерминированно):** (i) `Content-Length` **отсутствует** → HTTP **411 Length Required** `{"ok":false,"code":"length_required"}`; (ii) `Content-Length` **присутствует, но невалиден** (не целое >0) → **стрим до EOF** (`Connection: close`, `close_connection=True`), ограниченный read-timeout → **408** `{"code":"request_timeout"}`; (iii) `Content-Length` валиден >0 → стрим **ровно N байт**; недобор к EOF → **408**. Никогда тихий `b""` → 0 записей. BOM (`utf-8-sig`) снимается здесь, в одном месте.
  - Константы `MAX_BODY_BYTES:53`/`MAX_FTP_BYTES:56` → источник клапана (`--max-log-mb`); переименовать в `_MAX_LOG_BYTES` (0 = unlimited).
  - `_handle_analyze:1141`, `_handle_export:1155`, `_handle_bundle:1177` → при клапане `>0` единый **читаемый JSON** `{"ok":false,"code":"log_too_large","limit_bytes":N}` (без `close_connection`/«Failed to fetch»).
  - Новое: **реестр spool-handle** (dict + lock), handle-адресация, lifecycle/TTL, **startup-cleanup + age-sweeper + `finally`/`BrokenPipe`**-cleanup; вытеснение → `{"code":"session_expired"}`.
  - fstype-resolver `spool_backing_kind(mounts_text, path)` (**host-testable**, чистый; читает `/proc/mounts`) + выбор disk-backed каталога и fallback из tmpfs; free-space pre-check (`os.statvfs`); **per-session (`SPOOL_SESSION_MB`) и process-wide (`SPOOL_TOTAL_MB`) бюджеты** → `spool_full`.
  - `build_parser:1311`/`main` → флаги/константы из таблицы выше + loopback-guard (M4: non-loopback + unlimited без флага ⇒ 403 на приём) + семафор (`MAX_UPLOADS`) + `READ_TIMEOUT_S`.
  - `_records_from_bytes:166-181` → temp-файл из spool (crash-safe), `finally`-удаление.
  - Новые константы (overridable flag/env — таблица выше): `MAX_UPLOADS = 4`, `READ_TIMEOUT_S = 30.0`, `SPOOL_SESSION_MB = 2048`, `SPOOL_TOTAL_MB = 8192`, `SPOOL_TTL_S = 3600`.
- `tools/analyzer/tests/test_webui.py` (+ `fixtures/`): тесты приёма/клапана/handle/бюджета/cleanup/BOM.

**Границы (не-цели фазы).** Оконный анализ — LL-P2; FTP-логика — LL-P3; UI — LL-P4; продюсер — LL-P5. Chunked-upload нет. Схема P1/`KNOWN_TYPES`/`hldsmon.iter_records` не трогаются.

**Инварианты.** INV-L2 (каркас), INV-L3, INV-L4, INV-6.

**Acceptance (QAS-L1, QAS-L2, QAS-L7, QAS-L8, QAS-L9, QAS-L11-BOM, QAS-L12, QAS-L20):**
- [ ] Локальный файл >256 МиБ → HTTP 200; при удвоении входа (300→600 МиБ) пик `ru_maxrss` растёт **≤ `RSS_SLOPE_MAX_MIB = 64` МиБ** (subprocess; не пропорционально).
- [ ] По умолчанию **нет отклоняющего cap**; при `--max-log-mb >0` → 413 + `log_too_large` + `limit_bytes` (читаемый JSON).
- [ ] Framing (M3): отсутствие `Content-Length` → **411 `length_required`**; present-but-malformed → **стрим до EOF/408**; valid >0 → ровно N байт; никогда тихий 0.
- [ ] Loopback-guard (M4): non-loopback + `--max-log-mb=0` без `--allow-nonloopback-unlimited` ⇒ **403 `unlimited_nonloopback_disabled`**; с флагом или при `>0` ⇒ 200; loopback ⇒ 200.
- [ ] spool-каталог disk-backed (fstype-resolver); tmpfs → fallback; free-space pre-check; бюджеты `SPOOL_SESSION_MB=2048`/`SPOOL_TOTAL_MB=8192` → `spool_full`.
- [ ] Cleanup: startup-cleanup + age-sweeper (TTL `SPOOL_TTL_S=3600`) + `finally`/BrokenPipe; после закрытия/TTL spool-файлов нет.
- [ ] Лимит `MAX_UPLOADS=4` (5-я одновременная загрузка → **429 `busy`**) + `READ_TIMEOUT_S=30.0` (недобор → 408); не неограниченные потоки.
- [ ] BOM-файл → первая строка не теряется.

**Риски.** Флейки измерения RSS — фиксированные пороги (`RSS_SLOPE_MAX_MIB=64`) и subprocess-прогон; `/proc/mounts` недоступен → skip/fallback (не падать); крупные фикстуры — **генерировать синтетически**, не коммитить.

**Верификация.** `cd tools/analyzer && python3 -m unittest discover -s tests -v`; затем `make test`.

**DoD фазы.** Все пункты acceptance зелёные; `INV-L2/L3/L4` покрыты; существующие Python-тесты не красные (`test_hldsmon`/`test_webui`).

---

## LL-P2 — Оконный анализ + coarse стриминг-агрегатор + export/bundle через handle
**Цель.** `/api/analyze` принимает `from`/`to`/`window`, **всегда применяет окно** (default-окно при пустых входах); ответ = **coarse overview дня** (однопроходный агрегатор без полного списка записей) + handle; окна дозапрашиваются по требованию; export/bundle — по handle (+окно); стабильный ключ drop `(run,t,userid)`.

**Файлы.**
- `tools/analyzer/webui.py`:
  - `analyze_records:293-319` → разделить на **стриминг-агрегатор** (`coarse_overview(handle, ...)`: `digest.counts_by_type`, суммы по типам, `top drop reasons`, `player_groups`/`client_identities`, полный `drops` O(drops), `log_end`, счётчики `anomalies`; **без** полного `records`) и **материализатор окна** (`window_records(handle, from, to)` через `iter_records`).
  - `build_series:191-213` — переиспользовать для full-resolution окна + даунсэмпл-ряды coarse (память **O(window)**/O(точек даунсэмпла)); даунсэмпл ограничен **`SERIES_MAX_POINTS = 2000`** точек/ряд.
  - `_handle_analyze:1141-1154` → парсинг `from`/`to`/`window`; **всегда** default-окно; ответ coarse + handle.
  - `_handle_export:1155`/`_handle_bundle:1177` → источник — spool по **handle** (+окно); адресация drop — ключ `(run,t,userid)`; вытеснённый handle → `session_expired`; малый body-путь сохранён.
- `tools/analyzer/hldsmon.py`: стабильный ключ drop `(run,t,userid)` (реюз `_attach_runs:286`); оконная фильтрация на парсинге (без изменения `iter_records`).
- `tools/analyzer/tests/test_webui.py` + `fixtures/`.

**Контракт окна (decision-complete).** `from`/`to` — epoch-секунды, **включительно**; `window` — размер окна в секундах. Правила выбора диапазона: **(a)** заданы `from` и `to` → материализуется ровно `[from, to]`; **(b)** задан один конец → второй = ±`ANALYZE_DEFAULT_WINDOW_S`; **(c)** оба пусты → окно = последние `ANALYZE_DEFAULT_WINDOW_S` секунд, оканчивающееся на `t` последней записи spool. Новая константа **`ANALYZE_DEFAULT_WINDOW_S = 1800.0`** (отдельная от `FTP_WINDOW_S:59`; значение совпадает как реюз, сущность — нет). **Coarse overview строится всегда по всему spool**, независимо от окна. В ответе возвращается `handle`; «весь файл одним окном» — частный случай, когда диапазон покрывает весь spool (малый файл).

**Контракт coarse-ответа `/api/analyze` (M1, ключи зафиксированы).** Ответ — **всегда** объект с ключами (имена **одинаковы** на бэке LL-P2 и фронте LL-P4); `records` (сырой полный список) **не возвращается никогда**:
- `ok` (`true`), `version` (`PROTOCOL_VERSION`), `files` (список имён).
- **`handle`** (строка) — server-side spool-handle для последующих export/bundle/оконных запросов.
- **`window`** (`{"from": <epoch-sec|null>, "to": <epoch-sec|null>}`) — фактически материализованное full-resolution окно (`null` = открытый край).
- **`coarse`** (`true` для overview-запроса; `false`, когда запрошено конкретное окно) и **`window_count`** (int — число записей в окне).
- **Полнодневные агрегаты** (память O(players)+O(drops)+O(1); present в обоих режимах): `digest`, `clients`, `sessions`, `session_count`, `client_identities`, `drops`, `anomalies`, `anomalies_excluded`, `events`, `log_end`, `config`, `records_count` (всего записей в spool), `host_caveats`. Каждый drop несёт **`"key": {"run": <int>, "t": <float>, "userid": <int>}`** — адрес для `/api/bundle`.
- **Ряды** `series` (server), `client_series`, `host_series`: при `coarse=true` — **даунсэмпл** (≤ `SERIES_MAX_POINTS = 2000` точек на ряд); при `coarse=false` (окно) — **полное разрешение окна**.
- **`focus_points`** — окно-scoped (память O(событий окна)).
- **Отсутствует всегда:** `records`.

**Границы.** FTP-специфика — LL-P3; UI — LL-P4; новых полей записи нет; кэш повторных окон/инкрементальный агрегатор — не делаем (только ii-a+ii-b).

**Инварианты.** INV-L1 (всегда окно), INV-L2, INV-L3.

**Acceptance (QAS-L6, QAS-L10, QAS-L13):**
- [ ] Запрос окна материализует **≤ записей окна** (не всего файла); coarse дня строится **без полного списка**; пик `ru_maxrss` **≤ `RSS_ANALYSIS_MAX_MIB = 256` МиБ** и **≤ `RSS_WINDOW_SENS_MIB = 32` МиБ** над прогоном 5-мин окна (L10).
- [ ] `from`/`to` включительно дают **то же множество** записей, что эталонная клиентская фильтрация по полному набору (L13).
- [ ] Export/bundle по handle (+окно) для local; адрес drop — стабильный `key {run,t,userid}`; вытеснённый handle → `session_expired`; малый файл → весь файл одним окном (L6).
- [ ] Coarse-контракт **не содержит** `records`; ряды `series`/`client_series`/`host_series` присутствуют и при `coarse=true` ограничены **≤ `SERIES_MAX_POINTS = 2000`** точками.

**Риски.** Случайная материализация списка в агрегаторе (регресс памяти) — покрыть структурным тестом (ответ без `records`) + RSS-тестом; default-окно зафиксировано контрактом выше (`ANALYZE_DEFAULT_WINDOW_S = 1800.0`) и проверяется тестом.

**Верификация.** `cd tools/analyzer && python3 -m unittest discover -s tests -v`; `make test`.

**DoD фазы.** Acceptance зелёные; INV-L1/L2/L3 покрыты; прежний контракт приёма тела не сломан.

---

## LL-P3 — FTP: сегментный spool, без cap по умолчанию, REST-less fallback, суточная ротация, механика окна
**Цель.** Сессия хранит **упорядоченный список сегментных spool-файлов** (`[older…, current]`) + `t_lo/t_hi`/`spool_bytes`; `load-earlier` **добавляет** сегмент старше и доходит до offset 0; без cap по умолчанию; fallback без `REST`/`SIZE` стримит `RETR` в spool; смена суток — раздельные сегменты; механика окна (индикатор диапазона/`reached_start`/прогресс-поля).

**Файлы.**
- `tools/analyzer/webui.py`:
  - `ftp_connect:825`, `ftp_refresh:876`, `ftp_close:994` → модель сессии: сегментные spool-файлы + `t_lo/t_hi`/`spool_bytes` + handle; убрать остановку по `MAX_FTP_BYTES`.
  - `_load_tail:665-698`, `ftp_load_earlier:931-991` → добавление сегментов старше; **до offset 0** ⇒ `reached_start=true`; анализ — через оконный путь LL-P2 (снимает B2/O(n²)).
  - `_download:490`/`_download_range:548`/`_retr_all:465` → fallback стримит полный `RETR` **в spool-файл** (не в память); диапазоны нарезаются из файла.
  - `_ftp_meta:701-720` → `cap_hit`/`limit_bytes`/опц. `truncated` (только при клапане); `bytes` = байты **удалённого** диапазона (не спула); `reached_start` = offset 0 **удалённого** файла; поля индикатора диапазона (REQ-L6).
  - `ftp_refresh:876` суточная ротация (`:881-884`, `resolve_path` с `{date}`) → сегменты разных дней раздельно, без потерь/дублей на границе.
  - Константы `FTP_WINDOW_S:59` = **1800 с (дефолт окна)** / `FTP_BACK_CHUNK:62` = 8 МиБ — сохранить; **набор пресетов окна (H3): {300, 900, 1800, 3600, 21600, 86400} с**; прогресс — детерминированный бар (скачано/всего байт при известном `SIZE`, иначе индетерминированный) + фазовая подпись («Скачивание…»/«Разбор данных…»).
- `tools/analyzer/tests/test_webui.py` (+ fake-FTP фикстуры).

**Границы.** UI-рендер FTP-окна — LL-P4; auth нет; продюсер — LL-P5.

**Инварианты.** INV-L1, INV-L3, INV-L5 (`ftp.bytes`/`reached_start` — семантика удалённого файла).

**Acceptance (QAS-L3, QAS-L4, QAS-L5, QAS-L14; +QAS-L20-FTP):**
- [ ] Connect грузит хвост-окно (**1800 с** по умолчанию); `load-earlier` доходит до offset 0 ⇒ `reached_start=true`, весь файл в сегментном spool; приём/анализ — O(окно), не O(файл): пик `ru_maxrss` ≤ `RSS_ANALYSIS_MAX_MIB = 256` МиБ и ≤ `RSS_WINDOW_SENS_MIB = 32` МиБ.
- [ ] Упор в **включённый** клапан ⇒ `cap_hit=true` + `limit_bytes`; без тихой обрезки. При выключенном клапане `cap_hit` **не** выставляется.
- [ ] FTP без `SIZE`/`REST` → полный `RETR` стримится в spool; большой файл не даёт `_FtpTooLarge`; диапазоны нарезаются из файла.
- [ ] Смена суток: сегменты разных дней без потерь/дублей на границе.
- [ ] FTP-путь больше **не** отключает export/bundle (стык с LL-P2).

**Риски.** Разрастание числа сегментов (уборка по TTL/бюджету из LL-P1); fake-FTP фикстуры должны эмулировать отсутствие `SIZE`/`REST` и большой поток; граница суток по UTC.

**Верификация.** `cd tools/analyzer && python3 -m unittest discover -s tests -v`; `make test`. Реальный FTP — вне CI (ручной прогон).

**DoD фазы.** Acceptance зелёные; INV-L1/L3/L5 покрыты; `reached_start`/`bytes` семантика подтверждена тестом.

---

## LL-P4 — Frontend (`webui/index.html`): Blob-upload, серверный период + coarse + окна, FTP-UX, компактный «Период», sticky-тоггл
**Цель.** Прямой `Blob`-upload без `f.text()`; BOM/CRLF/мультифайл; период — **до/через** запрос (серверная нарезка), coarse + дозапрос окон; export/bundle через handle; FTP-окно UX (REQ-L6); компактный «Период» (REQ-L7); опциональный sticky-тоггл + `localStorage` (REQ-L8); обработка `log_too_large` (только при клапане).

**Файлы.** `tools/analyzer/webui/index.html`:
- `readAllFiles:1153-1158` → собрать **композитный `Blob`** из `File` (без `f.text()`); нормализация CRLF — **только** для line-splitting Blob-пути; **сохранить порядок `Blob`↔`files=`**.
- `fetchAnalyze:1160-1192` → `fetch(url,{body:blob})`; параметры `from`/`to`/`window`; вместо `state.body` (строка) — **handle**; читает ключи coarse-ответа (M1): **`data.handle`**, **`data.window`** (`{from,to}`), **`data.coarse`**, **`data.window_count`**, `data.series`/`data.client_series`/`data.host_series`, `data.digest`, `data.drops[].key`; `state.body`-реюз для `/api/export` (`:1205`)/`/api/bundle` (`:1239`) → handle.
- FTP: `state.body = null` (`:1528`) и отключение export/bundle (`:3692`,`:3708`) → export/bundle **включены** через handle.
- `buildPeriodPanel:1799` + `.panel-sticky`: **CSS-определение** `index.html:177` (`.panel-sticky { position: sticky; …`), **место применения** `:1863-1864`. **Компактный layout (H3): целевая высота sticky-блока ≤ `PERIOD_MAX_HEIGHT_PX = 96` px** (компактные от/до; пресеты одной линией/меню; глобальный выбор клиента — подстрокой), без потери функций.
- **Sticky-тоггл (REQ-L8):** новый ключ персиста по конвенции репозитория (`hldsmon.ui.*`, рядом с `COLLAPSED_KEY:1422`/`DATASETS_KEY:1443`, helper `:1380`); применяется без reload.
- **FTP-окно UX (REQ-L6):** явный размер окна — **пресеты {5 мин, 15 мин, 30 мин, 1 ч, 6 ч, 24 ч}** (= {300, 900, 1800, 3600, 21600, 86400} с; дефолт 30 мин), индикатор загруженного диапазона (время + `[start,end]`), очевидные «← раньше»/«позже →», **детерминированный прогресс-бар** (байты/проценты при известном `SIZE`, иначе индетерминированный; `role="progressbar"`), `reachedStart` (чтение `ftp.reached_start` `:1499`,`:1524`,`:1567`,`:1602`), видимый `cap_hit`; без скрытой тихой остановки.
- **Обработка `log_too_large`/413** (только при включённом клапане) — явное сообщение причины.
- `selfTest:4144` — добавить кейсы: компактный «Период», sticky-тоггл-персист, индикаторы FTP-окна.

**Границы.** Серверные контракты — LL-P1/P2/P3; продюсер — LL-P5; схема не трогается.

**Инварианты.** INV-L5 (UI-часть), INV-6; сервер остаётся stdlib-only.

**Acceptance (QAS-L16, QAS-L17, QAS-L18 — **вне `make test`**; QAS-L11-CRLF/порядок; QAS-L6-FTP):**
- [ ] **L16 (REQ-L6):** явный размер окна (пресеты {5 мин, 15 мин, 30 мин, 1 ч, 6 ч, 24 ч}; дефолт 30 мин); индикатор диапазона (время + `[start,end]`); кнопки «раньше/позже»; **детерминированный прогресс-бар**; `reached_start`; нет скрытой тихой остановки.
- [ ] **L17 (REQ-L7):** высота sticky-блока **≤ `PERIOD_MAX_HEIGHT_PX = 96` px** (компактный layout), функции (от/до, пресеты, выбор клиента) сохранены; `selfTest` измеряет `offsetHeight` контейнера.
- [ ] **L18 (REQ-L8):** закрепление можно выключить; состояние персистится в `localStorage` и восстанавливается после reload.
- [ ] **L11 (CRLF/порядок):** CRLF-нормализация + порядок мультифайлового `Blob` ↔ `files=` сохранены.
- [ ] Export/bundle доступны для FTP-пути (через handle).
- [ ] Обработка 413/`log_too_large` — только при включённом клапане; видимая причина.

**Метод проверки UI-QAS (обязателен, §28.8/M2).** QAS-L16/L17/L18 **не покрываются `make test`** (там C++ host-тесты + Python `unittest`; **JS-рендер `index.html` не покрыт — в `test_webui.py` есть лишь doctype smoke-test `test_get_index_html`, остальные ~58 тестов — JSON-API**). Механизм: **ручной QA-чеклист** (фиксируется в `docs/journal.md`) **и/или** прогон page-`selfTest` (`webui/index.html:4144`) с новыми кейсами (компактный период с проверкой `offsetHeight ≤ 96` px, sticky-тоггл-персист, индикаторы FTP-окна). Python-статическая проверка допустима только для реально проверяемого (напр. наличие ключа тоггла в HTML/JS) — добавляется в `test_webui.py`.

**Риски.** Расхождение серверного/клиентского периодов (эталон — серверная нарезка); регресс экспорта для FTP (проверить handle); порог высоты `PERIOD_MAX_HEIGHT_PX=96` измерим (`selfTest`), фиксируется в QA-чеклисте.

**Верификация.** `cd tools/analyzer && python3 -m unittest discover -s tests -v` (Python + sanity `test_webui`); `make test`; **ручной QA-чеклист** + прогон `selfTest` в браузере (для L16/L17/L18).

**DoD фазы.** L16/L17/L18 подтверждены ручным QA/`selfTest` (запись в журнале); L11/L6 зелёные; JS-строка на 700 МБ не строится (приём — `Blob`).

---

## LL-P5 — Продюсер: `hldsmon_max_mb` default `0`=unlimited
**Цель.** Снять размерный cap записи по умолчанию (опциональный клапан), синхронизировать `README`/design; `log_writer.cpp` **не трогается** (`0` уже =unlimited).

**Файлы.**
- `src/bootstrap.cpp`: default-строка `s_val_max_mb[]` `"512"`→`"0"` (**`:55`**); рантайм-fallback `cvar_f(c.cvars.max_mb, 512.0f)`→`0.0f` (**`:308`**). Регистрация cvar (**`:124`**) — **без изменений**.
- `README.md`: cvar-таблица — `hldsmon_max_mb` default **`0`** (unlimited), диапазон «`0` — без лимита»; риск disk-fill отмечен.
- `tests/test_log_writer.cpp`: тест семантики **`0` = unlimited** (запись не останавливается по размеру).
- **Верификация default-строки (M2):** `bootstrap.cpp` не компилируется host-целями (`make test`), поэтому дефолт проверяется **вне** `test_log_writer`: **(a)** статическая проверка исходника — `grep -Eq 's_val_max_mb\[\][[:space:]]*=[[:space:]]*"0"' src/bootstrap.cpp` **и** `grep -q 'c.cvars.max_mb, 0.0f' src/bootstrap.cpp`; **(b)** **ручной** прогон на тестовом ReHLDS-сервере: на свежем `FCVAR_ARCHIVE`-профиле `hldsmon_max_mb` = `0` (консоль/`hldsmon_status`), запись не останавливается по размеру. Оба результата — в `docs/journal.md`.
- `src/log_writer.cpp` — **НЕ трогается** (`:184` гейт только при `max_bytes_ != 0`; `:225` `0`=unlimited).
- `docs/design.md` §7/§12 — **проверить** (уже обновлены в §28); правок по возможности нет.

**Границы.** Никакого другого плагинного кода; `hldsmon_retention_days` не трогать; схема P1/ABI неизменны.

**Инварианты.** INV-2/INV-5 не затронуты; семантика продюсера — REQ-L4.

**Acceptance (QAS-L19):**
- [ ] `hldsmon_max_mb=0` (default) ⇒ запись **не** останавливается по размеру; non-zero клапан по-прежнему ограничивает.
- [ ] **Default покрыт (M2):** `test_log_writer.cpp` проверяет `set_max_mb(0)`=unlimited **и** статическая проверка `bootstrap.cpp` (default-строка `"0"` + fallback `0.0f`) проходит; ручной `hldsmon_status` на тестовом сервере зафиксирован в `docs/journal.md`.
- [ ] `README.md` cvar-таблица отражает `0`=unlimited; `docs/design.md §7/§12` согласованы.
- [ ] `static_assert`/размеры структур не менялись.

**Риски.** disk-fill при выключенном клапане — **осознанное решение владельца** (REQ-L4/§28.3 п.9), задокументировано; митигация — опциональный клапан.

**Верификация.** `make`; `make test` (в т.ч. `test_log_writer`); статическая проверка `bootstrap.cpp` (grep, M2); ручной `hldsmon_status` (запись в journal).

**DoD фазы.** `make`/`make test` зелёные; cvar-таблица и design §7/§12 согласованы; `log_writer.cpp` не изменён.

---

## LL-P6 — Верификация, документация, журнал
**Цель.** Финальная сквозная верификация стадии; запись журнала; журнал/индексы; фиксация UI-QA.

**Файлы.** `docs/journal.md` (запись стадии `large-logs`: что сделано, UI-QA-результат, коммит-статус); `README.md` (раздел анализатора/веб-UI — обновить описание безлимитного приёма/окон, если нужно); сверка `docs/design.md §7/§12`; индексы (`AGENTS.md` — уже указывает на `docs/plan.md`).

**Acceptance (QAS-L15):**
- [ ] **stdlib-only** сохранён; схема P1/`KNOWN_TYPES` **без изменений**; `exports.whitelist` без изменений.
- [ ] Все существующие Python-тесты зелёные + новые; `make`/`make test` зелёные.
- [ ] Каждый QAS-L1..L20 прослеживается к тесту/методу (карта выше); L16/L17/L18 — ручной QA/`selfTest` записан.
- [ ] `docs/journal.md` заполнен. **Коммит** — только по явной просьбе владельца.

**Верификация.** `make`; `make test`; `cd tools/analyzer && python3 -m unittest discover -s tests -v`.

**DoD фазы.** Все артефакты стадии на месте; журнал закрыт.

---

## Definition of Done (стадия `large-logs`)
- [ ] **REQ-L1** (чтение без лимита по умолчанию, local+FTP) — LL-P1/P3, QAS-L2/L20.
- [ ] **REQ-L2** (память ограничена стримингом) — LL-P1/P2, QAS-L1/L10.
- [ ] **REQ-L3** (опц. клапан `--max-log-mb` default 0) — LL-P1, QAS-L2.
- [ ] **REQ-L4** (продюсер `hldsmon_max_mb=0`) — LL-P5, QAS-L19.
- [ ] **REQ-L5** (window — UX, не потолок) — LL-P2, QAS-L13.
- [ ] **REQ-L6** (FTP-окно UX) — LL-P3/P4, QAS-L16 (ручной QA/`selfTest`).
- [ ] **REQ-L7** (компактный «Период») — LL-P4, QAS-L17 (ручной QA/`selfTest`).
- [ ] **REQ-L8** (sticky-тоггл + `localStorage`) — LL-P4, QAS-L18 (ручной QA/`selfTest`).
- [ ] **INV-L1..L5** покрыты (карта выше); INV-5/INV-6 сохранены; `iter_records`/`log_writer.cpp` не тронуты.
- [ ] QAS-L1..L20: каждая прослеживается к фазе/методу; **L16/L17/L18 вне `make test`** — подтверждены ручным QA-чеклистом + `selfTest` (`index.html:4144`), запись в журнале.
- [ ] `make` и `make test` зелёные; `cd tools/analyzer && python3 -m unittest discover -s tests -v` зелёный.
- [ ] JSONL-схема P1/`KNOWN_TYPES`/`exports.whitelist`/Makefile не изменены; stdlib-only.
- [ ] `docs/journal.md` заполнен; `README.md` cvar-таблица и раздел веб-UI согласованы.
- [ ] Коммит — только по явной просьбе владельца (один связный коммит стадии).

## Риски и откат (стадия)
| Риск | Митигация | Откат |
|---|---|---|
| **Disk-fill** при выключенном клапане (spool и лог) | Осознанное решение владельца (REQ-L4/§28.3 п.9); spool per-session/process-бюджет + free-space pre-check + `spool_full`; loopback-дефолт; опц. клапаны `--max-log-mb`/`hldsmon_max_mb` | Вернуть `hldsmon_max_mb` default `512` (`bootstrap.cpp:55`/`:308`); задать `--max-log-mb >0` |
| **spool на tmpfs (=RAM)** | fstype-resolver `spool_backing_kind` + fallback на disk-backed каталог; QAS-L8 | `--spool-dir` явно на диск |
| **Производительность оконного анализа** (случайная материализация → O(records)) | Стриминг-агрегатор (ii-b) без полного `records`; структурный тест ответа + RSS-тест (QAS-L10); окно ≤ записей окна (QAS-L13) | Уменьшить default-окно; откатить P2 |
| **UI-верификация** (L16/L17/L18 вне `make test`) | Ручной QA-чеклист + `selfTest` (`index.html:4144`); Python-статика только по реально проверяемому (наличие ключа тоггла) | Ручной QA-проход перед приёмкой |
| **FTP-путь** (нет `SIZE`/`REST`; смена суток) | Fallback `RETR`→spool; сегменты по дням (QAS-L5/L14); fake-FTP тесты | Ручной прогон на реальном FTP |
| **Совместимость контрактов** | Аддитивность: малый файл → одно окно; body-путь сохранён; `session_expired` при вытеснении (QAS-L6) | Откат фронта (P4) к прежнему upload-потоку |

## Раздел ревью плана (momus) — стадия `large-logs`
**Ревьюер:** `momus`. **Вердикт: PASS-WITH-REVISIONS — блокеров нет; plan-gate ЗАКРЫТ после сложения H1–H3/M1–M4 (L1/L2 — опциональные, сложены); implementation-gate ОТКРЫТ.**

| ID | Severity | Замечание | Disposition |
|---|---|---|---|
| H1 | High | Числовые дефолты resource-guard LL-P1 не заданы | **Внесено** — таблица флагов/ENV + LL-P1: `MAX_UPLOADS=4` (429 `busy`), `READ_TIMEOUT_S=30.0` (408 `request_timeout`), `SPOOL_SESSION_MB=2048`, `SPOOL_TOTAL_MB=8192`, `SPOOL_TTL_S=3600`; все overridable flag/env; ссылки в acceptance LL-P1. |
| H2 | High | Пороги RSS — placeholder | **Внесено** — измеримые критерии: `RSS_SLOPE_MAX_MIB=64` (QAS-L1/L20, удвоение входа), `RSS_ANALYSIS_MAX_MIB=256` + `RSS_WINDOW_SENS_MIB=32` (QAS-L3/L10, 30-мин vs 5-мин окно); в карте QAS и acceptance фаз. |
| H3 | High | UX-пороги делегированы плану (design §28.9(7)), но не заданы | **Внесено** — sticky «Период» ≤ `PERIOD_MAX_HEIGHT_PX=96` px (проверяется `selfTest` через `offsetHeight`); FTP-окно: дефолт 30 мин, пресеты {5м,15м,30м,1ч,6ч,24ч} = {300,900,1800,3600,21600,86400} с; прогресс — детерминированный бар (`role="progressbar"`, байты при известном `SIZE`, иначе индетерминированный). QAS-L16/L17 чек-листы привязаны к числам. |
| M1 | Med | Ключи coarse-ответа `/api/analyze` не зафиксированы | **Внесено** — «Контракт coarse-ответа (M1)» в LL-P2: `handle`, `window{from,to}`, `coarse`, `window_count`, агрегаты (`digest`/`clients`/`sessions`/`session_count`/`client_identities`/`drops` c `key {run,t,userid}`/`anomalies`/`anomalies_excluded`/`events`/`log_end`/`config`/`records_count`/`host_caveats`), даунсэмпл-ряды `series`/`client_series`/`host_series` (≤ `SERIES_MAX_POINTS=2000`), `focus_points`; `records` никогда. LL-P4 ссылается на те же ключи. |
| M2 | Med | QAS-L19 «default» не покрыт (`test_log_writer` покрывает лишь `set_max_mb(0)`) | **Внесено** — LL-P5: статическая проверка `bootstrap.cpp` (grep default-строки `"0"` + fallback `0.0f`) + ручной `hldsmon_status` на тестовом сервере; оба — в journal. |
| M3 | Med | «стрим до EOF или 411» оставлено исполнителю | **Внесено** — детерминированная семантика LL-P1: отсутствие `Content-Length` → **411** `length_required`; present-but-malformed → **стрим до EOF** (`Connection: close`, read-timeout → 408); valid >0 → ровно N байт (недобор → 408). |
| M4 | Med | Семантика `--allow-nonloopback-unlimited` неоднозначна | **Внесено** — LL-P1/таблица флагов: non-loopback + `--max-log-mb=0` без флага ⇒ **403** `unlimited_nonloopback_disabled` (startup-warning); с флагом или при `>0` ⇒ 200; loopback ⇒ всегда 200. QAS-L7 тестирует обе ветки. |
| L1 | Low | Anchor `.panel-sticky (:1863-1864)` — место применения; определение в `:177` | **Внесено** — LL-P4 цитирует **и** CSS-определение `index.html:177`, **и** место применения `:1863-1864`. |
| L2 | Low | Фраза про `test_webui.py` неточна | **Внесено** — «JS-рендер `index.html` не покрыт; есть лишь doctype smoke-test `test_get_index_html`, остальные ~58 тестов — JSON-API». |

**Fine (не менять):** scope `tools/analyzer` + плагинный дефолт `hldsmon_max_mb`; схема P1/`KNOWN_TYPES`/`exports.whitelist` без изменений; stdlib-only; `iter_records`/`log_writer.cpp` не трогаются; фазовая декомпозиция и карта REQ-L/QAS-L/INV-L.

**Гейт:** plan-review пройден (PASS-WITH-REVISIONS; все H/M и опциональные L1/L2 сложены) — **plan-gate ЗАКРЫТ; implementation-gate ОТКРЫТ**. Коммит — только по явной просьбе владельца.
