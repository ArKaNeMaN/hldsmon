#!/usr/bin/env python3
"""HldsMonitoring offline JSONL analyzer.

Reads the plugin's daily telemetry files (schema: ``docs/plan.md`` Appendix P1)
and offers aggregation/query subcommands for offline diagnosis.

Design constraints (plan Phase 5):
* standard library only (no third-party packages, no network);
* never modifies input files;
* tolerant streaming parser: blank lines, malformed JSON (including a
  truncated final line), non-object values and unknown ``type`` values are
  skipped without raising.

Importable API::

    from hldsmon import iter_records, parse_records

Command line::

    python3 hldsmon.py summary hldsmon-2026-10-09.jsonl
    python3 hldsmon.py drops hldsmon-2026-10-09.jsonl --reason timeout
    python3 hldsmon.py window hldsmon-2026-10-09.jsonl --around 1712345679 --secs 5
    python3 hldsmon.py clients hldsmon-2026-10-09.jsonl
    python3 hldsmon.py anomalies hldsmon-2026-10-09.jsonl
    python3 hldsmon.py digest hldsmon-2026-10-09.jsonl
    python3 hldsmon.py export-csv hldsmon-2026-10-09.jsonl --type drop
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from statistics import mean

_ISO_FRACTION = re.compile(r"\.(\d+)")

# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

#: Canonical record types (docs/plan.md, Appendix P1).
KNOWN_TYPES = (
    "server",
    "client",
    "drop",
    "reject",
    "connect",
    "netcmd",
    "massdrop",
    "packet",
    "hitch",
    "mapchange",
    "voice",
    "shutdown",
    "crash",
)
_KNOWN_TYPES = frozenset(KNOWN_TYPES)

#: Slide thresholds used by the anomaly heuristics.
MASS_DROP_COUNT = 3
MASS_DROP_WINDOW_S = 5.0
HITCH_CLUSTER_COUNT = 3
HITCH_CLUSTER_WINDOW_S = 10.0
REL_FILL_GROWTH = 25.0
REL_FILL_HIGH = 80.0
LOSS_SPIKE_ABS = 20.0
LOSS_SPIKE_DELTA = 15.0


# ---------------------------------------------------------------------------
# Parsing (tolerant, streaming)
# ---------------------------------------------------------------------------

def iter_records(path):
    """Yield valid records from a single JSONL file, line by line.

    Blank lines, invalid JSON (including a truncated final line), JSON values
    that are not objects, and records whose ``type`` is missing/unknown are
    silently skipped. This function never raises on malformed content.

    File-open errors (e.g. missing file) still propagate to the caller.
    """
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except (ValueError, TypeError):
                # Truncated tail / corrupt line: ignore and continue.
                continue
            if not isinstance(obj, dict):
                continue
            rtype = obj.get("type")
            if not isinstance(rtype, str) or rtype not in _KNOWN_TYPES:
                continue
            yield obj


def parse_records(paths):
    """Return a list of all valid records across ``paths``.

    ``paths`` may be a single path string or an iterable of paths; records are
    returned in file order, concatenated.
    """
    if isinstance(paths, (str, bytes)):
        paths = [paths]
    records = []
    for path in paths:
        records.extend(iter_records(path))
    return records


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _num(value):
    """Return ``value`` as a number, or ``None`` if it is not numeric.

    Booleans are deliberately rejected (``bool`` is an ``int`` subclass and is
    not meaningful as a telemetry metric).
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return value
    return None


def parse_ts(value):
    """Parse a timestamp as epoch seconds (int/float) or ISO-8601 (UTC).

    Raises ``ValueError`` for unparsable input.
    """
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        raise ValueError("empty timestamp")
    try:
        return float(text)
    except ValueError:
        pass
    iso = text
    if iso.endswith("Z") or iso.endswith("z"):
        iso = iso[:-1] + "+00:00"
    # datetime.fromisoformat on Python <3.11 only accepts 3- or 6-digit
    # fractional seconds; normalize any other width to 6 digits.
    match = _ISO_FRACTION.search(iso)
    if match:
        frac = (match.group(1) + "000000")[:6]
        iso = iso[:match.start(1)] + frac + iso[match.end(1):]
    try:
        dt = datetime.fromisoformat(iso)
    except ValueError as exc:  # pragma: no cover - message passthrough
        raise ValueError("unrecognized timestamp: %r" % (value,)) from exc
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


def fmt_ts(value):
    """Format epoch seconds as an ISO-8601 UTC string (empty for ``None``)."""
    t = _num(value)
    if t is None:
        return ""
    try:
        return datetime.fromtimestamp(t, tz=timezone.utc).isoformat()
    except (OverflowError, OSError, ValueError):
        return str(t)


def _sorted_counts(counter, limit=None):
    """Return ``[(key, count), ...]`` sorted by count desc then key asc."""
    items = sorted(counter.items(), key=lambda kv: (-kv[1], str(kv[0])))
    if limit is not None:
        items = items[:limit]
    return items


def _record_ts(rec):
    return _num(rec.get("t"))


# ---------------------------------------------------------------------------
# Run segmentation and connection keys
# ---------------------------------------------------------------------------

#: Authids that do not identify a real player (used by :func:`player_key`).
NON_IDENTIFYING_AUTHIDS = frozenset((
    "", "BOT", "STEAM_ID_LAN", "STEAM_ID_PENDING", "VALVE_ID_LAN", "UNKNOWN",
))


def _attach_runs(records):
    """Return ``[(record, run_index), ...]`` in file order.

    ``run_index`` starts at 0 and is incremented *after* a ``shutdown`` or
    ``crash`` record (so those records still belong to the run they ended); a
    ``mapchange`` does not end a run.
    """
    out = []
    run = 0
    for rec in records:
        out.append((rec, run))
        if rec.get("type") in ("shutdown", "crash"):
            run += 1
    return out


def _int_userid(value):
    """Return ``value`` as a non-negative int, or ``None`` when unusable."""
    number = _num(value)
    if number is None or not float(number).is_integer():
        return None
    number = int(number)
    return number if number >= 0 else None


def session_key(rec, run):
    """Return the per-connection session key for ``rec`` in ``run``.

    ``"<run>:u<userid>"`` when ``userid`` is a non-negative integer, otherwise
    ``"<run>:i<id>:g<gen>"`` (slot + generation). ``None`` when neither a usable
    ``userid`` nor an ``id`` is present.
    """
    userid = _int_userid(rec.get("userid"))
    if userid is not None:
        return "%d:u%d" % (int(run), userid)
    cid = _num(rec.get("id"))
    if cid is None:
        return None
    return "%d:i%d:g%s" % (int(run), int(cid), rec.get("gen"))


def player_key(identity):
    """Return a cross-run player key for an identity dict.

    ``steamid64`` when non-empty; else ``authid`` when non-empty and not one of
    :data:`NON_IDENTIFYING_AUTHIDS`; else the entry's own ``session`` key.
    """
    if not isinstance(identity, dict):
        return None
    steamid64 = identity.get("steamid64")
    if isinstance(steamid64, str) and steamid64 != "":
        return steamid64
    authid = identity.get("authid")
    if (isinstance(authid, str) and authid != ""
            and authid not in NON_IDENTIFYING_AUTHIDS):
        return authid
    session = identity.get("session")
    if isinstance(session, str) and session != "":
        return session
    return None


# ---------------------------------------------------------------------------
# Log end state and exclusion windows
# ---------------------------------------------------------------------------

def _terminal_event(records):
    """Return ``(t, kind)`` for the latest shutdown/crash event, or ``None``.

    ``kind`` is ``"shutdown"`` or ``"crash"``; ``t`` is the event timestamp, or
    ``None`` when the only terminal records carry no usable timestamp. Ties on
    ``t`` are broken by file order (last wins) so the result is deterministic.
    """
    best = None  # (t, index, kind)
    last_untimed = None  # kind of the last terminal record lacking a timestamp
    for index, rec in enumerate(records):
        kind = rec.get("type")
        if kind not in ("shutdown", "crash"):
            continue
        t = _record_ts(rec)
        if t is None:
            last_untimed = kind
            continue
        if best is None or (t, index) >= (best[0], best[1]):
            best = (t, index, kind)
    if best is not None:
        return (best[0], best[2])
    if last_untimed is not None:
        return (None, last_untimed)
    return None


def log_end_state(records):
    """Return how the log ended: ``"graceful"``, ``"crash"`` or ``"unknown"``.

    The terminal event is the shutdown/crash record with the greatest ``t``; a
    ``shutdown`` terminal is ``"graceful"``, a ``crash`` terminal is ``"crash"``.
    ``"unknown"`` when neither record type is present.
    """
    event = _terminal_event(records)
    if event is None:
        return "unknown"
    _, kind = event
    return "graceful" if kind == "shutdown" else "crash"


def exclusion_windows(records, map_pre=5.0, map_post=15.0,
                      end_window=15.0, warmup=10.0):
    """Return windows during which anomalies should not be reported.

    Each window is ``{"kind", "start", "end", "reason"}``. Windows cover:
    a startup warmup, every map change, and the terminal shutdown/crash. The
    list is sorted by ``start`` (ties broken deterministically); overlapping
    windows are returned as-is, never merged.
    """
    windows = []

    # Warmup: the first ``warmup`` seconds after the earliest timestamp.
    times = [t for t in (_record_ts(r) for r in records) if t is not None]
    if times:
        first_t = float(min(times))
        windows.append({
            "kind": "warmup",
            "start": first_t,
            "end": first_t + float(warmup),
            "reason": "warmup",
        })

    # Map changes: a small window on either side of the event.
    for rec in records:
        if rec.get("type") != "mapchange":
            continue
        t = _record_ts(rec)
        if t is None:
            continue
        map_name = rec.get("map")
        if not isinstance(map_name, str):
            map_name = "" if map_name is None else str(map_name)
        windows.append({
            "kind": "mapchange",
            "start": float(t) - float(map_pre),
            "end": float(t) + float(map_post),
            "reason": map_name,
        })

    # Terminal shutdown/crash: the window leading up to it.
    event = _terminal_event(records)
    if event is not None and event[0] is not None:
        t, kind = event
        start = float(t) - float(end_window)
        if start < 0.0:
            start = 0.0
        windows.append({
            "kind": kind,
            "start": start,
            "end": float(t),
            "reason": "graceful" if kind == "shutdown" else "crash",
        })

    windows.sort(key=lambda w: (w["start"], w["end"], w["kind"], w["reason"]))
    return windows


# ---------------------------------------------------------------------------
# Aggregation primitives
# ---------------------------------------------------------------------------

def counts_by_type(records):
    return Counter(r.get("type") for r in records)


def players_seen(records):
    """Set of client ids observed across id-bearing records."""
    ids = set()
    for rec in records:
        cid = _num(rec.get("id"))
        if cid is not None:
            ids.add(int(cid))
    return ids


def drop_reason_counts(records):
    counter = Counter()
    for rec in records:
        if rec.get("type") == "drop":
            reason = rec.get("reason")
            counter[str(reason) if reason not in (None, "") else "(unknown)"] += 1
    return counter


def period(records):
    """Return ``(start, end)`` epoch seconds or ``(None, None)``."""
    times = [t for t in (_record_ts(r) for r in records) if t is not None]
    if not times:
        return (None, None)
    return (min(times), max(times))


def lag_spike_count(records):
    """Count server samples reporting a major hitch (``hitch.gt100 > 0``)."""
    spikes = 0
    for rec in records:
        if rec.get("type") != "server":
            continue
        hitch = rec.get("hitch")
        if isinstance(hitch, dict):
            gt100 = _num(hitch.get("gt100"))
            if gt100 is not None and gt100 > 0:
                spikes += 1
    return spikes


# ---------------------------------------------------------------------------
# Client aggregation
# ---------------------------------------------------------------------------

def aggregate_clients(records):
    """Return ``{session_key: stats}`` from ``client`` records.

    Keyed by :func:`session_key` (per-connection, run-scoped). Each entry also
    carries ``session``/``run``/``userid``/``id``/``slot``/``steamid64``/
    ``authid``/``name`` plus samples, ping/loss/choke avg+max, rel_fill max and
    the identity bounds ``first_t``/``last_t``.
    """
    identities = client_identities(records)
    acc = defaultdict(lambda: {
        "ping": [], "loss": [], "choke": [], "rel_fill": [],
        "samples": 0,
    })
    for rec, run in _attach_runs(records):
        if rec.get("type") != "client":
            continue
        skey = session_key(rec, run)
        if skey is None:
            continue
        bucket = acc[skey]
        bucket["samples"] += 1
        for name in ("ping", "loss", "choke", "rel_fill"):
            val = _num(rec.get(name))
            if val is not None:
                bucket[name].append(val)

    result = {}
    for skey, bucket in acc.items():
        ident = identities.get(skey, {})

        def _avg(name):
            vals = bucket[name]
            return mean(vals) if vals else None

        def _max(name):
            vals = bucket[name]
            return max(vals) if vals else None

        result[skey] = {
            "session": skey,
            "run": ident.get("run"),
            "userid": ident.get("userid"),
            "id": ident.get("id"),
            "slot": ident.get("slot"),
            "steamid64": ident.get("steamid64") or "",
            "authid": ident.get("authid") or "",
            "name": ident.get("name") or "",
            "ip": ident.get("ip") or "",
            "port": ident.get("port"),
            "conn_s": ident.get("conn_s"),
            "fake": ident.get("fake"),
            "proxy": ident.get("proxy"),
            "first_t": ident.get("first_t"),
            "last_t": ident.get("last_t"),
            "samples": bucket["samples"],
            "ping_avg": _avg("ping"),
            "ping_max": _max("ping"),
            "loss_avg": _avg("loss"),
            "loss_max": _max("loss"),
            "choke_avg": _avg("choke"),
            "choke_max": _max("choke"),
            "rel_fill_max": _max("rel_fill"),
        }
    return result


_SERIES_KEYS = ("t", "ping", "loss", "choke", "rel_fill", "dloss",
                "delta_seq", "idle_ms", "flow_in_kbps", "flow_out_kbps")


def client_series(records):
    """Return per-session parallel time series keyed by :func:`session_key`.

    Each entry holds
    ``t/ping/loss/choke/rel_fill/dloss/delta_seq/idle_ms/flow_in_kbps/flow_out_kbps``
    lists, one value per ``client`` record in file order; a missing or
    non-numeric value becomes ``None`` (JSON null).

    ``flow_in_kbps``/``flow_out_kbps`` are the per-client network rates in KB/s
    (``in`` = client->server upload, ``out`` = server->client download).

    ``idle_ms`` is the milliseconds since the client's last received packet at
    that sample (0/absent when the plugin did not report it); it lets a
    "Timed out" drop be read as a client that had gone silent beforehand.
    """
    series = {}
    for rec, run in _attach_runs(records):
        if rec.get("type") != "client":
            continue
        skey = session_key(rec, run)
        if skey is None:
            continue
        entry = series.get(skey)
        if entry is None:
            entry = {name: [] for name in _SERIES_KEYS}
            series[skey] = entry
        for name in _SERIES_KEYS:
            entry[name].append(_num(rec.get(name)))
    return series


_ID_STR_FIELDS = ("name", "authid", "steamid64", "ip")
_ID_NUM_FIELDS = ("port", "slot", "userid", "conn_s")
_ID_BOOL_FIELDS = ("fake", "proxy")


def _identity_str(value):
    """Return a non-empty string identity, or ``None`` to keep the prior one."""
    if isinstance(value, str):
        return value if value != "" else None
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return str(value)
    return None


def client_identities(records):
    """Return the last known identity per client session.

    Keyed by :func:`session_key` (``"<run>:u<userid>"`` or
    ``"<run>:i<id>:g<gen>"``). String fields default to ``""``, numeric fields
    to ``None``, ``fake``/``proxy`` to the last boolean seen (or ``None``).
    Each entry also carries ``session``, ``run``, ``userid`` (int or ``None``),
    ``id`` (last slot id), ``slot`` (last 1-based slot), ``samples`` counting
    the session's ``client`` records and ``first_t``/``last_t`` bounds.
    """
    identities = {}
    for rec, run in _attach_runs(records):
        if rec.get("type") != "client":
            continue
        skey = session_key(rec, run)
        if skey is None:
            continue
        entry = identities.get(skey)
        if entry is None:
            entry = {name: "" for name in _ID_STR_FIELDS}
            entry.update({name: None for name in _ID_NUM_FIELDS})
            entry.update({name: None for name in _ID_BOOL_FIELDS})
            entry.update({"first_t": None, "last_t": None, "samples": 0,
                          "session": skey, "run": run, "id": None})
            identities[skey] = entry
        entry["samples"] += 1
        for name in _ID_STR_FIELDS:
            text = _identity_str(rec.get(name))
            if text is not None:
                entry[name] = text
        for name in _ID_NUM_FIELDS:
            value = _num(rec.get(name))
            if value is not None:
                entry[name] = value
        cid = _num(rec.get("id"))
        if cid is not None:
            entry["id"] = int(cid)
        for name in _ID_BOOL_FIELDS:
            value = rec.get(name)
            if isinstance(value, bool):
                entry[name] = value
        t = _record_ts(rec)
        if t is not None:
            if entry["first_t"] is None or t < entry["first_t"]:
                entry["first_t"] = t
            if entry["last_t"] is None or t > entry["last_t"]:
                entry["last_t"] = t
    return identities


def player_groups(records):
    """Return ``{player_key: group}`` linking sessions of the same player.

    ``group`` carries ``player`` (the key), the last non-empty
    ``steamid64``/``authid``/``name``, the ``sessions`` list (sorted by their
    first timestamp) and the overall ``first_t``/``last_t``.
    """
    identities = client_identities(records)
    groups = {}
    for skey, ident in identities.items():
        pkey = player_key(ident)
        if pkey is None:
            continue
        group = groups.get(pkey)
        if group is None:
            group = {
                "player": pkey,
                "steamid64": "",
                "authid": "",
                "name": "",
                "sessions": [],
                "first_t": None,
                "last_t": None,
            }
            groups[pkey] = group
        for field in ("steamid64", "authid", "name"):
            value = ident.get(field)
            if isinstance(value, str) and value != "":
                group[field] = value
        group["sessions"].append(skey)
        first = ident.get("first_t")
        last = ident.get("last_t")
        if first is not None and (group["first_t"] is None
                                  or first < group["first_t"]):
            group["first_t"] = first
        if last is not None and (group["last_t"] is None
                                 or last > group["last_t"]):
            group["last_t"] = last
    for group in groups.values():
        group["sessions"].sort(key=lambda s: (
            identities[s].get("first_t")
            if identities[s].get("first_t") is not None else float("inf"),
            s,
        ))
    return groups


# ---------------------------------------------------------------------------
# Anomaly heuristics
# ---------------------------------------------------------------------------

def _cluster_times(times, gap, minimum):
    """Return clusters ``(start, end, count)`` where ``minimum`` timestamps lie
    within a sliding window of ``gap`` seconds. Non-overlapping greedy sweep.
    """
    times = sorted(times)
    clusters = []
    i = 0
    n = len(times)
    while i < n:
        j = i
        while j < n and times[j] - times[i] <= gap:
            j += 1
        count = j - i
        if count >= minimum:
            clusters.append((times[i], times[j - 1], count))
            i = j  # consume the whole cluster, avoids duplicates
        else:
            i += 1
    return clusters


def _collect_findings(records):
    """Return ``[(text, association), ...]`` in the historical finding order.

    ``association`` lists the times that triggered the finding: cluster
    intervals as ``(start, end)`` tuples, per-client findings as sample points.
    """
    findings = []

    # 1. Mass drops: >= 3 drops within 5 s.
    drop_times = [_record_ts(r) for r in records
                  if r.get("type") == "drop" and _record_ts(r) is not None]
    for start, end, count in _cluster_times(
            drop_times, MASS_DROP_WINDOW_S, MASS_DROP_COUNT):
        findings.append((
            "mass drops: %d drops between %s and %s (window %.1fs)"
            % (count, fmt_ts(start), fmt_ts(end), MASS_DROP_WINDOW_S),
            [(start, end)],
        ))

    # 2. Hitch clusters: >= 3 hitch records within 10 s.
    hitch_times = [_record_ts(r) for r in records
                   if r.get("type") == "hitch" and _record_ts(r) is not None]
    for start, end, count in _cluster_times(
            hitch_times, HITCH_CLUSTER_WINDOW_S, HITCH_CLUSTER_COUNT):
        findings.append((
            "hitch cluster: %d hitches between %s and %s (window %.1fs)"
            % (count, fmt_ts(start), fmt_ts(end), HITCH_CLUSTER_WINDOW_S),
            [(start, end)],
        ))

    # 3. Reliable-fill growth / saturation per client.
    fill = defaultdict(list)  # id -> [(t, rel_fill)]
    loss = defaultdict(list)  # id -> [(t, loss)]
    for rec in records:
        if rec.get("type") != "client":
            continue
        cid = _num(rec.get("id"))
        if cid is None:
            continue
        t = _record_ts(rec)
        rf = _num(rec.get("rel_fill"))
        if rf is not None:
            fill[int(cid)].append((t if t is not None else 0.0, rf))
        lv = _num(rec.get("loss"))
        if lv is not None:
            loss[int(cid)].append((t if t is not None else 0.0, lv))

    for cid in sorted(fill):
        series = sorted(fill[cid])
        values = [v for _, v in series]
        if not values:
            continue
        lo, hi = min(values), max(values)
        if hi >= REL_FILL_HIGH or (hi - lo) >= REL_FILL_GROWTH:
            lo_t = series[values.index(lo)][0]
            hi_t = series[values.index(hi)][0]
            findings.append((
                "reliable-fill: client %d grew %s -> %s (max %s)"
                % (cid, _short(lo), _short(hi), _short(hi)),
                [lo_t, hi_t],
            ))

    # 4. Loss spikes per client (absolute or sharp jump).
    for cid in sorted(loss):
        series = sorted(loss[cid])
        if not series:
            continue
        prev = None
        prev_t = None
        peak = None
        peak_t = None
        for t, lv in series:
            if peak is None or lv > peak:
                peak = lv
                peak_t = t
            if prev is not None and (lv - prev) >= LOSS_SPIKE_DELTA:
                findings.append((
                    "loss spike: client %d jumped %s -> %s"
                    % (cid, _short(prev), _short(lv)),
                    [prev_t, t],
                ))
            prev = lv
            prev_t = t
        if peak is not None and peak >= LOSS_SPIKE_ABS:
            findings.append((
                "loss spike: client %d peaked at %s"
                % (cid, _short(peak)),
                [peak_t if peak_t is not None else 0.0],
            ))

    return findings


def _covering_window(association, windows):
    """Return the first window whose interval covers any association time."""
    for item in association:
        if isinstance(item, tuple):
            start, end = item
        else:
            start = end = item
        for window in windows:
            if start <= window["end"] and window["start"] <= end:
                return window
    return None


def find_anomalies_detail(records, exclude=True):
    """Return ``{"findings": [...], "excluded": [...]}``.

    With ``exclude=True`` any finding whose associated time falls inside an
    :func:`exclusion_windows` window is moved to ``excluded``, each prefixed
    with the window kind (e.g. ``"mapchange: hitch cluster: ..."``).
    """
    collected = _collect_findings(records)
    if not exclude:
        return {"findings": [text for text, _ in collected], "excluded": []}

    windows = exclusion_windows(records)
    findings = []
    excluded = []
    for text, association in collected:
        window = _covering_window(association, windows)
        if window is None:
            findings.append(text)
        else:
            excluded.append("%s: %s" % (window["kind"], text))
    return {"findings": findings, "excluded": excluded}


def find_anomalies(records, exclude=True):
    """Return a list of human-readable anomaly strings (deterministic order).

    When ``exclude`` is true, findings inside shutdown/crash/mapchange/warmup
    exclusion windows are suppressed.
    """
    return find_anomalies_detail(records, exclude=exclude)["findings"]


def _short(value):
    """Compact numeric formatting for reports."""
    if value is None:
        return "?"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    if isinstance(value, float):
        return ("%.2f" % value).rstrip("0").rstrip(".")
    return str(value)


# ---------------------------------------------------------------------------
# Focus points (extensible noteworthy-moment detectors)
# ---------------------------------------------------------------------------

def focus_points(records, idle_min_s=5.0, idle_growth_ms=2000.0):
    """Return noteworthy moments in ``records`` as an extensible list of dicts.

    Every entry carries a ``kind`` discriminator, so future detectors can append
    entries with a different ``kind`` without changing the shape callers read.

    The first detector, ``"idle_hang"``, flags a client whose reported
    ``idle_ms`` (milliseconds since it last received a packet) grows
    continuously for a sustained stretch -- from the server's perspective a
    client that has gone silent (most likely a network problem) and is heading
    for a timeout drop.

    Detection is deterministic and per :func:`session_key`. For each session its
    ``client`` samples are taken in file order; a *run* is a maximal block of
    consecutive samples whose numeric ``idle_ms`` strictly increases sample over
    sample, including the baseline sample that starts the block. A missing /
    non-numeric value, or a value not greater than the previous one, breaks the
    run (the breaching sample starts a fresh one). A run is emitted when it
    lasts at least ``idle_min_s`` seconds and its net ``idle_ms`` growth is at
    least ``idle_growth_ms``.

    Each entry::

        {"kind": "idle_hang", "session": <key>, "player": <key or None>,
         "start_t": <float>, "end_t": <float>, "duration_s": <float>,
         "peak_idle_ms": <float>, "drop_after": <bool>,
         "slot": <int|None>, "userid": <int|None>, "name": <str>,
         "steamid64": <str>}

    ``drop_after`` is true when a ``drop`` record for the same session occurs in
    ``[end_t, end_t + 60]``. ``player`` comes from :func:`player_groups` (``None``
    when the session has no identifying player); the remaining identity fields
    come from :func:`client_identities`. The result is sorted by ``start_t`` then
    session. Never raises on malformed input.
    """
    identities = client_identities(records)
    series = client_series(records)

    session_player = {}
    for pkey, group in player_groups(records).items():
        for skey in group.get("sessions", ()):
            session_player[skey] = pkey

    drop_times = defaultdict(list)
    for rec, run in _attach_runs(records):
        if rec.get("type") != "drop":
            continue
        skey = session_key(rec, run)
        if skey is None:
            continue
        t = _record_ts(rec)
        if t is not None:
            drop_times[skey].append(t)

    try:
        min_s = float(idle_min_s)
    except (TypeError, ValueError):
        min_s = 0.0
    try:
        min_growth = float(idle_growth_ms)
    except (TypeError, ValueError):
        min_growth = 0.0

    out = []

    def _flush(skey, run, times, idles):
        if not run:
            return
        t0 = times[run[0]]
        t1 = times[run[-1]]
        if t0 is None or t1 is None:
            return
        duration = float(t1) - float(t0)
        growth = float(idles[run[-1]]) - float(idles[run[0]])
        if duration < min_s or growth < min_growth:
            return
        end_t = float(t1)
        drops = drop_times.get(skey, ())
        ident = identities.get(skey, {})
        out.append({
            "kind": "idle_hang",
            "session": skey,
            "player": session_player.get(skey),
            "start_t": float(t0),
            "end_t": end_t,
            "duration_s": duration,
            "peak_idle_ms": float(max(idles[i] for i in run)),
            "drop_after": any(end_t <= dt <= end_t + 60.0 for dt in drops),
            "slot": ident.get("slot"),
            "userid": ident.get("userid"),
            "name": ident.get("name") or "",
            "steamid64": ident.get("steamid64") or "",
        })

    for skey, entry in series.items():
        times = entry.get("t") or []
        idles = entry.get("idle_ms") or []
        run = []
        for index in range(len(idles)):
            idle = idles[index]
            if idle is None:
                _flush(skey, run, times, idles)
                run = []
                continue
            if run and idle > idles[run[-1]]:
                run.append(index)
            else:
                _flush(skey, run, times, idles)
                run = [index]
        _flush(skey, run, times, idles)

    out.sort(key=lambda fp: (fp["start_t"], fp["session"] or ""))
    return out


# ---------------------------------------------------------------------------
# Digest
# ---------------------------------------------------------------------------

def build_digest(records, files=None):
    """Return a compact, machine-readable summary dict."""
    start, end = period(records)
    counts = counts_by_type(records)
    drops = drop_reason_counts(records)
    return {
        "files": list(files) if files else [],
        "records": len(records),
        "counts_by_type": {k: counts.get(k, 0) for k in KNOWN_TYPES},
        "period": {
            "start": start,
            "end": end,
            "duration_s": (end - start) if (start is not None and end is not None) else None,
        },
        "players": sorted(players_seen(records)),
        "sessions": len(client_identities(records)),
        "drop_count": counts.get("drop", 0),
        "top_drop_reasons": [[k, v] for k, v in _sorted_counts(drops)],
        "hitch_lines": counts.get("hitch", 0),
        "lag_spikes": lag_spike_count(records),
        "log_end": log_end_state(records),
        "exclusions": exclusion_windows(records),
        "anomalies_excluded": len(
            find_anomalies_detail(records, exclude=True)["excluded"]),
        "focus_point_count": len(focus_points(records)),
    }


# ---------------------------------------------------------------------------
# CSV
# ---------------------------------------------------------------------------

def _csv_value(value):
    if value is None:
        return ""
    if isinstance(value, (dict, list)):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    return value


def export_csv(records, rtype=None, stream=None):
    """Write CSV for ``records`` (optionally filtered to ``rtype``)."""
    rows = [r for r in records if rtype is None or r.get("type") == rtype]
    fieldnames = []
    seen = set()
    for key in ("type", "t"):
        if key not in seen:
            fieldnames.append(key)
            seen.add(key)
    extra = set()
    for rec in rows:
        extra.update(rec.keys())
    for key in sorted(extra):
        if key not in seen:
            fieldnames.append(key)
            seen.add(key)

    out = stream if stream is not None else sys.stdout
    writer = csv.DictWriter(out, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    for rec in rows:
        writer.writerow({k: _csv_value(rec.get(k)) for k in fieldnames})
    return len(rows)


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

def cmd_summary(args):
    records = parse_records(args.files)
    counts = counts_by_type(records)
    start, end = period(records)

    print("Files: %s" % ", ".join(args.files))
    print("Records: %d" % len(records))
    if start is not None:
        duration = end - start
        print("Period: %s .. %s (%.1fs)" % (fmt_ts(start), fmt_ts(end), duration))
    else:
        print("Period: (no timestamped records)")

    print("Counts by type:")
    for name in KNOWN_TYPES:
        if counts.get(name):
            print("  %-9s %d" % (name, counts[name]))

    ids = sorted(players_seen(records))
    print("Players seen: %d%s"
          % (len(ids), (" (" + ", ".join(str(i) for i in ids) + ")") if ids else ""))

    reasons = _sorted_counts(drop_reason_counts(records))
    print("Top drop reasons:")
    if reasons:
        for reason, count in reasons:
            print("  %-40s %d" % (reason, count))
    else:
        print("  (none)")

    print("Hitch lines: %d" % counts.get("hitch", 0))
    print("Lag spikes (server frames >100ms): %d" % lag_spike_count(records))

    detail = find_anomalies_detail(records, exclude=(not args.no_exclude))
    print("Anomalies: %d" % len(detail["findings"]))
    if detail["excluded"]:
        print("Excluded by shutdown/mapchange/warmup: %d" % len(detail["excluded"]))
    windows = exclusion_windows(records)
    if windows:
        print("Exclusion windows:")
        for window in windows:
            print("  %-9s %s .. %s (%s)" % (
                window["kind"], fmt_ts(window["start"]),
                fmt_ts(window["end"]), window["reason"]))
    return 0


def cmd_drops(args):
    try:
        since = parse_ts(args.since)
        until = parse_ts(args.until)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    records = parse_records(args.files)
    selected = []
    for rec in records:
        if rec.get("type") != "drop":
            continue
        if args.client is not None:
            cid = _num(rec.get("id"))
            if cid is None or int(cid) != args.client:
                continue
        if args.reason:
            needle = args.reason.lower()
            haystack = " ".join(
                str(rec.get(k, "")) for k in ("reason", "category")).lower()
            if needle not in haystack:
                continue
        t = _record_ts(rec)
        if since is not None and (t is None or t < since):
            continue
        if until is not None and (t is None or t > until):
            continue
        selected.append(rec)

    selected.sort(key=lambda r: (_record_ts(r) or 0.0, _num(r.get("id")) or 0))
    print("Drops: %d" % len(selected))
    for rec in selected:
        print("%s  id=%s gen=%s crash=%s via=%s  reason=%s [%s]" % (
            fmt_ts(rec.get("t")),
            rec.get("id"),
            rec.get("gen"),
            rec.get("crash"),
            rec.get("observed_via"),
            rec.get("reason"),
            rec.get("category"),
        ))
    return 0


def cmd_window(args):
    try:
        center = parse_ts(args.around)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.secs < 0:
        print("error: --secs must be non-negative", file=sys.stderr)
        return 2

    records = parse_records(args.files)
    low = center - args.secs
    high = center + args.secs
    window = [r for r in records
              if _record_ts(r) is not None and low <= _record_ts(r) <= high]
    window.sort(key=lambda r: _record_ts(r))
    print("Window: %s +/- %.1fs -> %d records" % (fmt_ts(center), args.secs, len(window)))
    for rec in window:
        # Compact, agent-friendly one-liners.
        print("%s  %s  %s" % (
            fmt_ts(rec.get("t")),
            rec.get("type"),
            json.dumps({k: v for k, v in rec.items() if k != "type"},
                       sort_keys=True, separators=(",", ":")),
        ))
    return 0


def _session_sort_key(stats):
    run = stats.get("run")
    run = int(run) if isinstance(run, (int, float)) and not isinstance(run, bool) else 0
    cid = stats.get("id")
    cid = int(cid) if isinstance(cid, (int, float)) and not isinstance(cid, bool) else 10 ** 9
    return (run, cid, str(stats.get("session") or ""))


def cmd_clients(args):
    records = parse_records(args.files)
    clients = aggregate_clients(records)
    ordered = sorted(clients.values(), key=_session_sort_key)
    print("Clients: %d" % len(ordered))
    print("%-16s %-7s %-5s %-5s %-20s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s" % (
        "session", "userid", "slot", "id", "steamid64", "samples",
        "ping_avg", "ping_max", "loss_avg", "loss_max",
        "choke_avg", "choke_max", "fill_max"))
    for s in ordered:
        print("%-16s %-7s %-5s %-5s %-20s %-8d %-8s %-8s %-8s %-8s %-8s %-8s %-8s" % (
            s["session"],
            _short(s["userid"]), _short(s["slot"]), _short(s["id"]),
            s["steamid64"] or "—",
            s["samples"],
            _short(s["ping_avg"]), _short(s["ping_max"]),
            _short(s["loss_avg"]), _short(s["loss_max"]),
            _short(s["choke_avg"]), _short(s["choke_max"]),
            _short(s["rel_fill_max"]),
        ))
    return 0


def cmd_anomalies(args):
    records = parse_records(args.files)
    detail = find_anomalies_detail(records, exclude=(not args.no_exclude))
    findings = detail["findings"]
    print("Anomalies: %d" % len(findings))
    for line in findings:
        print("  - %s" % line)
    if detail["excluded"]:
        print("Excluded by shutdown/mapchange/warmup: %d"
              % len(detail["excluded"]))
    return 0


def cmd_digest(args):
    records = parse_records(args.files)
    digest = build_digest(records, files=args.files)
    print(json.dumps(digest, indent=2, sort_keys=True))
    return 0


def cmd_export_csv(args):
    records = parse_records(args.files)
    if args.type and args.type not in _KNOWN_TYPES:
        print("error: unknown --type %r" % args.type, file=sys.stderr)
        return 2
    export_csv(records, rtype=args.type)
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(
        prog="hldsmon",
        description="Offline analyzer for HldsMonitoring JSONL telemetry "
                    "(schema: docs/plan.md Appendix P1).")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("summary", help="period, counts, players, top drop reasons")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.add_argument("--no-exclude", dest="no_exclude", action="store_true",
                   help="count anomalies inside shutdown/mapchange/warmup windows")
    p.set_defaults(func=cmd_summary)

    p = sub.add_parser("drops", help="list drop events with optional filters")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.add_argument("--reason", help="case-insensitive substring on reason/category")
    p.add_argument("--client", type=int, help="client id")
    p.add_argument("--since", help="ISO-8601 or epoch seconds (inclusive)")
    p.add_argument("--until", help="ISO-8601 or epoch seconds (inclusive)")
    p.set_defaults(func=cmd_drops)

    p = sub.add_parser("window", help="records within +/- N seconds of a timestamp")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.add_argument("--around", required=True, help="ISO-8601 or epoch seconds")
    p.add_argument("--secs", type=float, required=True, help="half-window seconds")
    p.set_defaults(func=cmd_window)

    p = sub.add_parser("clients", help="per-client aggregates")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.set_defaults(func=cmd_clients)

    p = sub.add_parser("anomalies", help="heuristic anomaly scan")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.add_argument("--no-exclude", dest="no_exclude", action="store_true",
                   help="report anomalies inside shutdown/mapchange/warmup windows")
    p.set_defaults(func=cmd_anomalies)

    p = sub.add_parser("digest", help="compact machine-readable JSON summary")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.set_defaults(func=cmd_digest)

    p = sub.add_parser("export-csv", help="export records as CSV to stdout")
    p.add_argument("files", nargs="+", help="JSONL file(s)")
    p.add_argument("--type", help="restrict to a single record type")
    p.set_defaults(func=cmd_export_csv)

    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except FileNotFoundError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    except BrokenPipeError:  # e.g. `... | head`
        return 0


if __name__ == "__main__":
    sys.exit(main())
