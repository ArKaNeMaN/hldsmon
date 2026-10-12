# tools/analyzer — офлайн-анализатор телеметрии HldsMonitoring

Парсер и агрегатор дневных JSONL-логов плагина (каноническая схема — `docs/plan.md`, Приложение P1).
Работает **офлайн** над файлами, которые владелец забирает с сервера по FTP.

## Ограничения

- Только стандартная библиотека Python 3 (argparse, json, csv, datetime, collections, statistics). Без pip и без сети.
- **Не изменяет** входные файлы и ничего не пишет на диск (кроме stdout).
- Устойчив к частично записанным/повреждённым строкам: пустые строки, невалидный JSON (в т.ч. обрезанная последняя строка), не-объекты и неизвестные `type` молча пропускаются.

## Запуск

```bash
cd tools/analyzer
python3 hldsmon.py <subcommand> FILE [FILE...] [опции]
```

Модуль также импортируем:

```python
from hldsmon import iter_records, parse_records

records = parse_records(["hldsmon-2026-10-09.jsonl"])   # список записей
for rec in iter_records("hldsmon-2026-10-09.jsonl"):     # потоково
    ...
```

## Подкоманды

| Команда | Назначение |
|---|---|
| `summary FILE...` | Период покрытия, счётчики по типам, игроки, топ причин дропов, число hitch-строк, число всплесков лага (кадры >100 ms), строка `modnet` (число записей и модули), строка `config` (число записей), строка `host` (число записей). |
| `drops FILE...` | Список drop-событий. Фильтры: `--reason` (подстрока по reason/category, регистронезависимо), `--client ID`, `--since`, `--until` (ISO-8601 или epoch-секунды, включительно). |
| `window FILE --around TS --secs N` | Все записи в окне ±N секунд вокруг `TS` — для скармливания агенту. |
| `clients FILE` | Per-client агрегаты: число сэмплов, avg/max ping, avg/max loss, avg/max choke, max rel_fill. |
| `anomalies FILE` | Эвристики: массовые дропы (≥3 дропа за 5 с), серии hitch (≥3 за 10 с), рост reliable-fill, всплески loss. |
| `digest FILE` | Компактный машинно-читаемый JSON-дайджест (сортированные ключи, детерминированный), включая блок `modnet` (Σ in/out bytes, `conn_fail`, per-module top), блок `config` (число записей и последний снимок; `null` — cvar отсутствует) и блок `host` (последний снимок; `null` — источник недоступен или Δ без базовой точки (первый сэмпл); `caveats` — имена сработавших оговорок). |
| `export-csv FILE [--type TYPE]` | Выгрузка записей в CSV на stdout; вложенные объекты сериализуются в JSON-строку. |

### Примеры

```bash
python3 hldsmon.py summary hldsmon-2026-10-09.jsonl
python3 hldsmon.py drops hldsmon-2026-10-09.jsonl --reason timeout --since 2026-10-09T18:00:00Z
python3 hldsmon.py drops hldsmon-2026-10-09.jsonl --client 7 --since 1712345670 --until 1712345690
python3 hldsmon.py window hldsmon-2026-10-09.jsonl --around 1712345679 --secs 5
python3 hldsmon.py clients hldsmon-2026-10-09.jsonl
python3 hldsmon.py anomalies hldsmon-2026-10-09.jsonl
python3 hldsmon.py digest hldsmon-2026-10-09.jsonl
python3 hldsmon.py export-csv hldsmon-2026-10-09.jsonl --type drop > drops.csv
```

## Типы записей

`server`, `client`, `drop` (с вложенными `client`/`netchan`/`window`/`server`/`neighbors`),
`reject`, `connect`, `netcmd`, `massdrop`, `packet`, `hitch`, `mapchange`, `voice`,
`modnet` (per-`.so` трафик чужих модулей: `module`/`so`, Δ счётчиков и байт за интервал;
стадия `modnet`), `config` (контекст конфигурации: `rehlds_major`/`rehlds_minor`,
`map`, `maxclients` и числовой rate/timeout/tickrate-снимок; отсутствующий cvar —
`null`; стадия `config-context`), `host` (best-effort хост/ядро-метрики: Δ счётчиков
`udp_*`/`net_rx_*`/`io_*`, `steal_pct` %, `disk_bavail_mb`; `null` — источник
недоступен или Δ без базовой точки (первый сэмпл); `present_mask` — по полям,
`caveat_mask` — по источникам: значение присутствует, но помечено (общий netns,
драйверные NIC-счётчики и т.п.); стадия `host-metrics`), `degraded` (Δ ошибок
записи сбора) и `retention` (Δ удалённых файлов ротации; стадия `event-enrich`).
Полная схема полей — `docs/plan.md`, Приложение P1 и его расширения `modnet`/`host`.

Библиотечный API для modnet-рядов (ключ `(module, so, run)`; пропуск интервала = ноль):

```python
from hldsmon import modnet_series, modnet_summary, correlate_modnet

series = modnet_series(records)          # {"module|so|run": {t, conn_fail, ...}}
summary = modnet_summary(records)        # {module: Σ bytes/счётчиков, peak kbps}
corr = correlate_modnet(records, 5.0)    # всплески/conn_fail рядом с drop/hitch
```

Библиотечный API для host-рядов (параллельные списки по записям `host`):
`host_series(records)` возвращает `t` + все метрики полей + `present_mask`/
`caveat_mask`; недоступное значение — `None` (не ноль). Константы раскладки битов
масок — `HOST_PRESENT_BITS` / `HOST_CAVEAT_BITS` (зеркало `src/host_metrics.h`).
Карта field→источник оговорки — `HOST_FIELD_CAVEATS` (`udp_*→udp_netns`,
`net_rx_*→net_dev`, `steal_pct→steal`, `io_*→io`, `disk_bavail_mb→disk`),
биты по полям — `HOST_FIELD_CAVEAT_BITS`.

Библиотечный API для config-diff и incident bundle:

```python
from hldsmon import config_at, config_diff, config_series, incident_bundle, read_bundle

cfg = config_at(records, t)          # последний config с t' <= t (или None)
diff = config_diff(before, after)    # {changed, added, removed, baseline}
bundle = incident_bundle(records, drop, mins=2.0)   # JSON-конверт инцидента
again = read_bundle("bundle.json")   # записи, совместимые с iter_records
```

`config_diff` сравнивает `CONFIG_FIELDS` + `map`/`maxclients`/`rehlds_*`
(набор — `CONFIG_DIFF_FIELDS`); значение, присутствующее только на одной
стороне (`null`/пропуск), — установка (`added`) или снятие (`removed`), а не
`changed`. Без baseline `baseline=false` и все списки пусты (не «всё
изменилось»). `config_series` отдаёт снимки с `diff` к предыдущему.

`incident_bundle` возвращает конверт `{bundle_version, drop, window, config,
host, records, warnings}`: `drop` — как есть; сырые `records` окна включены
только когда дропа нет (дедуп: дроп уже несёт `window`), при
`include_records=True` — оба (без самого дропа). Размер ограничен `max_bytes`
(по умолчанию 8 МиБ): при превышении сбрасываются `records`, затем
обрезается `drop.window`, каждый шаг — в `warnings`. `read_bundle`
разворачивает конверт (путь/файл/словарь) обратно в записи.

## Веб-интерфейс

`python3 webui.py` (или `make web`) отдаёт дашборд и локальный JSON-API.
Помимо прежних ключей, `POST /api/analyze` возвращает `host_series` (ряды
`host`; `None` — недоступно/Δ без базы), `host_caveats`
(`field → {source, bit}` для маркеров caveated-точек) и `config` (снимки
config-записей с `diff`). `POST /api/bundle?drop=<индекс>&mins=<мин>`
возвращает JSON-конверт инцидента; `/api/export` остаётся CSV-only.

UI: общий график «Все датасеты» накладывает ряды `host` (три состояния точки:
`null` — разрыв, установленный бит caveat — кольцо-маркер, иначе норма; `null`
никогда не рисуется нулём), панель «Конфигурация» показывает снимки и diff
(`изменено`/`установлено`/`снято`), кнопка `bundle` у панели drop скачивает
конверт (полуокно ±N мин). Набор датасетов (включая host-ключи) сохраняется в
`localStorage`; старые сохранения совместимы.

## Тесты

```bash
cd tools/analyzer
python3 -m unittest discover -s tests -v
```

Фикстуры в `tests/fixtures/` покрывают все типы записей P1 (включая расширения
`modnet`/`host`), вложенные срезы drop, а также устойчивость парсера (обрезанную
последнюю строку, неизвестный `type`, пустые строки, мусорные строки):

- `types_ok.jsonl` — по одной записи каждого типа, включая полный nested drop;
- `modnet.jsonl` — `modnet` двух модулей (активные и пустые интервалы,
  `conn_fail`, `dns_calls`) плюс `drop`/`hitch` для корреляции;
- `host.jsonl` — `host`: полный снимок, первый сэмпл (все Δ — `null`),
  caveated-значение (`present` + `caveat`) и недоступный диск (`null`);
- `truncated.jsonl` — заканчивается намеренно обрезанной (невалидной) строкой;
- `dirty.jsonl` — пустые строки, неизвестный `type`, не-JSON строки, объект без `type`;
- `anomalies.jsonl` — данные, активирующие все четыре эвристики.
