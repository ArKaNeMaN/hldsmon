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
          players, real, proxy, fake, in_kbps, out_kbps, edicts, map, uptime_s, fps, maxclients}
client:  {type, t, id, gen, ping, loss, dloss, choke, dchoke, delta_seq, in_seq, in_ack, out_seq,
          rel_seq, rel_fill, rate, cleartime, updaterate, cmdrate, state, fragbuf_total,
          incomingready_mask, flow_in_kbps, flow_out_kbps, flow_in_avg, flow_out_avg, msg_flags,
          idle_ms, name, authid, steamid64, ip, port, userid, slot, conn_s, fake, proxy}
drop:    {type, t, id, gen, reason, category, crash, observed_via,
          name, authid, steamid64, ip, port, userid, slot, conn_s, fake, proxy,
          client:{active,spawned,fully_connected,connected,proxy,fake,userid,connecttime,cmdtime,connection_started},
          netchan:{in_seq,in_ack,in_rel_ack,in_rel_seq,out_seq,rel_seq,last_rel_seq,rel_len,rel_fill,rate,cleartime,msg_flags,last_received,connect_time,now_real,idle_ms,sv_timeout},
          window:[{client-sample compact}], server:{server-sample}, neighbors:{count,avg_loss,avg_ping,max_loss}}
reject:  {type, t, ip, steamid, stage, detail}
connect: {type, t, id, ip, steamid, name}
netcmd:  {type, t, id, cmd, count}
massdrop:{type, t, window_s, count, reason, category}
packet:  {type, t, pps, bytes, sizes:{lt128,lt512,lt1024,ge1024}, unique_ips, top_ip}
hitch:   {type, t, duration_ms}
mapchange:{type, t, map}
voice:   {type, t, id, last_voice_time}
shutdown:{type, t, reason}
crash:   {type, t, signal}
```

Все числовые значения — неотрицательные; отсутствующие — `null`; строки JSON-экранируются. Поля идентичности (`name`/`authid`/`steamid64`/`ip`) — `""`, `port`/`userid` — `-1`, `conn_s` — `0` при отсутствии данных; `fake`/`proxy` — булевы.

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
