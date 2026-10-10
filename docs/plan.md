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
