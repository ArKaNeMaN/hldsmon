#!/usr/bin/env python3
"""HldsMonitoring offline web UI (stdlib only).

A tiny, local-only HTTP server that serves a single dashboard page and a JSON
analysis API backed by the offline analyzer :mod:`hldsmon`. It never parses
JSONL itself: uploaded bytes are written to a temporary file and handed to
``hldsmon.parse_records``.

Design constraints (plan Phase 5 / web UI):
* standard library only (no network, no third-party packages, no framework);
* never modifies input files -- the only file ever written is a temporary file
  that is always deleted (see :func:`_records_from_bytes`);
* tolerant of malformed uploads because parsing is delegated to ``hldsmon``;
* binds to ``127.0.0.1`` by default and only ever serves the single
  ``webui/index.html`` page (or a built-in fallback), never arbitrary files.

Launch::

    python3 tools/analyzer/webui.py [--host 127.0.0.1] [--port 8765] [--no-open]

Importable test surface (no socket required)::

    analyze_bytes(data, files=None) -> dict
    export_bytes(data, rtype=None) -> str
    make_server(host, port) -> http.server.ThreadingHTTPServer
"""
from __future__ import annotations

import argparse
import ftplib
import io
import json
import os
import secrets
import ssl
import sys
import tempfile
import threading
import time
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import hldsmon

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

#: Hard cap on the request body size (256 MiB).
MAX_BODY_BYTES = 256 * 1024 * 1024

#: Hard cap on a single FTP download (256 MiB), matching ``MAX_BODY_BYTES``.
MAX_FTP_BYTES = 256 * 1024 * 1024

#: Default socket timeout (seconds) for FTP connections.
DEFAULT_FTP_TIMEOUT = 30.0

#: In-memory FTP tail sessions. Credentials live here only, are never returned
#: to a client, never logged, and never written to disk. Guarded by ``_FTP_LOCK``.
_FTP_SESSIONS = {}
_FTP_LOCK = threading.Lock()

#: Transient download-progress entries keyed by an opaque job id, query-able
#: while a connect/refresh is in flight. Purged after ``_PROGRESS_TTL_S`` and
#: never persisted. Guarded by ``_PROGRESS_LOCK``.
_PROGRESS = {}
_PROGRESS_LOCK = threading.Lock()
_PROGRESS_TTL_S = 300.0
_thread_progress = threading.local()

#: Path of the (separately authored) dashboard page, resolved from this file.
def _resource_dir():
    """Directory holding the bundled dashboard page (works frozen and normal)."""
    base = getattr(sys, "_MEIPASS", None)
    if base:
        return base
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


INDEX_PATH = os.path.join(_resource_dir(), "webui", "index.html")

SERVICE_NAME = "hldsmon-webui"
PROTOCOL_VERSION = 1

#: Ordered series accessors. Each maps a ``server`` record to a JSON value.
#: ``real_players`` is the wire name for the schema's ``real`` field (Appendix
#: P1). Missing / non-numeric fields yield ``None`` (JSON ``null``) so every
#: series array has exactly one entry per server record.
_SERIES_FIELDS = (
    "t", "frame_ms_min", "frame_ms_avg", "frame_ms_max",
    "cpu_pct", "rss_kb", "players", "real_players", "edicts",
    "fps", "in_kbps", "out_kbps",
)

#: Minimal built-in page used only when ``webui/index.html`` is absent. It is
#: never written to disk.
FALLBACK_HTML = """<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>HldsMonitoring — анализ</title>
<style>
 body { font-family: system-ui, sans-serif; margin: 2rem; background: #10141a;
        color: #e6edf3; }
 code { background: #1c2230; padding: .1rem .3rem; border-radius: 4px; }
 pre  { background: #1c2230; padding: 1rem; border-radius: 8px;
        overflow: auto; }
 .ok  { color: #4ade80; }
</style>
</head>
<body>
<h1>HldsMonitoring — офлайн-анализ</h1>
<p>Служебная страница-заглушка: файл <code>webui/index.html</code> не найден.</p>
<p>API доступен: <code>GET /api/health</code>,
   <code>POST /api/analyze</code>, <code>POST /api/export</code>.</p>
<p id="health">Проверка сервиса…</p>
<pre id="json"></pre>
<script>
fetch("/api/health").then(function (r) { return r.json(); })
  .then(function (d) {
    document.getElementById("health").className = d.ok ? "ok" : "";
    document.getElementById("health").textContent =
      "Сервис: " + d.service + " (version " + d.version + ")" +
      (d.ok ? " — работает" : " — ошибка");
    document.getElementById("json").textContent = JSON.stringify(d, null, 2);
  })
  .catch(function (e) {
    document.getElementById("health").textContent = "Ошибка: " + e;
  });
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# Core testable functions (no socket)
# ---------------------------------------------------------------------------

def _silent_remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def _records_from_bytes(data):
    """Parse raw JSONL ``data`` via ``hldsmon`` using a temporary file.

    The temporary file is always removed, even if parsing raises.
    """
    if isinstance(data, str):
        data = data.encode("utf-8")
    tmp = tempfile.NamedTemporaryFile(
        mode="wb", suffix=".jsonl", prefix="hldsmon-webui-", delete=False)
    path = tmp.name
    try:
        with tmp:
            tmp.write(data)
        return hldsmon.parse_records([path])
    finally:
        _silent_remove(path)


def _frame_value(rec, key):
    frame = rec.get("frame_ms")
    if isinstance(frame, dict):
        return hldsmon._num(frame.get(key))
    return None


def build_series(records):
    """Build per-sample series from ``server`` records, in file order.

    Every array has identical length (== number of server records); absent or
    non-numeric values are ``None`` (JSON ``null``).
    """
    series = dict((name, []) for name in _SERIES_FIELDS)
    for rec in records:
        if rec.get("type") != "server":
            continue
        series["t"].append(hldsmon._num(rec.get("t")))
        series["frame_ms_min"].append(_frame_value(rec, "min"))
        series["frame_ms_avg"].append(_frame_value(rec, "avg"))
        series["frame_ms_max"].append(_frame_value(rec, "max"))
        series["cpu_pct"].append(hldsmon._num(rec.get("cpu_pct")))
        series["rss_kb"].append(hldsmon._num(rec.get("rss_kb")))
        series["players"].append(hldsmon._num(rec.get("players")))
        series["real_players"].append(hldsmon._num(rec.get("real")))
        series["edicts"].append(hldsmon._num(rec.get("edicts")))
        series["fps"].append(hldsmon._num(rec.get("fps")))
        series["in_kbps"].append(hldsmon._num(rec.get("in_kbps")))
        series["out_kbps"].append(hldsmon._num(rec.get("out_kbps")))
    return series


def _as_str(value):
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return str(value)


def build_clients(records):
    """Return aggregate_clients entries sorted by (run, slot, id), one per session."""
    aggregates = hldsmon.aggregate_clients(records)
    clients = [dict(entry) for entry in aggregates.values()]
    clients.sort(key=lambda c: (
        c.get("run") if c.get("run") is not None else 0,
        c.get("slot") if c.get("slot") is not None else float("inf"),
        c.get("id") if c.get("id") is not None else float("inf"),
        c.get("session") or "",
    ))
    return clients


#: Identity fields copied onto each drop when a matching client is known.
_IDENTITY_STR_FIELDS = ("name", "authid", "steamid64", "ip")
_IDENTITY_FIELDS = (
    "name", "authid", "steamid64", "ip", "port", "slot", "userid",
    "conn_s", "fake", "proxy",
)


def _identity_fields(ident):
    """Return the identity fields for a drop, defaulting when absent.

    String fields default to ``""``; numeric/boolean fields default to ``None``.
    Never raises for a missing or malformed identity.
    """
    fields = {}
    for key in _IDENTITY_FIELDS:
        default = "" if key in _IDENTITY_STR_FIELDS else None
        value = ident.get(key) if isinstance(ident, dict) else None
        fields[key] = default if value is None else value
    return fields


def build_drops(records, identities=None):
    """Extract ``drop`` records (file order) into the wire shape.

    Each entry carries its ``session`` key (so the UI can navigate to the right
    client) and is enriched with the matching session's identity fields (when a
    ``client_identities`` lookup is supplied or derivable); absent fields are
    ``""`` (text) / ``None`` (numeric, boolean).
    """
    if identities is None:
        identities = hldsmon.client_identities(records)
    drops = []
    for rec, run in hldsmon._attach_runs(records):
        if rec.get("type") != "drop":
            continue
        t = hldsmon._num(rec.get("t"))
        cid = hldsmon._num(rec.get("id"))
        skey = hldsmon.session_key(rec, run)
        ident = identities.get(skey) if skey is not None else None
        entry = {
            "t": t,
            "iso": hldsmon.fmt_ts(t),
            "id": cid,
            "session": skey,
            "reason": _as_str(rec.get("reason")),
            "category": _as_str(rec.get("category")),
            "crash": rec.get("crash"),
            "observed_via": _as_str(rec.get("observed_via")),
            "gen": hldsmon._num(rec.get("gen")),
        }
        entry.update(_identity_fields(ident))
        drops.append(entry)
    return drops


def analyze_records(records, files=None):
    """Return the ``/api/analyze`` JSON payload for parsed ``records``."""
    files_list = list(files) if files else []
    identities = hldsmon.client_identities(records)
    return {
        "ok": True,
        "version": PROTOCOL_VERSION,
        "files": files_list,
        "digest": hldsmon.build_digest(records, files_list),
        "series": build_series(records),
        "clients": build_clients(records),
        "sessions": hldsmon.player_groups(records),
        "session_count": len(identities),
        "drops": build_drops(records, identities),
        "anomalies": hldsmon.find_anomalies(records),
        "records_count": len(records),
        "client_series": hldsmon.client_series(records),
        "client_identities": identities,
        "events": hldsmon.exclusion_windows(records),
        "log_end": hldsmon.log_end_state(records),
        "anomalies_excluded": hldsmon.find_anomalies_detail(
            records, exclude=True)["excluded"],
        "focus_points": hldsmon.focus_points(records),
    }


def analyze_bytes(data, files=None):
    """Return the ``/api/analyze`` JSON payload for raw JSONL ``data``."""
    records = _records_from_bytes(data)
    return analyze_records(records, files)


def export_bytes(data, rtype=None):
    """Return CSV text for raw JSONL ``data`` (optionally one record type).

    Raises ``ValueError`` for an unknown ``rtype``.
    """
    if rtype is not None and rtype not in hldsmon.KNOWN_TYPES:
        raise ValueError("unknown type: %r" % (rtype,))
    records = _records_from_bytes(data)
    buf = io.StringIO()
    hldsmon.export_csv(records, rtype=rtype, stream=buf)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# FTP live-tail (stdlib ftplib only; credentials in RAM only)
# ---------------------------------------------------------------------------

def _parse_line(raw):
    """Parse one JSONL line (bytes/str) into a record object or ``None``.

    Mirrors ``hldsmon.iter_records``: blank lines, invalid JSON, non-object
    values, and records with a missing/unknown ``type`` are skipped.
    """
    if isinstance(raw, bytes):
        try:
            text = raw.decode("utf-8", "replace")
        except Exception:
            return None
    else:
        text = raw
    text = text.strip()
    if not text:
        return None
    try:
        obj = json.loads(text)
    except (ValueError, TypeError):
        return None
    if not isinstance(obj, dict):
        return None
    rtype = obj.get("type")
    if not isinstance(rtype, str) or rtype not in hldsmon.KNOWN_TYPES:
        return None
    return obj


def _records_from_lines(lines):
    """Parse an iterable of complete line bytes into records (tolerant)."""
    out = []
    for raw in lines:
        rec = _parse_line(raw)
        if rec is not None:
            out.append(rec)
    return out


def split_tail(leftover, chunk):
    """Split appended bytes into complete lines plus the new incomplete tail.

    ``leftover`` is the unfinished line kept from the previous read; ``chunk``
    is the freshly appended bytes. Returns ``(complete_lines, new_leftover)``
    where ``complete_lines`` is a list of newline-stripped line ``bytes`` and
    ``new_leftover`` is the trailing incomplete line (``b""`` when the data
    ends on a newline). Accepts bytes or str inputs.
    """
    if isinstance(leftover, str):
        leftover = leftover.encode("utf-8")
    if isinstance(chunk, str):
        chunk = chunk.encode("utf-8")
    data = (leftover or b"") + (chunk or b"")
    idx = data.rfind(b"\n")
    if idx < 0:
        return [], data
    complete = data[:idx]
    new_leftover = data[idx + 1:]
    lines = complete.split(b"\n") if complete else []
    return lines, new_leftover


def resolve_path(path, date):
    """Substitute ``{date}`` / ``{YYYY-MM-DD}`` tokens with ``date``.

    ``date`` is the UTC ``YYYY-MM-DD`` string. Non-string input is returned
    unchanged.
    """
    if not isinstance(path, str):
        return path
    return path.replace("{date}", date).replace("{YYYY-MM-DD}", date)


def _utc_date():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d")


def _utc_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class _FtpTooLarge(Exception):
    """Raised when a remote download would exceed ``MAX_FTP_BYTES``."""


def _ftp_open(cfg):
    """Open an FTP(S) connection from ``cfg``; credentials stay in memory."""
    timeout = cfg.get("timeout")
    timeout = DEFAULT_FTP_TIMEOUT if timeout is None else float(timeout)
    host = cfg["host"]
    port = int(cfg.get("port") or 21)
    if cfg.get("tls"):
        ftp = ftplib.FTP_TLS(context=ssl.create_default_context())
    else:
        ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=timeout)
    ftp.login(cfg.get("user") or "anonymous", cfg.get("password") or "")
    if cfg.get("tls"):
        ftp.prot_p()
    return ftp


def _retr_all(ftp, path, offset=None, on_bytes=None):
    """Download remote ``path`` (binary), enforcing the size cap.

    ``offset`` (when given) is passed as the REST argument so only the appended
    tail is transferred. ``on_bytes(n)`` is called after each block with the
    running byte count (progress reporting).
    """
    chunks = []
    total = [0]

    def _sink(block):
        total[0] += len(block)
        if total[0] > MAX_FTP_BYTES:
            raise _FtpTooLarge("remote file exceeds %d bytes" % MAX_FTP_BYTES)
        chunks.append(block)
        if on_bytes is not None:
            on_bytes(total[0])

    if offset is None:
        ftp.retrbinary("RETR " + path, _sink)
    else:
        ftp.retrbinary("RETR " + path, _sink, rest=offset)
    return b"".join(chunks)


def _download(cfg, offset=0):
    """Return ``(remote_size, data)`` for ``cfg['path']``.

    ``data`` is the bytes from ``offset`` to EOF; the whole file is returned
    when ``offset`` is falsy or the remote shrank to ``<= offset``. Opens and
    always closes one connection. This is the network seam tests replace with
    a fake.
    """
    offset = int(offset or 0)
    ftp = _ftp_open(cfg)
    try:
        path = cfg["path"]
        try:
            size = ftp.size(path)
        except Exception:
            size = None
        if offset > 0 and size is not None and size > offset:
            try:
                return size, _retr_all(ftp, path, offset,
                                       _progress_callback(size - offset))
            except ftplib.all_errors:
                pass  # REST unsupported -> fall through to a full download
        data = _retr_all(ftp, path, None, _progress_callback(size))
        if offset > 0 and offset < len(data):
            return len(data), data[offset:]
        return (size if size is not None else len(data)), data
    finally:
        try:
            ftp.quit()
        except Exception:
            try:
                ftp.close()
            except Exception:
                pass


def _session_put(session_id, session):
    with _FTP_LOCK:
        _FTP_SESSIONS[session_id] = session


def _session_peek(session_id):
    with _FTP_LOCK:
        return _FTP_SESSIONS.get(session_id)


def _session_update(session_id, session, **fields):
    with _FTP_LOCK:
        current = _FTP_SESSIONS.get(session_id)
        if current is session:
            current.update(fields)


def _progress_callback(total):
    """Per-block reporter bound to the calling thread's active progress sink."""
    if not total:
        return lambda done: _emit_progress(done, None)
    return lambda done: _emit_progress(done, total)


def _emit_progress(done, total):
    sink = getattr(_thread_progress, "sink", None)
    if sink is None:
        return
    try:
        sink(done, total)
    except Exception:
        pass


def _progress_set(job, done, total):
    if not job:
        return
    now = time.time()
    pct = None
    if total and total > 0:
        pct = max(0.0, min(100.0, 100.0 * done / total))
    with _PROGRESS_LOCK:
        for key in [k for k, v in _PROGRESS.items()
                    if now - v.get("_ts", now) > _PROGRESS_TTL_S]:
            _PROGRESS.pop(key, None)
        _PROGRESS[job] = {
            "job": job,
            "phase": "download",
            "done": int(done),
            "total": int(total) if total else None,
            "pct": pct,
            "finished": False,
            "updated_at": _utc_iso(),
            "_ts": now,
        }


def _progress_finish(job, ok=True, error=None):
    if not job:
        return
    with _PROGRESS_LOCK:
        entry = _PROGRESS.get(job)
        if entry is None:
            return
        entry["finished"] = True
        entry["ok"] = bool(ok)
        if ok:
            entry["pct"] = 100.0
            if entry.get("total"):
                entry["done"] = entry["total"]
        if error:
            entry["error"] = str(error)
        entry["updated_at"] = _utc_iso()
        entry["_ts"] = time.time()


def _progress_phase(job, phase):
    """Switch a job to a new phase (e.g. "download" -> "parse"); indeterminate."""
    if not job:
        return
    with _PROGRESS_LOCK:
        entry = _PROGRESS.get(job)
        if entry is None:
            return
        entry["phase"] = str(phase)
        entry["finished"] = False
        entry["pct"] = None
        entry["updated_at"] = _utc_iso()
        entry["_ts"] = time.time()


def progress_get(job):
    """Return the public view of a progress entry, or ``None``."""
    if job is None:
        return None
    with _PROGRESS_LOCK:
        entry = _PROGRESS.get(str(job))
        if entry is None:
            return None
        return dict((k, v) for k, v in entry.items() if not k.startswith("_"))


def ftp_connect(spec):
    """Open an FTP tail session and return the first analysis payload."""
    host = str(spec.get("host") or "").strip()
    if not host:
        raise ValueError("host is required")
    path_token = str(spec.get("path") or "").strip()
    if not path_token:
        raise ValueError("path is required")
    resolved = resolve_path(path_token, _utc_date())
    cfg = {
        "host": host,
        "port": spec.get("port") or 21,
        "user": str(spec.get("user") or ""),
        "password": str(spec.get("password") or ""),
        "tls": bool(spec.get("tls")),
        "timeout": spec.get("timeout"),
        "path": resolved,
    }
    job = str(spec.get("job") or "").strip() or secrets.token_urlsafe(12)
    _thread_progress.sink = lambda done, total: _progress_set(job, done, total)
    _progress_set(job, 0, None)
    try:
        _, data = _download(cfg, 0)
    except BaseException as exc:
        _progress_finish(job, ok=False, error=str(exc))
        raise
    finally:
        _thread_progress.sink = None
    _progress_phase(job, "parse")
    try:
        lines, leftover = split_tail(b"", data)
        records = _records_from_lines(lines)
        offset = len(data) if isinstance(data, (bytes, bytearray)) else 0
        session_id = secrets.token_urlsafe(24)
        updated_at = _utc_iso()
        _session_put(session_id, {
            "cfg": cfg,
            "path_token": path_token,
            "offset": offset,
            "leftover": leftover,
            "records": records,
            "updated_at": updated_at,
        })
        payload = analyze_records(records, files=[resolved])
        payload["session"] = session_id
        payload["job"] = job
        payload["ftp"] = {
            "path": resolved,
            "bytes": offset,
            "records": len(records),
            "updated_at": updated_at,
        }
    except BaseException as exc:
        _progress_finish(job, ok=False, error=str(exc))
        raise
    _progress_finish(job, ok=True)
    return payload


def ftp_refresh(session_id):
    """Read only the newly appended bytes and return the recomputed payload."""
    session = _session_peek(session_id)
    if session is None:
        raise KeyError("unknown session")
    resolved = resolve_path(session["path_token"], _utc_date())
    cfg = dict(session["cfg"])
    cfg["path"] = resolved
    offset = int(session["offset"])
    rotated = resolved != session["cfg"]["path"]

    _thread_progress.sink = lambda done, total: _progress_set(session_id, done, total)
    _progress_set(session_id, 0, None)
    try:
        if rotated:
            _, data = _download(cfg, 0)
        else:
            size, delta = _download(cfg, offset)
    except BaseException as exc:
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    finally:
        _thread_progress.sink = None
    _progress_phase(session_id, "parse")
    try:
        if rotated:
            lines, leftover = split_tail(b"", data)
            records = _records_from_lines(lines)
            new_bytes = len(data)
            new_offset = len(data)
            new_records = len(records)
        else:
            if size < offset:
                lines, leftover = split_tail(b"", delta)
                records = _records_from_lines(lines)
                new_bytes = len(delta)
                new_offset = len(delta)
                new_records = len(records)
            else:
                lines, leftover = split_tail(session["leftover"], delta)
                appended = _records_from_lines(lines)
                records = list(session["records"]) + appended
                new_bytes = len(delta)
                new_offset = offset + len(delta)
                new_records = len(appended)
        updated_at = _utc_iso()
        _session_update(session_id, session, cfg=cfg, offset=new_offset,
                        leftover=leftover, records=records, updated_at=updated_at)
        payload = analyze_records(records, files=[resolved])
        payload["session"] = session_id
        payload["ftp"] = {
            "path": resolved,
            "new_bytes": new_bytes,
            "new_records": new_records,
            "bytes": new_offset,
            "records": len(records),
            "updated_at": updated_at,
        }
    except BaseException as exc:
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    _progress_finish(session_id, ok=True)
    return payload


def ftp_close(session_id):
    """Drop an FTP session; returns ``True`` when it existed."""
    with _FTP_LOCK:
        return _FTP_SESSIONS.pop(session_id, None) is not None


def parse_json_body(data):
    """Decode a request body into a dict, raising ``ValueError`` on bad input."""
    if not data:
        return {}
    try:
        obj = json.loads(data.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError("invalid JSON body") from exc
    if not isinstance(obj, dict):
        raise ValueError("JSON body must be an object")
    return obj


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class _Handler(BaseHTTPRequestHandler):
    server_version = "HldsMonWebUI/" + str(PROTOCOL_VERSION)
    protocol_version = "HTTP/1.1"

    # -- logging ------------------------------------------------------------
    def log_message(self, fmt, *args):
        """Concise single line to stderr instead of the default noise."""
        try:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))
        except Exception:
            pass

    # -- helpers ------------------------------------------------------------
    def _send(self, code, body, content_type):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command == "HEAD":
            return
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def _send_json(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def _read_body(self):
        """Read the request body; ``None`` means it exceeds the cap."""
        raw = self.headers.get("Content-Length")
        if raw is None:
            return b""
        try:
            length = int(raw)
        except (TypeError, ValueError):
            return b""
        if length <= 0:
            return b""
        if length > MAX_BODY_BYTES:
            return None
        chunks = []
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    @staticmethod
    def _first(query, name, default=None):
        values = query.get(name)
        if not values:
            return default
        return values[0]

    @staticmethod
    def _query_files(query):
        """Parse the comma-separated ``files`` query param (display only)."""
        raw = query.get("files")
        if not raw:
            return []
        joined = ",".join(raw)
        return [part.strip() for part in joined.split(",") if part.strip()]

    # -- routing ------------------------------------------------------------
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/":
            self._serve_index()
        elif path == "/api/health":
            self._send_json(200, {
                "ok": True,
                "service": SERVICE_NAME,
                "version": PROTOCOL_VERSION,
            })
        elif path == "/api/ftp/progress":
            query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
            job = (query.get("id") or [""])[0]
            self._send_json(200, {"ok": True, "progress": progress_get(job)})
        else:
            self._not_found()

    def do_POST(self):
        parts = urlsplit(self.path)
        query = parse_qs(parts.query, keep_blank_values=True)
        if parts.path == "/api/analyze":
            self._handle_analyze(query)
        elif parts.path == "/api/export":
            self._handle_export(query)
        elif parts.path == "/api/ftp/connect":
            self._handle_ftp(ftp_connect)
        elif parts.path == "/api/ftp/refresh":
            self._handle_ftp_refresh()
        elif parts.path == "/api/ftp/close":
            self._handle_ftp_close()
        else:
            self.close_connection = True
            self._not_found()

    # -- endpoints ----------------------------------------------------------
    def _serve_index(self):
        html = None
        if os.path.isfile(INDEX_PATH):
            try:
                with open(INDEX_PATH, "r", encoding="utf-8",
                          errors="replace") as fh:
                    html = fh.read()
            except OSError:
                html = None
        if html is None:
            html = FALLBACK_HTML
        self._send(200, html, "text/html; charset=utf-8")

    def _handle_analyze(self, query):
        data = self._read_body()
        if data is None:
            self.close_connection = True
            self._send_json(413, {"ok": False, "error": "payload too large"})
            return
        files = self._query_files(query)
        try:
            payload = analyze_bytes(data, files)
        except Exception as exc:  # noqa: BLE001 - surface any analyzer error
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send_json(200, payload)

    def _handle_export(self, query):
        data = self._read_body()
        if data is None:
            self.close_connection = True
            self._send_json(413, {"ok": False, "error": "payload too large"})
            return
        rtype = self._first(query, "type", None)
        if rtype == "":
            rtype = None
        if rtype is not None and rtype not in hldsmon.KNOWN_TYPES:
            self._send_json(400, {
                "ok": False,
                "error": "unknown type: %r" % (rtype,),
            })
            return
        try:
            csv_text = export_bytes(data, rtype)
        except Exception as exc:  # noqa: BLE001
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send(200, csv_text, "text/csv; charset=utf-8")

    def _body_or_error(self):
        """Return the raw body, sending a 413/400 and ``None`` on failure."""
        data = self._read_body()
        if data is None:
            self.close_connection = True
            self._send_json(413, {"ok": False, "error": "payload too large"})
        return data

    def _handle_ftp(self, worker):
        data = self._body_or_error()
        if data is None:
            return
        try:
            spec = parse_json_body(data)
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        try:
            payload = worker(spec)
        except Exception as exc:  # noqa: BLE001 - never crash the server
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send_json(200, payload)

    def _handle_ftp_refresh(self):
        data = self._body_or_error()
        if data is None:
            return
        try:
            spec = parse_json_body(data)
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        session_id = spec.get("session")
        if not isinstance(session_id, str):
            self._send_json(400, {"ok": False, "error": "unknown session"})
            return
        try:
            payload = ftp_refresh(session_id)
        except KeyError:
            self._send_json(400, {"ok": False, "error": "unknown session"})
            return
        except Exception as exc:  # noqa: BLE001 - never crash the server
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send_json(200, payload)

    def _handle_ftp_close(self):
        data = self._body_or_error()
        if data is None:
            return
        try:
            spec = parse_json_body(data)
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        session_id = spec.get("session")
        if not isinstance(session_id, str) or not ftp_close(session_id):
            self._send_json(400, {"ok": False, "error": "unknown session"})
            return
        self._send_json(200, {"ok": True})

    def _not_found(self):
        self._send(404, "404 Not Found\n", "text/plain; charset=utf-8")


# ---------------------------------------------------------------------------
# Server / entry point
# ---------------------------------------------------------------------------

def make_server(host, port):
    """Create (but do not start) a threading HTTP server for the web UI.

    Pass ``port=0`` for an ephemeral port; the chosen port is available as
    ``server.server_address[1]``.
    """
    server = ThreadingHTTPServer((host, port), _Handler)
    server.daemon_threads = True
    return server


def build_parser():
    parser = argparse.ArgumentParser(
        prog="webui",
        description="Local offline web UI for HldsMonitoring JSONL telemetry.")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765,
                        help="TCP port; 0 picks a free port (default: 8765)")
    parser.add_argument("--open", action="store_true", dest="open_browser",
                        help=argparse.SUPPRESS)
    parser.add_argument("--no-open", action="store_false", dest="open_browser",
                        help="do not open the browser automatically")
    parser.set_defaults(open_browser=True)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    server = make_server(args.host, args.port)
    port = server.server_address[1]
    url = "http://%s:%d/" % (args.host, port)
    print("HldsMonitoring web UI: %s" % url, flush=True)
    if args.open_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
