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
    "modnet",
    "shutdown",
    "crash",
    "config",
    "host",
    "degraded",
    "retention",
)
_KNOWN_TYPES = frozenset(KNOWN_TYPES)

#: Numeric tracked-config fields (docs/plan.md, P1 `config` / `drop.config`).
CONFIG_FIELDS = (
    "sys_ticrate",
    "host_framerate",
    "sv_timeout",
    "sv_maxrate",
    "sv_minrate",
    "sv_maxupdaterate",
    "sv_minupdaterate",
    "sv_maxcmdrate",
    "sv_mincmdrate",
)

#: Numeric `host` metric fields (docs/plan.md, P1 extension `host`).
HOST_FIELDS = (
    "udp_rcvbuf_errors",
    "udp_in_errors",
    "udp_in_csum_errors",
    "net_rx_dropped",
    "net_rx_missed",
    "net_rx_overruns",
    "steal_pct",
    "io_read_bytes",
    "io_write_bytes",
    "disk_bavail_mb",
)

#: `present` mask bit per emitted `host` field; constants mirrored in
#: ``src/host_metrics.h`` (HM-3). A field whose bit is clear is JSON null
#: (unavailable or no Δ baseline yet).
HOST_PRESENT_BITS = {name: 1 << index for index, name in enumerate(HOST_FIELDS)}

#: `caveat` mask bits by source (``src/host_metrics.h``): the value is present
#: but must not be attributed to this server (shared netns, driver-dependent
#: NIC counters, lxcfs-steal, task-io semantics, thin/quota disk, truncated
#: source read).
HOST_CAVEAT_BITS = {
    "udp_netns": 1 << 0,
    "net_dev": 1 << 1,
    "steal": 1 << 2,
    "io": 1 << 3,
    "disk": 1 << 4,
    "truncated": 1 << 5,
}

#: ``host`` metric field -> caveat source name (:data:`HOST_CAVEAT_BITS` key).
#: A sample is *caveated* for that field when the source's bit is set in
#: ``caveat_mask``: the value is present but must not be attributed to this
#: server (shared netns, driver-dependent NIC counters, lxcfs-steal, task-io
#: semantics, thin/quota disk). Mirrored in the web UI via the ``host_caveats``
#: payload key (design §27.2 п.1, AU-5/F3).
HOST_FIELD_CAVEATS = {
    "udp_rcvbuf_errors": "udp_netns",
    "udp_in_errors": "udp_netns",
    "udp_in_csum_errors": "udp_netns",
    "net_rx_dropped": "net_dev",
    "net_rx_missed": "net_dev",
    "net_rx_overruns": "net_dev",
    "steal_pct": "steal",
    "io_read_bytes": "io",
    "io_write_bytes": "io",
    "disk_bavail_mb": "disk",
}

#: :data:`HOST_FIELD_CAVEATS` flattened to the mask bit per field.
HOST_FIELD_CAVEAT_BITS = {
    field: HOST_CAVEAT_BITS[source]
    for field, source in HOST_FIELD_CAVEATS.items()
}

#: Incident bundle envelope version and default upper size bound (bytes of the
#: serialized JSON envelope). The bound is enforced best-effort with a warning
#: (AU-1/AU-2).
BUNDLE_VERSION = 1
BUNDLE_MAX_BYTES = 8 * 1024 * 1024

#: Slide thresholds used by the anomaly heuristics.
MASS_DROP_COUNT = 3
MASS_DROP_WINDOW_S = 5.0
HITCH_CLUSTER_COUNT = 3
HITCH_CLUSTER_WINDOW_S = 10.0
REL_FILL_GROWTH = 25.0
REL_FILL_HIGH = 80.0
LOSS_SPIKE_ABS = 20.0
LOSS_SPIKE_DELTA = 15.0

#: modnet correlation: a sample at or above this rate (KB/s) next to a
#: drop/hitch counts as a traffic burst; any ``conn_fail`` counts as an anomaly.
MODNET_BURST_KBPS = 50.0


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


# Trailing loss-rate window (stage 7, design §24.3 п.4 / D2). The emitted
# `loss` is already a percentage (client_t::packet_loss), so a trailing mean
# separates a transient spike from a persistently bad link without any new
# plugin field and without cumulative counters (in_seq).
LOSS_RATE_WINDOW = 5


def rolling_mean(values, window=LOSS_RATE_WINDOW):
    """Trailing mean over up to ``window`` samples, skipping ``None`` gaps.

    Returns one value per input position, in order; the value is ``None`` when
    the window holds no numeric sample. ``window`` < 1 is treated as 1.
    """
    if window < 1:
        window = 1
    out = []
    for idx in range(len(values)):
        chunk = [v for v in values[max(0, idx - window + 1):idx + 1]
                 if v is not None]
        out.append(mean(chunk) if chunk else None)
    return out


def loss_rate_series(records, window=LOSS_RATE_WINDOW):
    """Per-session rolling loss-rate from :func:`client_series`.

    Keyed exactly like ``client_series``; each value is the trailing mean of
    the per-sample ``loss`` percentages (missing samples are skipped, an
    all-``None`` window stays ``None``).
    """
    return {skey: rolling_mean(entry.get("loss", []), window)
            for skey, entry in client_series(records).items()}


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
# Modnet aggregation
# ---------------------------------------------------------------------------

_MODNET_SERIES_KEYS = ("t", "conns", "conn_fail", "connect_attempts",
                       "dns_calls", "in_bytes", "out_bytes",
                       "in_kbps", "out_kbps")

_MODNET_SUM_FIELDS = ("conns", "conn_fail", "connect_attempts", "dns_calls",
                      "in_bytes", "out_bytes")


def _modnet_text(value):
    """Return a modnet ``module``/``so`` label as a string (``""`` if unusable)."""
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return ""
    if isinstance(value, (int, float)):
        return str(value)
    return ""


def modnet_key(rec, run):
    """Return the ``"<module>|<so>|<run>"`` series key for a modnet record.

    ``run`` is the process-level run index from :func:`_attach_runs` (a
    ``shutdown``/``crash`` boundary); object unload/reload inside one run is not
    split because the object-level counter reset has no log marker (MN-8).
    """
    return "%s|%s|%d" % (_modnet_text(rec.get("module")),
                         _modnet_text(rec.get("so")), int(run))


def modnet_series(records):
    """Return per-object modnet parallel time series keyed by :func:`modnet_key`.

    Each entry holds ``t/conns/conn_fail/connect_attempts/dns_calls/in_bytes/
    out_bytes/in_kbps/out_kbps`` lists, one value per ``modnet`` record in file
    order. A missing or non-numeric value becomes ``None``. An interval in which
    the object had no activity emits no record at all, so it is simply absent
    from the arrays -- a gap means zero, never a null or a series reset.
    """
    series = {}
    for rec, run in _attach_runs(records):
        if rec.get("type") != "modnet":
            continue
        key = modnet_key(rec, run)
        entry = series.get(key)
        if entry is None:
            entry = {name: [] for name in _MODNET_SERIES_KEYS}
            series[key] = entry
        for name in _MODNET_SERIES_KEYS:
            entry[name].append(_num(rec.get(name)))
    return series


def modnet_summary(records):
    """Return ``{module: aggregate}`` over all ``modnet`` records.

    Each aggregate carries the summed ``conns``/``conn_fail``/
    ``connect_attempts``/``dns_calls``/``in_bytes``/``out_bytes`` (over every
    run and object of that module) and the peak ``in_kbps``/``out_kbps``
    (``None`` when no usable sample). Missing/non-numeric fields contribute
    nothing; sums are integer ``0`` when no usable value was seen.
    """
    acc = {}
    for rec in records:
        if rec.get("type") != "modnet":
            continue
        module = _modnet_text(rec.get("module"))
        entry = acc.get(module)
        if entry is None:
            entry = {name: 0 for name in _MODNET_SUM_FIELDS}
            entry["peak_in_kbps"] = None
            entry["peak_out_kbps"] = None
            acc[module] = entry
        for name in _MODNET_SUM_FIELDS:
            value = _num(rec.get(name))
            if value is not None:
                entry[name] += value
        for name, peak_name in (("in_kbps", "peak_in_kbps"),
                                ("out_kbps", "peak_out_kbps")):
            value = _num(rec.get(name))
            if value is None:
                continue
            peak = entry[peak_name]
            if peak is None or value > peak:
                entry[peak_name] = value
    return acc


def correlate_modnet(records, window_s=5.0):
    """Correlate ``modnet`` anomalies with ``drop``/``hitch`` events.

    Every sample within ``window_s`` seconds (inclusive) of a ``drop``/``hitch``
    event whose ``in_kbps``/``out_kbps`` is at least :data:`MODNET_BURST_KBPS`,
    or whose ``conn_fail`` is non-zero, yields one entry::

        {"event_t", "kind", "module", "so", "metric", "value"}

    A sample may match several events/metrics; entries are sorted by
    ``(event_t, kind, module, so, metric, value)`` so the output is
    deterministic. Events and samples without a usable ``t`` are skipped.
    """
    try:
        half = float(window_s)
    except (TypeError, ValueError):
        half = 0.0

    events = []
    for rec in records:
        if rec.get("type") not in ("drop", "hitch"):
            continue
        t = _record_ts(rec)
        if t is not None:
            events.append((float(t), rec.get("type")))

    out = []
    for rec in records:
        if rec.get("type") != "modnet":
            continue
        t = _record_ts(rec)
        if t is None:
            continue
        t = float(t)
        metrics = (
            ("in_kbps", MODNET_BURST_KBPS, False),
            ("out_kbps", MODNET_BURST_KBPS, False),
            ("conn_fail", 0.0, True),
        )
        for name, threshold, is_counter in metrics:
            value = _num(rec.get(name))
            if value is None:
                continue
            anomaly = value > 0 if is_counter else value >= threshold
            if not anomaly:
                continue
            for event_t, kind in events:
                if event_t - half <= t <= event_t + half:
                    out.append({
                        "event_t": event_t,
                        "kind": kind,
                        "module": _modnet_text(rec.get("module")),
                        "so": _modnet_text(rec.get("so")),
                        "metric": name,
                        "value": value,
                    })
    out.sort(key=lambda item: (item["event_t"], item["kind"], item["module"],
                               item["so"], item["metric"], item["value"]))
    return out


def _modnet_digest_block(records):
    """Return the digest ``modnet`` block: totals plus per-module rows."""
    summary = modnet_summary(records)
    total_in = 0
    total_out = 0
    total_fail = 0
    rows = []
    for module, agg in summary.items():
        total_in += agg["in_bytes"]
        total_out += agg["out_bytes"]
        total_fail += agg["conn_fail"]
        rows.append([module, agg["in_bytes"], agg["out_bytes"],
                     agg["conn_fail"]])
    rows.sort(key=lambda row: (-(row[1] + row[2]), row[0]))
    return {
        "records": sum(1 for r in records if r.get("type") == "modnet"),
        "in_bytes": total_in,
        "out_bytes": total_out,
        "conn_fail": total_fail,
        "modules": rows,
    }


def _config_digest_block(records):
    """Return the digest ``config`` block: record count and latest snapshot.

    ``null`` values (cvar absent on the server) pass through as ``None``.
    """
    configs = [r for r in records if r.get("type") == "config"]
    latest = None
    if configs:
        rec = configs[-1]
        latest = {
            "t": _num(rec.get("t")),
            "rehlds_major": _num(rec.get("rehlds_major")),
            "rehlds_minor": _num(rec.get("rehlds_minor")),
            "map": rec.get("map") if isinstance(rec.get("map"), str) else None,
            "maxclients": _num(rec.get("maxclients")),
        }
        for key in CONFIG_FIELDS:
            latest[key] = _num(rec.get(key))
    return {"records": len(configs), "latest": latest}


#: Tracked fields compared by :func:`config_diff`: the numeric
#: :data:`CONFIG_FIELDS` plus the non-numeric config context (design §27.2 п.4).
CONFIG_DIFF_FIELDS = CONFIG_FIELDS + (
    "map",
    "maxclients",
    "rehlds_major",
    "rehlds_minor",
)


def _config_value(rec, field):
    """Return a config field's value, or ``None`` when absent/unusable.

    ``map`` is a non-empty string; every other field is numeric. A missing key
    and an explicit JSON ``null`` are equivalent (the cvar was not registered).
    """
    value = rec.get(field)
    if field == "map":
        return value if isinstance(value, str) and value != "" else None
    return _num(value)


def config_snapshot(rec):
    """Return the normalized snapshot of a ``config`` record.

    Keys: ``t`` plus :data:`CONFIG_DIFF_FIELDS`; absent/unusable values become
    ``None`` (never zero). Accepts both raw records and snapshots.
    """
    snap = {"t": _record_ts(rec)}
    for field in CONFIG_DIFF_FIELDS:
        snap[field] = _config_value(rec, field)
    return snap


def config_at(records, t):
    """Return the last ``config`` record with time ``<= t``, or ``None``.

    "Last" is file order among matching records (ties on ``t``: later wins).
    ``t`` is epoch seconds; a non-numeric or ``None`` ``t`` yields ``None``.
    """
    cutoff = _num(t)
    if cutoff is None:
        return None
    latest = None
    for rec in records:
        if rec.get("type") != "config":
            continue
        rt = _record_ts(rec)
        if rt is None or rt > cutoff:
            continue
        latest = rec
    return latest


def config_diff(before, after):
    """Diff two config records/snapshots over :data:`CONFIG_DIFF_FIELDS`.

    Returns ``{"baseline", "changed", "added", "removed"}``; every entry is
    ``{"field", "before", "after"}`` in :data:`CONFIG_DIFF_FIELDS` order.

    A value that is null/missing on one side is *added* (set) or *removed*
    (cleared) — never *changed*. With no baseline (``before`` is ``None``/not a
    dict) all three lists stay empty: absence of a baseline is not "everything
    changed".
    """
    result = {"baseline": isinstance(before, dict),
              "changed": [], "added": [], "removed": []}
    if not result["baseline"]:
        return result
    after_d = after if isinstance(after, dict) else {}
    for field in CONFIG_DIFF_FIELDS:
        old = _config_value(before, field)
        new = _config_value(after_d, field)
        if old is None and new is None:
            continue
        entry = {"field": field, "before": old, "after": new}
        if old is None:
            result["added"].append(entry)
        elif new is None:
            result["removed"].append(entry)
        elif old != new:
            result["changed"].append(entry)
    return result


def config_series(records, low=None, high=None):
    """Return normalized ``config`` snapshots (file order), each with ``diff``.

    ``diff`` is :func:`config_diff` against the previous snapshot — inside the
    ``[low, high]`` window when bounds are given, with the baseline taken from
    the last config at/ before ``low``. Snapshots without a usable ``t`` are
    skipped. ``records`` must be a sequence (it is scanned twice when ``low``
    is set).
    """
    prev = config_at(records, low) if low is not None else None
    out = []
    for rec in records:
        if rec.get("type") != "config":
            continue
        t = _record_ts(rec)
        if t is None:
            continue
        if low is not None and t < low:
            continue
        if high is not None and t > high:
            continue
        snap = config_snapshot(rec)
        snap["diff"] = config_diff(prev, rec)
        out.append(snap)
        prev = rec
    return out


def host_series(records):
    """Return parallel time series over ``host`` records in file order.

    Keys: ``t`` plus :data:`HOST_FIELDS` plus ``present_mask``/``caveat_mask``.
    A missing or ``null`` value becomes ``None`` — unavailable (or the first
    sample, whose Δ has no baseline) must never read as a healthy zero.
    """
    keys = ("t",) + HOST_FIELDS + ("present_mask", "caveat_mask")
    series = {name: [] for name in keys}
    for rec in records:
        if rec.get("type") != "host":
            continue
        for name in keys:
            series[name].append(_num(rec.get(name)))
    return series


def _host_digest_block(records):
    """Return the digest ``host`` block: record count and latest snapshot.

    ``null`` values pass through as ``None``; ``caveats`` lists the source
    names whose bits are set in the latest ``caveat_mask`` (value present but
    not attributable — design §23).
    """
    hosts = [r for r in records if r.get("type") == "host"]
    latest = None
    if hosts:
        rec = hosts[-1]
        latest = {"t": _num(rec.get("t"))}
        for key in HOST_FIELDS:
            latest[key] = _num(rec.get(key))
        present = _num(rec.get("present_mask"))
        caveat = _num(rec.get("caveat_mask"))
        latest["present_mask"] = present
        latest["caveat_mask"] = caveat
        latest["caveats"] = [
            name for name, bit in sorted(HOST_CAVEAT_BITS.items())
            if caveat is not None and (int(caveat) & bit)
        ]
    return {"records": len(hosts), "latest": latest}


# ---------------------------------------------------------------------------
# Incident bundle (JSON envelope) + reader
# ---------------------------------------------------------------------------

def _bundle_size(bundle):
    """Serialized size in bytes, matching the web UI's JSON encoding."""
    return len(json.dumps(bundle, ensure_ascii=False).encode("utf-8"))


def _bundle_config(records, low, high):
    """Config snapshots in the window; falls back to the active snapshot."""
    snapshots = config_series(records, low, high)
    if not snapshots and high is not None:
        anchor = config_at(records, high)
        if anchor is not None:
            snap = config_snapshot(anchor)
            snap["diff"] = config_diff(None, anchor)
            snapshots = [snap]
    return snapshots


def _bundle_host(window_records):
    """Host block: the Δ series of the ``host`` records inside the window."""
    hosts = [r for r in window_records if r.get("type") == "host"]
    return {"records": len(hosts), "series": host_series(hosts)}


def incident_bundle(records, drop=None, mins=2.0, include_records=None,
                    max_bytes=BUNDLE_MAX_BYTES):
    """Build the incident JSON envelope for an analyzer record sequence.

    Envelope: ``{bundle_version, drop, window:{start,end}, config, host,
    records, warnings}``. ``drop`` anchors the window: a drop record (dict,
    embedded as-is), a timestamp, or ``None`` (whole log span). ``mins`` is the
    half-window in minutes.

    Dedup (AU-2/F1): the drop already embeds its own ``window`` sample slice, so
    raw window records are included only when there is no drop — or when
    ``include_records=True`` forces both (the embedded drop is then excluded
    from ``records``). ``max_bytes`` caps the serialized envelope; oversize
    bundles drop raw records, then trim ``drop.window`` from the oldest end,
    recording every step in ``warnings``.
    """
    records = list(records)

    if isinstance(drop, dict):
        center = _record_ts(drop)
        if center is None:
            raise ValueError("drop record has no usable t")
        drop_out = drop
    elif drop is None:
        center = None
        drop_out = None
    else:
        center = _num(drop)
        if center is None:
            raise ValueError("drop must be a record dict, a timestamp or None")
        drop_out = None

    half = _num(mins)
    if half is None or half < 0:
        raise ValueError("mins must be a non-negative number")
    half_seconds = float(half) * 60.0

    if center is not None:
        low, high = center - half_seconds, center + half_seconds
    else:
        stamps = [t for t in (_record_ts(r) for r in records)
                  if t is not None]
        low, high = (min(stamps), max(stamps)) if stamps else (None, None)

    window = []
    if low is not None:
        for index, rec in enumerate(records):
            t = _record_ts(rec)
            if t is not None and low <= t <= high:
                window.append((t, index, rec))
    window.sort(key=lambda item: (item[0], item[1]))
    ordered = [rec for _, _, rec in window]

    warnings = []
    include = (drop_out is None) if include_records is None else bool(include_records)
    if include:
        kept = [r for r in ordered if r is not drop_out]
        if drop_out is not None:
            warnings.append(
                "drop and raw records both included: the drop's embedded "
                "window may duplicate client samples (dedup policy AU-2/F1)")
    else:
        kept = []
        if ordered:
            warnings.append(
                "raw window records omitted (dedup policy AU-2/F1: the drop "
                "already carries its own window); pass include_records=True "
                "to force")

    bundle = {
        "bundle_version": BUNDLE_VERSION,
        "drop": drop_out,
        "window": {"start": low, "end": high},
        "config": _bundle_config(records, low, high),
        "host": _bundle_host(ordered),
        "records": kept,
        "warnings": warnings,
    }

    cap = _num(max_bytes)
    if cap is None or cap < 0:
        cap = BUNDLE_MAX_BYTES
    cap = int(cap)
    if _bundle_size(bundle) > cap:
        if bundle["records"]:
            bundle["records"] = []
            warnings.append(
                "size cap %d bytes: raw window records dropped" % cap)
        if _bundle_size(bundle) > cap and isinstance(drop_out, dict):
            embedded = drop_out.get("window")
            if isinstance(embedded, list) and embedded:
                trimmed = list(embedded)
                drop_out = dict(drop_out)
                drop_out["window"] = trimmed
                bundle["drop"] = drop_out
                warn_at = len(warnings)
                warnings.append("")
                while True:
                    warnings[warn_at] = (
                        "size cap %d bytes: drop.window trimmed to %d samples"
                        % (cap, len(trimmed)))
                    if not trimmed or _bundle_size(bundle) <= cap:
                        break
                    trimmed.pop(0)
        if _bundle_size(bundle) > cap:
            warnings.append(
                "bundle still exceeds the %d-byte cap (cannot shrink further)"
                % cap)
    return bundle


def read_bundle(source):
    """Return the records embedded in an incident bundle (re-ingestible).

    ``source`` may be a path, a file object, or an already-decoded envelope.
    Returns ``[drop] + records`` sorted by ``t`` (stable: file order for ties,
    timestamp-less records last) — every entry carries a known ``type``, so the
    result can be written back as JSONL and parsed by :func:`iter_records` or
    fed to any record-based analyzer function. Raises ``ValueError`` when
    ``source`` is not an incident bundle envelope.
    """
    if isinstance(source, dict):
        bundle = source
    elif hasattr(source, "read"):
        bundle = _parse_bundle_json(source.read())
    elif isinstance(source, (str, bytes)) or hasattr(source, "__fspath__"):
        with open(source, "r", encoding="utf-8", errors="replace") as fh:
            bundle = _parse_bundle_json(fh.read())
    else:
        raise ValueError(
            "source must be a path, a file object or a bundle dict")
    if not isinstance(bundle, dict) or bundle.get("bundle_version") is None:
        raise ValueError("not an incident bundle: missing bundle_version")
    out = []
    if isinstance(bundle.get("drop"), dict):
        out.append(bundle["drop"])
    recs = bundle.get("records")
    if isinstance(recs, list):
        out.extend(r for r in recs
                   if isinstance(r, dict) and r.get("type") in _KNOWN_TYPES)
    out.sort(key=lambda r: (_record_ts(r) if _record_ts(r) is not None
                            else float("inf")))
    return out


def _parse_bundle_json(text):
    try:
        return json.loads(text)
    except (ValueError, TypeError) as exc:
        raise ValueError("invalid bundle JSON") from exc


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
        "modnet": _modnet_digest_block(records),
        "config": _config_digest_block(records),
        "host": _host_digest_block(records),
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

    modules = ", ".join(sorted(modnet_summary(records)))
    print("Modnet: %d records, modules: %s"
          % (counts.get("modnet", 0), modules if modules else "(none)"))

    print("Config: %d records" % counts.get("config", 0))

    print("Host: %d records" % counts.get("host", 0))

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
