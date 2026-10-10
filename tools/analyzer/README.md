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
| `summary FILE...` | Период покрытия, счётчики по типам, игроки, топ причин дропов, число hitch-строк, число всплесков лага (кадры >100 ms). |
| `drops FILE...` | Список drop-событий. Фильтры: `--reason` (подстрока по reason/category, регистронезависимо), `--client ID`, `--since`, `--until` (ISO-8601 или epoch-секунды, включительно). |
| `window FILE --around TS --secs N` | Все записи в окне ±N секунд вокруг `TS` — для скармливания агенту. |
| `clients FILE` | Per-client агрегаты: число сэмплов, avg/max ping, avg/max loss, avg/max choke, max rel_fill. |
| `anomalies FILE` | Эвристики: массовые дропы (≥3 дропа за 5 с), серии hitch (≥3 за 10 с), рост reliable-fill, всплески loss. |
| `digest FILE` | Компактный машинно-читаемый JSON-дайджест (сортированные ключи, детерминированный). |
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
`reject`, `connect`, `netcmd`, `massdrop`, `packet`, `hitch`, `mapchange`, `voice`.
Полная схема полей — `docs/plan.md`, Приложение P1.

## Тесты

```bash
cd tools/analyzer
python3 -m unittest discover -s tests -v
```

Фикстуры в `tests/fixtures/` покрывают все типы записей P1, вложенные срезы drop,
а также устойчивость парсера (обрезанную последнюю строку, неизвестный `type`,
пустые строки, мусорные строки):

- `types_ok.jsonl` — по одной записи каждого типа, включая полный nested drop;
- `truncated.jsonl` — заканчивается намеренно обрезанной (невалидной) строкой;
- `dirty.jsonl` — пустые строки, неизвестный `type`, не-JSON строки, объект без `type`;
- `anomalies.jsonl` — данные, активирующие все четыре эвристики.
