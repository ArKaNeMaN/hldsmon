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
    bundle_bytes(data, drop_index, mins=2.0) -> dict
    make_server(host, port) -> http.server.ThreadingHTTPServer
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import ftplib
import io
import json
import os
import secrets
import shutil
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

#: Optional read valve in bytes (0 = unlimited). Off by default; overridable
#: via ``--max-log-mb`` / ``HLDSMON_WEBUI_MAX_MB``. This is the *only* size
#: guard, shared by the local request path and the FTP download path
#: (stage ``large-logs``; growth is bounded by streaming, not a cap).
_MAX_LOG_BYTES = 0

#: Maximum number of concurrent body uploads/sessions; the excess gets HTTP
#: 429 ``busy`` (a resource guard, not a log cap). Overridable via
#: ``--max-uploads`` / ``HLDSMON_WEBUI_MAX_UPLOADS``.
MAX_UPLOADS = 4

#: Socket read timeout (seconds) for request bodies and FTP reads. A body that
#: does not finish within it yields HTTP 408 ``request_timeout``. Overridable
#: via ``--read-timeout`` / ``HLDSMON_WEBUI_READ_TIMEOUT``.
READ_TIMEOUT_S = 30.0

#: Per-session disk-spool budget (MiB) - a resource guard, *not* a log size cap.
#: Exhaustion yields ``spool_full``. Overridable via ``--spool-session-mb``.
SPOOL_SESSION_MB = 2048

#: Process-wide disk-spool budget (MiB) - a resource guard, *not* a log cap.
#: Overridable via ``--spool-total-mb``.
SPOOL_TOTAL_MB = 8192

#: Spool handle/segment time-to-live (seconds) for the age sweeper (plus a
#: startup cleanup of orphans). Overridable via ``--spool-ttl-sec``.
SPOOL_TTL_S = 3600

#: Interval (seconds) of the background spool sweeper that frees expired
#: handles (file + budget + ``entry.analysis``) — the TTL-sweeper is really
#: run, not only startup cleanup (stage ``instant-period``, INV-I7).
SPOOL_SWEEP_S = 60

#: Per-handle analysis-session budget (MiB); exceeding it drops the window
#: memo and coarse rows while keeping the sparse index (INV-I6).
ANALYSIS_SESSION_MB = 16

#: LRU size of the per-handle window memo; key ``(from,to,window,sections)``
#: (owner decision O13).
MEMO_LRU = 8

#: Streaming chunk size (bytes) used while spooling a request body. Ingest
#: memory is O(SPOOL_CHUNK), independent of the body size.
SPOOL_CHUNK = 64 * 1024

_MIB = 1024 * 1024

#: UTF-8 byte-order mark, stripped exactly once at ingest (offset 0). Never
#: handled in ``hldsmon.iter_records`` - its global CLI/library behaviour is
#: left untouched.
_BOM = b"\xef\xbb\xbf"

#: Filesystem types treated as RAM-backed (rejected for the *default* spool
#: directory; an explicit ``--spool-dir`` is always honoured).
_TMPFS_FSTYPES = frozenset(("tmpfs", "ramfs", "devtmpfs", "hugetlbfs"))

#: Initial / per-step time window (seconds) loaded from the tail of the FTP log.
FTP_WINDOW_S = 1800.0

#: Bytes fetched per backward (older) range read.
FTP_BACK_CHUNK = 8 * 1024 * 1024

#: Selectable FTP window sizes in seconds (REQ-L6/H3): 5m / 15m / 30m / 1h /
#: 6h / 24h. ``FTP_WINDOW_S`` (30m) is the default offered by the UI; the
#: window is a UX/performance knob, never a size cap (design §28.3 п.10).
FTP_WINDOW_PRESETS = (300.0, 900.0, 1800.0, 3600.0, 21600.0, 86400.0)

#: Default socket timeout (seconds) for FTP connections.
DEFAULT_FTP_TIMEOUT = 30.0

#: Default analysis window (seconds) applied by ``/api/analyze`` when no
#: ``from``/``to`` is supplied: the last ``ANALYZE_DEFAULT_WINDOW_S`` seconds
#: ending at the newest record. Separate entity from ``FTP_WINDOW_S`` even
#: though the value coincides (plan LL-P2 window contract).
ANALYZE_DEFAULT_WINDOW_S = 1800.0

#: Maximum buckets per series row and the initial bucket width (mirrors
#: ``hldsmon``; stage ``instant-period``, O11 / INV-I10). ``SERIES_MAX_POINTS``
#: is kept as a backwards-compatible alias of ``CHART_MAX_POINTS``.
CHART_MAX_POINTS = hldsmon.CHART_MAX_POINTS
SERIES_MAX_POINTS = hldsmon.SERIES_MAX_POINTS
MIN_RESOLUTION_S = hldsmon.MIN_RESOLUTION_S

#: Sparse-index knobs (mirrors ``hldsmon``; stage ``instant-period``, INV-I1).
INDEX_STEP_S = hldsmon.INDEX_STEP_S
INDEX_STEP_BYTES = hldsmon.INDEX_STEP_BYTES
INDEX_MAX_ENTRIES = hldsmon.INDEX_MAX_ENTRIES

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


# ---------------------------------------------------------------------------
# Runtime configuration (flag/env overridable; see ``build_parser``/``main``)
# ---------------------------------------------------------------------------

class _Config:
    """Process-wide runtime knobs resolved from flags/env at startup."""

    def __init__(self):
        self.max_log_bytes = _MAX_LOG_BYTES
        self.spool_dir = None
        self.allow_nonloopback_unlimited = False
        self.max_uploads = MAX_UPLOADS
        self.read_timeout = READ_TIMEOUT_S
        self.spool_session_bytes = SPOOL_SESSION_MB * _MIB
        self.spool_total_bytes = SPOOL_TOTAL_MB * _MIB
        self.spool_ttl = SPOOL_TTL_S
        self.analysis_session_bytes = ANALYSIS_SESSION_MB * _MIB


_CONFIG = _Config()


class _HttpError(Exception):
    """An HTTP-level failure carrying a JSON payload and status code."""

    def __init__(self, status, payload, close=False):
        super().__init__(payload.get("code") or payload.get("error") or status)
        self.status = status
        self.payload = payload
        self.close = close


class _IngestError(Exception):
    """Base class for body-ingest failures mapped to an HTTP response."""

    status = 400
    code = "ingest_error"
    close = False

    def payload(self):
        return {"ok": False, "code": self.code}


class _RequestTimeout(_IngestError):
    status = 408
    code = "request_timeout"
    close = True


class _LogTooLarge(_IngestError):
    status = 413
    code = "log_too_large"

    def __init__(self, limit_bytes):
        super().__init__()
        self.limit_bytes = int(limit_bytes)

    def payload(self):
        return {"ok": False, "code": self.code, "limit_bytes": self.limit_bytes}


class _SpoolFull(_IngestError):
    status = 507
    code = "spool_full"


# ---------------------------------------------------------------------------
# Disk-spool: backing-directory choice, budget, handle registry
# ---------------------------------------------------------------------------

class _SpoolBudget:
    """Thread-safe process-wide byte budget shared by all spool files."""

    def __init__(self, total):
        self._total = int(total)
        self._used = 0
        self._lock = threading.Lock()

    @property
    def used(self):
        with self._lock:
            return self._used

    def reserve(self, n):
        n = int(n)
        if n <= 0:
            return True
        with self._lock:
            if self._total > 0 and self._used + n > self._total:
                return False
            self._used += n
            return True

    def release(self, n):
        n = int(n)
        if n <= 0:
            return
        with self._lock:
            self._used = max(0, self._used - n)


class AnalysisSession:
    """Bounded per-handle analysis cache (stage ``instant-period``, INV-I6).

    Holds only the sparse index, the whole-log scalar summary, and — filled by
    later phases — bucketed coarse rows and the window memo. Heavy full-day
    lists (drops/clients/sessions/identities/config/events/anomalies) are never
    stored here; they are rebuilt in the coarse response each time.
    """

    __slots__ = ("index", "t_lo", "t_hi", "segments", "monotonic",
                 "summary", "coarse_series", "memo", "lock")

    def __init__(self, index=None, summary=None):
        self.index = index
        self.t_lo = index.t_lo if index is not None else None
        self.t_hi = index.t_hi if index is not None else None
        self.segments = index.segments if index is not None else 0
        self.monotonic = index.monotonic if index is not None else True
        self.summary = summary
        self.coarse_series = None
        self.memo = collections.OrderedDict()
        self.lock = threading.Lock()


class SpoolHandle:
    """A server-owned spool file and its lifecycle metadata."""

    __slots__ = ("handle", "path", "created", "size", "analysis")

    def __init__(self, handle, path, created):
        self.handle = handle
        self.path = path
        self.created = created
        self.size = 0
        self.analysis = None


_SPOOL_BUDGET = _SpoolBudget(_CONFIG.spool_total_bytes)
_SPOOL_LOCK = threading.Lock()
_SPOOLS = {}

_SPOOL_PREFIX = "hldsmon-spool-"
_SPOOL_SUFFIX = ".jsonl"

_UPLOAD_SEM = threading.BoundedSemaphore(max(1, int(_CONFIG.max_uploads)))


def _read_proc_mounts():
    try:
        with open("/proc/mounts", "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def spool_backing_kind(mounts_text, path):
    """Classify the filesystem backing ``path`` from ``/proc/mounts`` text.

    Returns ``"tmpfs"`` for RAM-backed mounts, ``"disk"`` for any other real
    filesystem, or ``"unknown"`` when no mount entry matches. The longest
    matching mount point wins. Pure and host-testable.
    """
    if not path:
        return "unknown"
    target = os.path.abspath(path)
    best_len = -1
    best_fstype = None
    for line in (mounts_text or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        mountpoint = parts[1].replace("\\040", " ")
        fstype = parts[2]
        mp = mountpoint.rstrip("/") or "/"
        if mp == "/" or target == mp or target.startswith(mp + "/"):
            if len(mp) > best_len:
                best_len = len(mp)
                best_fstype = fstype
    if best_fstype is None:
        return "unknown"
    return "tmpfs" if best_fstype in _TMPFS_FSTYPES else "disk"


def choose_spool_dir(configured=None, mounts_text=None):
    """Pick the disk-backed spool directory.

    An explicit ``configured`` directory is always honoured. Otherwise the
    default ``<tempdir>/hldsmon-spool`` is used, falling back to
    ``<cwd>/.hldsmon-spool`` when the default resolves to a RAM-backed mount.
    """
    if configured:
        return configured
    default = os.path.join(tempfile.gettempdir(), "hldsmon-spool")
    if mounts_text is None:
        mounts_text = _read_proc_mounts()
    if spool_backing_kind(mounts_text, default) == "tmpfs":
        return os.path.join(os.getcwd(), ".hldsmon-spool")
    return default


def spool_free_bytes(path):
    """Return free bytes on the filesystem holding ``path``, or ``None``.

    Cross-platform: ``shutil.disk_usage`` (Windows and POSIX) first, then
    ``os.statvfs`` where the platform provides it. Returns ``None`` when
    neither is usable, so the caller skips the free-space pre-check instead of
    crashing (``os.statvfs`` does not exist on Windows).
    """
    try:
        return shutil.disk_usage(path).free
    except OSError:
        pass
    statvfs = getattr(os, "statvfs", None)
    if statvfs is not None:
        try:
            st = statvfs(path)
            return st.f_bavail * st.f_frsize
        except OSError:
            return None
    return None


def _spool_dir():
    base = choose_spool_dir(_CONFIG.spool_dir)
    os.makedirs(base, exist_ok=True)
    return base


def _spool_create():
    handle = secrets.token_urlsafe(24)
    path = os.path.join(_spool_dir(), _SPOOL_PREFIX + handle + _SPOOL_SUFFIX)
    entry = SpoolHandle(handle, path, time.time())
    with _SPOOL_LOCK:
        _SPOOLS[handle] = entry
    return entry


def _spool_commit(entry, size):
    with _SPOOL_LOCK:
        entry.size = int(size)


def _spool_remove(handle):
    with _SPOOL_LOCK:
        entry = _SPOOLS.pop(handle, None)
    if entry is not None:
        entry.analysis = None
        _SPOOL_BUDGET.release(entry.size)
        _silent_remove(entry.path)
    return entry


def _spool_write(entry, data, append=False):
    """Write ``data`` to a spool entry, reserving the process budget for it.

    ``append`` extends the file (a refresh appending only new remote bytes).
    Raises ``_SpoolFull`` when the process budget is exhausted. Memory is
    O(data), never the whole spool.
    """
    n = len(data)
    if n and not _SPOOL_BUDGET.reserve(n):
        raise _SpoolFull()
    try:
        with open(entry.path, "ab" if append else "wb") as fh:
            fh.write(data)
    except OSError:
        _SPOOL_BUDGET.release(n)
        raise
    _spool_commit(entry, entry.size + n)


def spool_get(handle):
    """Return a live handle, or ``None`` when unknown/expired (and reap it)."""
    now = time.time()
    entry = None
    with _SPOOL_LOCK:
        entry = _SPOOLS.get(handle)
        if entry is not None and _CONFIG.spool_ttl \
                and now - entry.created > _CONFIG.spool_ttl:
            _SPOOLS.pop(handle, None)
        else:
            return entry
    entry.analysis = None
    _SPOOL_BUDGET.release(entry.size)
    _silent_remove(entry.path)
    return None


def spool_sweep(now=None):
    """Reap handles older than the TTL; return the number removed.

    Frees each expired handle's file, process budget, and ``entry.analysis``
    (INV-I7). Called by the background sweeper started in :func:`main`.
    """
    now = time.time() if now is None else float(now)
    ttl = _CONFIG.spool_ttl
    expired = []
    with _SPOOL_LOCK:
        for handle, entry in list(_SPOOLS.items()):
            if ttl and now - entry.created > ttl:
                expired.append(_SPOOLS.pop(handle))
    for entry in expired:
        entry.analysis = None
        _SPOOL_BUDGET.release(entry.size)
        _silent_remove(entry.path)
    return len(expired)


class _Sweeper:
    """Periodic daemon thread running :func:`spool_sweep` (INV-I7)."""

    def __init__(self, interval):
        self.interval = float(interval)
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(
            target=self._run, name="hldsmon-spool-sweeper", daemon=True)
        self._thread.start()
        return self

    def _run(self):
        while not self._stop.wait(self.interval):
            try:
                spool_sweep()
            except Exception:  # never let the sweeper thread die
                pass

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


_SWEEPER = None


def start_sweeper(interval=None):
    """Start the background TTL sweeper thread; return its controller (INV-I7)."""
    global _SWEEPER
    stop_sweeper()
    _SWEEPER = _Sweeper(SPOOL_SWEEP_S if interval is None
                        else float(interval)).start()
    return _SWEEPER


def stop_sweeper():
    """Stop the background TTL sweeper thread, if running."""
    global _SWEEPER
    if _SWEEPER is not None:
        _SWEEPER.stop()
        _SWEEPER = None


def spool_startup_cleanup(spool_dir=None):
    """Remove orphaned spool files from a previous (possibly crashed) run."""
    directory = spool_dir or _spool_dir()
    removed = 0
    try:
        names = os.listdir(directory)
    except OSError:
        names = []
    for name in names:
        if name.startswith(_SPOOL_PREFIX) and name.endswith(_SPOOL_SUFFIX):
            if _silent_remove(os.path.join(directory, name)):
                removed += 1
    with _SPOOL_LOCK:
        _SPOOLS.clear()
    return removed


def _reset_upload_semaphore():
    global _UPLOAD_SEM
    _UPLOAD_SEM = threading.BoundedSemaphore(max(1, int(_CONFIG.max_uploads)))


def _reset_spool_budget():
    global _SPOOL_BUDGET
    _SPOOL_BUDGET = _SpoolBudget(_CONFIG.spool_total_bytes)


def _is_loopback(host):
    """Return ``True`` when ``host`` denotes a loopback bind address."""
    if host is None or not isinstance(host, str):
        return True
    h = host.strip().lower()
    if h in ("", "localhost", "::1", "::ffff:127.0.0.1"):
        return True
    if h.startswith("127."):
        return True
    return False


def _parse_content_length(raw):
    """Return a positive declared length, or ``None`` when absent/malformed.

    Per the deterministic ingest contract, a value that is not an integer
    greater than zero is treated as "no usable length" (read to EOF).
    """
    try:
        value = int(raw)
    except (TypeError, ValueError):
        return None
    return value if value > 0 else None


def _strip_bom(data):
    """Drop a single leading UTF-8 BOM from a byte string."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    if data.startswith(_BOM):
        return data[len(_BOM):]
    return data


def receive_body(rfile, dest_path, length, *, valve=0, session_limit=0,
                 budget=None, chunk=SPOOL_CHUNK):
    """Stream a request body from ``rfile`` into ``dest_path``.

    ``length`` is the declared ``Content-Length`` (int > 0) or ``None`` to read
    until EOF. A leading UTF-8 BOM is stripped once. Memory is O(chunk), never
    O(body). Raises ``_LogTooLarge`` (valve), ``_SpoolFull`` (budget) or
    ``_RequestTimeout`` (short/absent body, socket timeout).
    """
    written = 0
    pending = b""
    bom_checked = False

    def _emit(data):
        nonlocal written
        if not data:
            return
        if valve and written + len(data) > valve:
            raise _LogTooLarge(valve)
        if session_limit and written + len(data) > session_limit:
            raise _SpoolFull()
        if budget is not None and not budget.reserve(len(data)):
            raise _SpoolFull()
        fh.write(data)
        written += len(data)

    def _feed(data):
        nonlocal pending, bom_checked
        if bom_checked:
            _emit(data)
            return
        pending += data
        if len(pending) < len(_BOM):
            return
        payload = pending[len(_BOM):] if pending.startswith(_BOM) else pending
        pending = b""
        bom_checked = True
        _emit(payload)

    fh = open(dest_path, "wb")
    over = False
    try:
        try:
            if length is None:
                while True:
                    try:
                        block = rfile.read(chunk)
                    except TimeoutError as exc:
                        raise _RequestTimeout() from exc
                    if not block:
                        break
                    if not over and valve and written + len(block) > valve:
                        over = True
                    if over:
                        continue
                    _feed(block)
            else:
                remaining = length
                while remaining > 0:
                    try:
                        block = rfile.read(min(chunk, remaining))
                    except TimeoutError as exc:
                        raise _RequestTimeout() from exc
                    if not block:
                        if over:
                            break
                        raise _RequestTimeout()
                    remaining -= len(block)
                    if not over and valve and written + len(block) > valve:
                        over = True
                    if over:
                        continue
                    _feed(block)
            if over:
                raise _LogTooLarge(valve)
            if pending:
                _emit(pending)
            return written
        except BaseException:
            if budget is not None:
                budget.release(written)
            raise
    finally:
        try:
            fh.close()
        except OSError:
            pass

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
PROTOCOL_VERSION = 2

#: Canonical ``sections=`` keys (design §29.3 п.11 / R3-m1). Key == payload
#: field; ``identities`` is never a key (the field is ``client_identities``).
SECTION_KEYS = ("digest", "series", "host_series", "client_series",
                "client_identities", "clients", "sessions", "drops",
                "anomalies", "events", "config", "focus_points")


def _want(sections, key):
    """Return ``True`` when ``key`` is requested (``None`` sections = all)."""
    return sections is None or key in sections


def _parse_sections(raw):
    """Parse a comma-separated ``sections`` param; ``None``/empty => all.

    Raises ``ValueError`` for an unknown section key (mapped to HTTP 400).
    """
    if raw in (None, ""):
        return None
    keys = set()
    for part in str(raw).split(","):
        part = part.strip()
        if not part:
            continue
        if part not in SECTION_KEYS:
            raise ValueError("unknown section: %r" % (part,))
        keys.add(part)
    return keys or None

#: Ordered series accessors. Each maps a ``server`` record to a JSON value.
#: ``real_players`` is the wire name for the schema's ``real`` field (Appendix
#: P1). Missing / non-numeric fields yield ``None`` (JSON ``null``) so every
#: series array has exactly one entry per server record.
_SERIES_FIELDS = (
    "t", "frame_ms_min", "frame_ms_avg", "frame_ms_max",
    "cpu_pct", "rss_kb", "players", "real_players", "edicts",
    "fps", "in_kbps", "out_kbps",
)

#: ``host`` metric field -> ``{source, bit}`` of its caveat bit, sent to the UI
#: so caveated overlay points can be marked (mirror of ``hldsmon``/C header).
HOST_CAVEATS = {
    field: {"source": source, "bit": hldsmon.HOST_CAVEAT_BITS[source]}
    for field, source in hldsmon.HOST_FIELD_CAVEATS.items()
}

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
   <code>POST /api/analyze</code>, <code>POST /api/export</code>,
   <code>POST /api/bundle</code>.</p>
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
    # Windows: removing a still-open file raises PermissionError and this
    # returns False; eviction is then deferred to spool_startup_cleanup, which
    # is harmless because spool files are opened only for the duration of a
    # request (stage ``instant-period`` residual, design §29.3 п.3).
    try:
        os.remove(path)
        return True
    except OSError:
        return False


def _records_from_bytes(data):
    """Parse raw JSONL ``data`` via ``hldsmon`` using a temporary file.

    A leading UTF-8 BOM is stripped first (ingest-time contract). The temporary
    file is always removed, even if parsing raises.
    """
    data = _strip_bom(data)
    tmp = tempfile.NamedTemporaryFile(
        mode="wb", suffix=".jsonl", prefix="hldsmon-webui-", delete=False)
    path = tmp.name
    try:
        with tmp:
            tmp.write(data)
        return hldsmon.parse_records([path])
    finally:
        _silent_remove(path)


def build_series(records, *, max_points=CHART_MAX_POINTS):
    """Build a bucketed ``server`` series from ``records`` (single pass).

    Returns ``{"t": [bucket_start, ...], field: {min,max,avg,n}}`` — the shared
    bucket timestamp plus one ``{min,max,avg,n}`` triple per metric (INV-I10).
    A bucket with no numeric value for a metric yields ``None``/``n=0``.
    """
    builder = hldsmon._Bucketer(_SERIES_FIELDS, max_points)
    for rec in records:
        if rec.get("type") != "server":
            continue
        builder.add(hldsmon.server_series_value(rec, "t"),
                    {name: hldsmon.server_series_value(rec, name)
                     for name in _SERIES_FIELDS})
    return builder.finish()


def _bucket_client_series(flat, *, max_points=CHART_MAX_POINTS):
    """Bucket per-session flat ``client_series`` into the triple contract."""
    return {skey: hldsmon._bucket_flat(entry, max_points=max_points)
            for skey, entry in flat.items()}


def _bucket_host_series(flat, *, max_points=CHART_MAX_POINTS):
    """Bucket a flat ``host_series`` into the triple contract (nulls kept)."""
    return hldsmon._bucket_flat(flat, max_points=max_points)


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


def _drop_entry(rec, run, identities):
    """Build one wire drop entry, enriched with its session identity."""
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
        "key": {
            "run": int(run),
            "t": t,
            "userid": hldsmon._int_userid(rec.get("userid")),
        },
    }
    entry.update(_identity_fields(ident))
    return entry


def build_drops(records, identities=None):
    """Extract ``drop`` records (file order) into the wire shape.

    Each entry carries its ``session`` key (so the UI can navigate to the right
    client) and is enriched with the matching session's identity fields (when a
    ``client_identities`` lookup is supplied or derivable); absent fields are
    ``""`` (text) / ``None`` (numeric, boolean). Every entry also carries the
    stable ``key`` ``{run, t, userid}`` used to address a drop for
    ``/api/bundle``.
    """
    if identities is None:
        identities = hldsmon.client_identities(records)
    drops = []
    for rec, run in hldsmon._attach_runs(records):
        if rec.get("type") != "drop":
            continue
        drops.append(_drop_entry(rec, run, identities))
    return drops


def _iter_pairs(records):
    """Yield ``(record, run_index)`` streaming, mirroring ``_attach_runs``."""
    run = 0
    for rec in records:
        yield rec, run
        if rec.get("type") in ("shutdown", "crash"):
            run += 1


def _parse_epoch(value):
    """Return ``value`` as a float epoch second, or ``None`` when absent."""
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError("invalid epoch: %r" % (value,)) from exc


def _parse_window(value):
    """Return ``value`` as a positive window (seconds), or ``None``."""
    if value in (None, ""):
        return None
    try:
        seconds = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError("invalid window: %r" % (value,)) from exc
    if seconds <= 0:
        raise ValueError("window must be positive")
    return seconds


def _payload_from_agg(result, files, focus, series, client_series,
                      host_series, lo, hi, coarse, window_count,
                      sections=None):
    """Assemble the ``/api/analyze`` coarse-overview contract payload.

    ``sections`` (``None`` = all) gates which fields are included; the always-on
    core is ``ok``/``version``/``files``/``records_count``/``window``/``coarse``/
    ``window_count``/``log_end``.
    """
    counts = result["counts_by_type"]
    payload = {
        "ok": True,
        "version": PROTOCOL_VERSION,
        "files": files,
        "records_count": result["records"],
        "log_end": result["log_end"],
        "window": {"from": lo, "to": hi},
        "coarse": coarse,
        "window_count": window_count,
    }
    if _want(sections, "digest"):
        payload["digest"] = {
            "files": files,
            "records": result["records"],
            "counts_by_type": {k: counts.get(k, 0)
                               for k in hldsmon.KNOWN_TYPES},
            "period": result["period"],
            "players": result["players"],
            "sessions": result["session_count"],
            "drop_count": counts.get("drop", 0),
            "top_drop_reasons": result["top_drop_reasons"],
            "hitch_lines": counts.get("hitch", 0),
            "lag_spikes": result["lag_spikes"],
            "modnet": result["modnet_block"],
            "config": result["config_block"],
            "host": result["host_block"],
            "log_end": result["log_end"],
            "exclusions": result["exclusions"],
            "anomalies_excluded": len(result["anomalies_excluded"]),
            "focus_point_count": len(focus),
        }
    if _want(sections, "series"):
        payload["series"] = series
    if _want(sections, "client_series"):
        payload["client_series"] = client_series
    if _want(sections, "host_series"):
        payload["host_series"] = host_series
        payload["host_caveats"] = HOST_CAVEATS
    if _want(sections, "focus_points"):
        payload["focus_points"] = focus
    if _want(sections, "client_identities"):
        payload["client_identities"] = result["identities"]
    if _want(sections, "clients"):
        payload["clients"] = result["clients"]
    if _want(sections, "sessions"):
        payload["sessions"] = result["player_groups"]
        payload["session_count"] = result["session_count"]
    if _want(sections, "drops"):
        identities = result["identities"]
        payload["drops"] = [_drop_entry(rec, run, identities)
                            for rec, run in result["drops_raw"]]
    if _want(sections, "anomalies"):
        payload["anomalies"] = result["anomalies"]
    if _want(sections, "events"):
        payload["events"] = result["exclusions"]
        payload["anomalies_excluded"] = result["anomalies_excluded"]
    if _want(sections, "config"):
        payload["config"] = result["config_series"]
    return payload


def _analyze_iter(pairs, files, from_ts, to_ts, window_s, sections=None):
    """One-pass streaming analysis of ``(record, run)`` pairs.

    The whole spool is folded through a :class:`hldsmon.StreamAggregator` while
    only the requested window (``[from, to]`` inclusive, or the default
    ``ANALYZE_DEFAULT_WINDOW_S`` ending at the newest record) is materialized
    for full-resolution series/``focus_points``. Raw records are never all
    retained. ``sections`` (``None`` = all) gates both the per-record work and
    the returned fields (REQ-I10).
    """
    files_list = list(files) if files else []
    size = window_s if (window_s and window_s > 0) else ANALYZE_DEFAULT_WINDOW_S
    coarse = from_ts is None and to_ts is None
    if not coarse and from_ts is None:
        from_ts = float(to_ts) - size
    if not coarse and to_ts is None:
        to_ts = float(from_ts) + size

    agg = hldsmon.StreamAggregator(
        server_fields=_SERIES_FIELDS,
        client_fields=hldsmon._SERIES_KEYS,
        host_fields=("t",) + hldsmon.HOST_FIELDS + ("present_mask",
                                                    "caveat_mask"),
        max_points=CHART_MAX_POINTS, sections=sections)

    window_pairs = []
    if coarse:
        rolling = collections.deque()
        t_hi = None
        for rec, run in pairs:
            agg.add(rec, run)
            t = hldsmon._record_ts(rec)
            if t is None:
                continue
            if t_hi is None or t > t_hi:
                t_hi = t
            cutoff = t_hi - size
            while rolling and rolling[0][0] < cutoff:
                rolling.popleft()
            if t >= cutoff:
                rolling.append((t, rec, run))
        window_pairs = [(rec, run) for _, rec, run in rolling]
        result = agg.finish()
        end = result["period"]["end"]
        lo = end - size if end is not None else None
        hi = end
    else:
        lo, hi = float(from_ts), float(to_ts)
        for rec, run in pairs:
            agg.add(rec, run)
            t = hldsmon._record_ts(rec)
            if t is not None and lo <= t <= hi:
                window_pairs.append((rec, run))
        result = agg.finish()

    window_records = [rec for rec, _ in window_pairs]
    window_runs = [run for _, run in window_pairs]
    focus = (hldsmon.focus_points(window_records, runs=window_runs)
             if _want(sections, "focus_points") else [])
    if coarse:
        series = result["series"]
        client_series = result["client_series"]
        host_series = result["host_series"]
    else:
        series = (build_series(window_records)
                  if _want(sections, "series") else {})
        client_series = (
            _bucket_client_series(hldsmon.client_series(window_records,
                                                        runs=window_runs))
            if _want(sections, "client_series") else {})
        host_series = (
            _bucket_host_series(hldsmon.host_series(window_records))
            if _want(sections, "host_series") else {})
    return _payload_from_agg(result, files_list, focus, series, client_series,
                             host_series, lo, hi, coarse, len(window_pairs),
                             sections)


def analyze_records(records, files=None, *, from_ts=None, to_ts=None,
                    window_s=None, sections=None):
    """Return the ``/api/analyze`` payload for parsed ``records``."""
    return _analyze_iter(_iter_pairs(records), files, from_ts, to_ts, window_s,
                         sections)


def analyze_bytes(data, files=None):
    """Return the ``/api/analyze`` JSON payload for raw JSONL ``data``."""
    records = _records_from_bytes(data)
    return analyze_records(records, files)


def _iter_spool_pairs(paths, index_builder=None, on_progress=None):
    """Yield ``(record, run)`` across ordered spool ``paths``, runs continuous.

    The run index keeps counting across segment boundaries (a ``shutdown`` or
    ``crash`` in one segment advances the run for the next), so a windowed
    analysis of a segmented spool matches a single-file reference. When
    ``index_builder`` is given, each record also feeds the sparse index with its
    byte offset and segment index in the same pass (REQ-I2).
    """
    if isinstance(paths, (str, bytes)):
        paths = [paths]
    run = 0
    for segment_idx, path in enumerate(paths):
        for rec, _run, offset, _seg in hldsmon.iter_records_offsets(
                path, on_progress=on_progress):
            if index_builder is not None:
                index_builder.add(
                    hldsmon._record_ts(rec), run, offset, segment_idx)
            yield rec, run
            if rec.get("type") in ("shutdown", "crash"):
                run += 1


#: Rough per-sample in-memory footprint (bytes) for the session budget estimate.
_INDEX_SAMPLE_BYTES = 64


def analysis_session_bytes(analysis):
    """Estimate the in-memory size (bytes) of an :class:`AnalysisSession`."""
    if analysis is None:
        return 0
    total = 0
    if analysis.index is not None:
        total += len(analysis.index.samples) * _INDEX_SAMPLE_BYTES
    if analysis.summary is not None:
        total += len(json.dumps(analysis.summary, default=str))
    if analysis.coarse_series is not None:
        total += len(json.dumps(analysis.coarse_series, default=str))
    for fragment in analysis.memo.values():
        total += len(json.dumps(fragment, default=str))
    return total


def _enforce_analysis_budget(analysis):
    """Drop memo/coarse rows when over the per-handle budget; keep the index."""
    if analysis is None:
        return False
    if analysis_session_bytes(analysis) <= _CONFIG.analysis_session_bytes:
        return False
    analysis.memo = collections.OrderedDict()
    analysis.coarse_series = None
    return True


def _build_analysis_session(payload, index):
    """Build the bounded per-handle session from a coarse payload + index."""
    digest = payload.get("digest") or {}
    summary = {
        "files": payload.get("files") or [],
        "records": digest.get("records"),
        "counts_by_type": digest.get("counts_by_type"),
        "period": digest.get("period"),
        "players": digest.get("players") or [],
        "players_count": len(digest.get("players") or []),
        "session_count": payload.get("session_count"),
        "drop_count": digest.get("drop_count"),
        "top_drop_reasons": digest.get("top_drop_reasons"),
        "hitch_lines": digest.get("hitch_lines"),
        "lag_spikes": digest.get("lag_spikes"),
        "log_end": digest.get("log_end"),
        "anomalies_excluded": digest.get("anomalies_excluded"),
    }
    analysis = AnalysisSession(index=index, summary=summary)
    _enforce_analysis_budget(analysis)
    return analysis


def _iter_spool_records(paths):
    """Yield records in order across an ordered list of spool ``paths``."""
    if isinstance(paths, (str, bytes)):
        paths = [paths]
    for path in paths:
        for rec in hldsmon.iter_records(path):
            yield rec


def coarse_overview(path, files=None, *, from_ts=None, to_ts=None,
                    window_s=None, on_progress=None, index_builder=None,
                    sections=None):
    """Stream the spool at ``path`` and return the coarse-overview payload.

    ``on_progress`` (optional) is threaded to the parser as its byte-offset
    reporter (local-file parse progress). When ``index_builder`` is given, the
    sparse index is built in the **same** pass over the spool (REQ-I2).
    ``sections`` (``None`` = all) gates computed/returned fields (REQ-I10).
    """
    if index_builder is None:
        pairs = _iter_pairs(hldsmon.iter_records(path, on_progress=on_progress))
    else:
        pairs = _iter_spool_pairs([path], index_builder=index_builder,
                                  on_progress=on_progress)
    return _analyze_iter(pairs, files, from_ts, to_ts, window_s, sections)


def coarse_overview_paths(paths, files=None, *, from_ts=None, to_ts=None,
                          window_s=None, sections=None):
    """Coarse-overview payload across an ordered list of spool segments."""
    return _analyze_iter(_iter_spool_pairs(paths), files,
                         from_ts, to_ts, window_s, sections)


def _scan_window(path, offset, run, lo, hi):
    """Read ``[lo, hi]`` from ``path`` starting at byte ``offset`` with ``run``.

    ``run`` is the run index of the record at ``offset``; it is advanced on
    every read ``shutdown``/``crash`` (including time-filtered records), so the
    window is run-correct even when it starts after a run boundary. Reading
    stops at the first timed record beyond ``hi`` (INV-I4/I9).
    """
    out = []
    with open(path, "rb") as fh:
        fh.seek(offset)
        while True:
            start = fh.tell()
            raw = fh.readline()
            if not raw:
                break
            if start == 0 and raw.startswith(_BOM):
                raw = raw[len(_BOM):]
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except (ValueError, TypeError):
                continue
            if not isinstance(obj, dict):
                continue
            rtype = obj.get("type")
            if not isinstance(rtype, str) or rtype not in hldsmon._KNOWN_TYPES:
                continue
            t = hldsmon._record_ts(obj)
            if t is not None and hi is not None and t > hi:
                break
            if t is not None and (lo is None or t >= lo):
                out.append((obj, run))
            if rtype in ("shutdown", "crash"):
                run += 1
    return out


def window_records(path, from_ts, to_ts, *, index=None):
    """Materialize ``[(record, run), ...]`` within ``[from, to]`` inclusive.

    When ``index`` (a :class:`hldsmon.IndexBuilder`) is supplied, the spool is
    seeked to the greatest sample with ``t <= from`` and the sample's ``run``
    seeds the run counter (O(window)). The fast path is taken only when the
    chosen seek sample lies strictly beyond every ``t`` inversion recorded by
    the builder (``index.last_inv_t``): from there the stream is monotonic, so
    the ``t > to`` early stop is sound. An early startup backward step thus
    keeps the index usable for every later window; only a window that still
    crosses the inversion falls back to a full run-aware scan (INV-I3/I8).
    """
    lo = hldsmon._num(from_ts)
    hi = hldsmon._num(to_ts)
    if index is not None and index.samples and lo is not None:
        sample_ts = [s[0] for s in index.samples]
        pos = bisect.bisect_right(sample_ts, lo) - 1
        if pos >= 0:
            last_inv = getattr(index, "last_inv_t", None)
            sample = index.samples[pos]
            if last_inv is None or (sample[0] > last_inv and lo > last_inv):
                _t, _seg, offset, run = sample
                return _scan_window(path, offset, run, lo, hi)
    out = []
    for rec, run in _iter_pairs(hldsmon.iter_records(path)):
        t = hldsmon._record_ts(rec)
        if t is None:
            continue
        if lo is not None and t < lo:
            continue
        if hi is not None and t > hi:
            continue
        out.append((rec, run))
    return out


def _window_payload(handle, analysis, lo, hi, window_count, series,
                    client_series, host_series, focus, sections):
    """Assemble a lightweight windowed ``/api/analyze`` response (N-M2).

    Carries only ``handle``/``window``/``coarse:false`` + the windowed series
    and ``focus_points`` + the **whole-log** scalar digest from the cached
    summary (``period`` is ``[t_lo, t_hi]``, never the window). Heavy full-day
    lists are omitted; the frontend merges them from the coarse response.
    """
    summary = analysis.summary if analysis is not None else {}
    payload = {
        "ok": True,
        "version": PROTOCOL_VERSION,
        "files": summary.get("files") or [],
        "handle": handle,
        "coarse": False,
        "window": {"from": lo, "to": hi},
        "window_count": window_count,
        "records_count": summary.get("records"),
        "log_end": summary.get("log_end"),
    }
    if _want(sections, "digest"):
        payload["digest"] = {
            "files": summary.get("files") or [],
            "records": summary.get("records"),
            "counts_by_type": summary.get("counts_by_type"),
            "period": summary.get("period"),
            "players": summary.get("players"),
            "sessions": summary.get("session_count"),
            "drop_count": summary.get("drop_count"),
            "top_drop_reasons": summary.get("top_drop_reasons"),
            "hitch_lines": summary.get("hitch_lines"),
            "lag_spikes": summary.get("lag_spikes"),
            "log_end": summary.get("log_end"),
            "anomalies_excluded": summary.get("anomalies_excluded"),
            "focus_point_count": len(focus),
        }
    if _want(sections, "series"):
        payload["series"] = series
    if _want(sections, "client_series"):
        payload["client_series"] = client_series
    if _want(sections, "host_series"):
        payload["host_series"] = host_series
        payload["host_caveats"] = HOST_CAVEATS
    if _want(sections, "focus_points"):
        payload["focus_points"] = focus
    return payload


def serve_window(entry, from_ts, to_ts, window_s, sections=None):
    """Serve ``[from, to]`` from a spool's cached analysis session (REQ-I1/I3).

    Seeks the spool via the cached sparse index (falling back to a full scan
    when the index is missing or non-monotonic, INV-I8); returns the windowed
    lightweight response. Raises ``OSError`` if the spool file vanished
    (caller maps that to ``session_expired``).
    """
    analysis = getattr(entry, "analysis", None)
    size = window_s if (window_s and window_s > 0) else ANALYZE_DEFAULT_WINDOW_S
    lo, hi = from_ts, to_ts
    if lo is None:
        lo = float(hi) - size
    if hi is None:
        hi = float(lo) + size
    index = analysis.index if analysis is not None else None
    pairs = window_records(entry.path, lo, hi, index=index)
    records = [rec for rec, _ in pairs]
    runs = [run for _, run in pairs]
    series = (build_series(records) if _want(sections, "series") else {})
    client_series = (
        _bucket_client_series(hldsmon.client_series(records, runs=runs))
        if _want(sections, "client_series") else {})
    host_series = (_bucket_host_series(hldsmon.host_series(records))
                   if _want(sections, "host_series") else {})
    focus = (hldsmon.focus_points(records, runs=runs)
             if _want(sections, "focus_points") else [])
    return _window_payload(entry.handle, analysis, lo, hi, len(pairs), series,
                           client_series, host_series, focus, sections)


def export_records(records, rtype=None):
    """Return CSV text for parsed ``records`` (optionally one record type).

    Raises ``ValueError`` for an unknown ``rtype``.
    """
    if rtype is not None and rtype not in hldsmon.KNOWN_TYPES:
        raise ValueError("unknown type: %r" % (rtype,))
    buf = io.StringIO()
    hldsmon.export_csv(records, rtype=rtype, stream=buf)
    return buf.getvalue()


def export_bytes(data, rtype=None):
    """Return CSV text for raw JSONL ``data`` (optionally one record type)."""
    return export_records(_records_from_bytes(data), rtype)


def bundle_records(records, drop_index, mins=2.0):
    """Return the incident-bundle envelope for parsed ``records``.

    ``drop_index`` is the 0-based position of the drop record in file order
    (the same order the ``drops`` payload uses). Raises ``ValueError`` when the
    log has no drops, the index is out of range, or ``mins`` is invalid.
    """
    if isinstance(drop_index, bool) or not isinstance(drop_index, int):
        raise ValueError("drop index must be an integer")
    drops = [r for r in records if r.get("type") == "drop"]
    if not drops:
        raise ValueError("no drop records in the log")
    if not 0 <= drop_index < len(drops):
        raise ValueError("drop index out of range: %d" % drop_index)
    return hldsmon.incident_bundle(records, drops[drop_index], mins=mins)


def bundle_bytes(data, drop_index, mins=2.0):
    """Return the incident-bundle envelope for raw JSONL ``data``."""
    return bundle_records(_records_from_bytes(data), drop_index, mins)


def _same_userid(value, expected):
    return hldsmon._int_userid(value) == expected


def bundle_from_spool(paths, run, t, userid, mins=2.0):
    """Build an incident bundle from the spool using the ``(run,t,userid)`` key.

    ``paths`` may be one spool path or an ordered list of segments. Streams the
    spool once, materializing only the ``[t-mins, t+mins]`` window (plus the
    last config snapshot at or before the window, for the diff baseline).
    Raises ``ValueError`` when the drop key matches no record.
    """
    try:
        run = int(run)
        center = float(t)
        mins = float(mins)
    except (TypeError, ValueError) as exc:
        raise ValueError("invalid drop key") from exc
    if mins < 0:
        raise ValueError("mins must be non-negative")
    low = center - mins * 60.0
    high = center + mins * 60.0
    baseline = None
    window = []
    drop_rec = None
    for rec, rec_run in _iter_pairs(_iter_spool_records(paths)):
        rtype = rec.get("type")
        rt = hldsmon._record_ts(rec)
        if rtype == "config" and rt is not None and rt <= low:
            baseline = rec
        if rt is not None and low <= rt <= high:
            window.append(rec)
        if (drop_rec is None and rtype == "drop" and rec_run == run
                and rt is not None and abs(rt - center) < 1e-6
                and _same_userid(rec.get("userid"), userid)):
            drop_rec = rec
    if drop_rec is None:
        raise ValueError("drop not found for key run=%d t=%s userid=%s"
                         % (run, t, userid))
    records = list(window)
    if baseline is not None and hldsmon._record_ts(baseline) < low:
        records = [baseline] + records
    return hldsmon.incident_bundle(records, drop_rec, mins=mins)


def _window_matches(rec, lo, hi):
    if lo is None and hi is None:
        return True
    t = hldsmon._record_ts(rec)
    if t is None:
        return False
    if lo is not None and t < lo:
        return False
    if hi is not None and t > hi:
        return False
    return True


def export_spool_chunks(paths, rtype=None, lo=None, hi=None, chunk_size=65536):
    """Yield CSV byte chunks for the spool, filtered by type/window.

    ``paths`` may be one spool path or an ordered list of segments. Two
    streaming passes (field discovery, then rows); memory is O(fields) plus one
    chunk, never O(records). Raises ``ValueError`` for an unknown ``rtype``.
    """
    if rtype is not None and rtype not in hldsmon.KNOWN_TYPES:
        raise ValueError("unknown type: %r" % (rtype,))
    fieldnames = ["type", "t"]
    seen = {"type", "t"}
    extra = set()
    for rec in _iter_spool_records(paths):
        if rtype is not None and rec.get("type") != rtype:
            continue
        if not _window_matches(rec, lo, hi):
            continue
        extra.update(rec.keys())
    for key in sorted(extra):
        if key not in seen:
            fieldnames.append(key)
            seen.add(key)

    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    pending = buf.getvalue().encode("utf-8")
    buf.seek(0)
    buf.truncate(0)
    for rec in _iter_spool_records(paths):
        if rtype is not None and rec.get("type") != rtype:
            continue
        if not _window_matches(rec, lo, hi):
            continue
        writer.writerow({k: hldsmon._csv_value(rec.get(k))
                         for k in fieldnames})
        if buf.tell() >= chunk_size:
            pending += buf.getvalue().encode("utf-8")
            buf.seek(0)
            buf.truncate(0)
            yield pending
            pending = b""
    pending += buf.getvalue().encode("utf-8")
    if pending:
        yield pending


# ---------------------------------------------------------------------------
# FTP live-tail (stdlib ftplib only; credentials in RAM only)
# ---------------------------------------------------------------------------

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


class _ValveHit(Exception):
    """Internal signal: the optional read valve clipped an FTP stream."""


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
    ftp.voidcmd("TYPE I")
    return ftp


def _retr_into_file(ftp, path, dest_path, offset=None, on_bytes=None,
                    max_bytes=None):
    """Stream a binary RETR into ``dest_path``; memory is O(chunk), not O(file).

    ``offset`` (REST) skips to a remote byte offset. ``max_bytes`` clips the
    transfer at the optional read valve and returns ``(written, cap_hit)`` so
    an over-valve read is reported, never silently truncated.
    """
    total = [0]
    hit = [False]

    def _sink(block):
        room = None if max_bytes is None else max_bytes - total[0]
        if room is not None and len(block) > room:
            if room > 0:
                fh.write(block[:room])
                total[0] += room
            hit[0] = True
            raise _ValveHit()
        fh.write(block)
        total[0] += len(block)
        if on_bytes is not None:
            on_bytes(total[0])

    with open(dest_path, "wb") as fh:
        try:
            if offset is None:
                ftp.retrbinary("RETR " + path, _sink)
            else:
                ftp.retrbinary("RETR " + path, _sink, rest=offset)
        except _ValveHit:
            pass
    return total[0], hit[0]


def _download(cfg, dest_path, offset=0, on_bytes=None, max_bytes=None):
    """Stream remote ``cfg['path']`` (from ``offset``) into ``dest_path``.

    Returns ``(written, cap_hit)``. This is the network seam tests replace; the
    production path writes to disk so memory stays O(chunk), never O(file).
    """
    offset = int(offset or 0)
    ftp = _ftp_open(cfg)
    try:
        return _retr_into_file(ftp, cfg["path"], dest_path,
                               offset or None, on_bytes, max_bytes)
    finally:
        _ftp_close(ftp)


def _ftp_close(ftp):
    try:
        ftp.quit()
    except Exception:
        try:
            ftp.close()
        except Exception:
            pass


def _remote_size(cfg):
    """Return the remote file size in bytes, or ``None`` if SIZE is unsupported."""
    ftp = _ftp_open(cfg)
    try:
        try:
            return int(ftp.size(cfg["path"]))
        except Exception:
            return None
    finally:
        _ftp_close(ftp)


def _download_range(cfg, start, length):
    """Return the bytes of ``cfg['path']`` in ``[start, start + length)``.

    Uses an FTP REST range read; when the server lacks REST/SIZE the full RETR
    is streamed into a temporary spool file and the slice read from it, so a
    large file is never held in memory. Tolerates a short transfer.
    """
    start = int(start)
    length = int(length)
    if length <= 0:
        return b""
    ftp = _ftp_open(cfg)
    tmp_path = None
    try:
        path = cfg["path"]
        try:
            size = ftp.size(path)
        except Exception:
            size = None
        stop = start + length
        if size is not None and size < stop:
            stop = size
        if stop <= start:
            return b""
        try:
            conn = ftp.transfercmd("RETR " + path, rest=start)
        except ftplib.all_errors:
            conn = None
        if conn is not None:
            buf = bytearray()
            want = stop - start
            try:
                while len(buf) < want:
                    block = conn.recv(min(65536, want - len(buf)))
                    if not block:
                        break
                    buf += block
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except Exception:
                    pass
            try:
                ftp.voidresp()
            except Exception:
                pass
            return bytes(buf)
        tmp = tempfile.NamedTemporaryFile(
            suffix=".spool", prefix="hldsmon-ftp-", delete=False)
        tmp_path = tmp.name
        tmp.close()
        _retr_into_file(ftp, path, tmp_path)
        with open(tmp_path, "rb") as fh:
            fh.seek(start)
            return fh.read(stop - start)
    finally:
        if tmp_path:
            _silent_remove(tmp_path)
        _ftp_close(ftp)


def _scan_t_range_file(path):
    """Return ``(min_t, max_t)`` over a spool file, streamed (O(1) memory)."""
    lo = hi = None
    for rec in hldsmon.iter_records(path):
        t = hldsmon._num(rec.get("t"))
        if t is None:
            continue
        if lo is None or t < lo:
            lo = t
        if hi is None or t > hi:
            hi = t
    return lo, hi


def _segment_make(data, start, end, remote_path):
    """Spool ``data`` as a new segment covering remote ``[start, end)``."""
    entry = _spool_create()
    try:
        _spool_write(entry, data)
    except BaseException:
        _spool_remove(entry.handle)
        raise
    return {"entry": entry, "path": entry.path, "size": len(data),
            "start": int(start), "end": int(end), "remote_path": remote_path}


def _remove_segments(segments):
    """Release every segment's spool file and budget reservation."""
    for seg in segments or []:
        _spool_remove(seg["entry"].handle)


def _read_backward_segment(cfg, cursor, job, loaded, keep_tail_partial,
                           remaining=None, progress_total=None):
    """Fetch one chunk ending at remote ``cursor`` into a spool segment.

    Returns ``(aligned_start, segment, leftover, t_lo, t_hi, cap_hit)``. The
    leading partial line is dropped, so concatenating segments oldest->newest
    yields a gap-free, duplicate-free stream. Memory is O(chunk).
    """
    fetch_start = max(0, cursor - FTP_BACK_CHUNK)
    desired = cursor - fetch_start
    want = desired
    if remaining is not None:
        want = min(desired, max(0, remaining - loaded[0]))
    if want <= 0:
        return cursor, None, b"", None, None, remaining is not None
    clipped = remaining is not None and want < desired
    chunk = _download_range(cfg, fetch_start, want)
    loaded[0] += len(chunk)
    if fetch_start > 0:
        nl = chunk.find(b"\n")
        aligned_start = fetch_start + nl + 1 if nl >= 0 else fetch_start
    else:
        aligned_start = 0
    payload = chunk[aligned_start - fetch_start:]
    leftover = b""
    if keep_tail_partial and payload and not payload.endswith(b"\n"):
        nl = payload.rfind(b"\n")
        leftover = payload[nl + 1:] if nl >= 0 else payload
    seg = None
    t_lo = t_hi = None
    if payload:
        seg = _segment_make(payload, aligned_start, cursor, cfg["path"])
        t_lo, t_hi = _scan_t_range_file(seg["path"])
    _progress_set(job, loaded[0], progress_total)
    return aligned_start, seg, leftover, t_lo, t_hi, clipped


def _load_unsized(cfg, job, remaining=None):
    """No remote SIZE: stream the whole RETR once into a single segment.

    The tail window is then applied by the streaming analysis; ranges are
    sliced locally, so a large remote file is never held in memory nor rejected
    by size (REQ-L1/REQ-L2).
    """
    entry = _spool_create()
    try:
        written, cap = _download(cfg, entry.path, 0,
                                 _progress_callback(None), remaining)
    except BaseException:
        _spool_remove(entry.handle)
        raise
    if written and not _SPOOL_BUDGET.reserve(written):
        _spool_remove(entry.handle)
        raise _SpoolFull()
    _spool_commit(entry, written)
    seg = {"entry": entry, "path": entry.path, "size": written,
           "start": 0, "end": written, "remote_path": cfg["path"]}
    t_lo, t_hi = _scan_t_range_file(entry.path)
    _progress_set(job, written, written if written else None)
    return [seg], b"", t_lo, t_hi, cap


def _load_tail(cfg, window_s, job, remaining=None):
    """Load the newest ``window_s`` seconds into ordered spool segments.

    Returns ``(segments, leftover, t_lo, t_hi, cap_hit)`` with ``segments``
    oldest->newest. Ingest memory is O(chunk); the analysis later applies the
    window, so nothing is held proportional to the file.
    """
    size = _remote_size(cfg)
    if size is None or size <= 0:
        return _load_unsized(cfg, job, remaining)
    end = int(size)
    cursor = end
    loaded = [0]
    first = True
    leftover = b""
    newest_first = []
    t_lo = t_hi = None
    cap_hit = False
    while True:
        prev_cursor = cursor
        aligned_start, seg, chunk_leftover, s_lo, s_hi, cap = \
            _read_backward_segment(cfg, cursor, job, loaded,
                                   keep_tail_partial=first,
                                   remaining=remaining,
                                   progress_total=size)
        if seg is not None:
            newest_first.append(seg)
        if s_lo is not None and (t_lo is None or s_lo < t_lo):
            t_lo = s_lo
        if s_hi is not None and (t_hi is None or s_hi > t_hi):
            t_hi = s_hi
        if first:
            leftover = chunk_leftover
            first = False
        if cap:
            cap_hit = True
        cursor = aligned_start
        if t_lo is not None and t_hi is not None and (t_hi - t_lo) >= window_s:
            break
        if cursor <= 0 or cursor >= prev_cursor or seg is None:
            break
    newest_first.reverse()
    return newest_first, leftover, t_lo, t_hi, cap_hit


def _ftp_meta(path, segments, t_lo, t_hi, records_count, updated_at,
              cap_hit=False):
    """Build the ``ftp`` block shared by connect / refresh / load-earlier.

    ``bytes`` is the loaded **remote** byte range (a sum of segment spans, not
    the spool size) and ``reached_start`` is true once the oldest loaded
    segment starts at remote offset 0 (INV-L5). ``cap_hit``/``limit_bytes``
    appear only when the optional read valve is on (QAS-L4).
    """
    start = segments[0]["start"] if segments else 0
    end = segments[-1]["end"] if segments else 0
    remote_bytes = sum(int(seg["size"]) for seg in segments)
    meta = {
        "path": path,
        "bytes": int(remote_bytes),
        "records": int(records_count),
        "updated_at": updated_at,
        "reached_start": bool(segments) and int(segments[0]["start"]) <= 0,
        "start_t": t_lo,
        "end_t": t_hi,
        "start": int(start),
        "end": int(end),
        "segments": len(segments),
    }
    if _CONFIG.max_log_bytes:
        meta["cap_hit"] = bool(cap_hit)
        meta["limit_bytes"] = int(_CONFIG.max_log_bytes)
        if cap_hit:
            meta["truncated"] = True
    return meta


def _session_put(session_id, session):
    with _FTP_LOCK:
        _FTP_SESSIONS[session_id] = session


def _session_peek(session_id):
    with _FTP_LOCK:
        return _FTP_SESSIONS.get(session_id)


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


def _progress_set(job, done, total, phase="download"):
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
            "phase": str(phase),
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


def _ftp_valve_remaining(session):
    """Bytes still allowed by the optional read valve, or ``None`` when off."""
    valve = _CONFIG.max_log_bytes
    if not valve:
        return None
    return max(0, int(valve) - int(session.get("spool_bytes", 0)))


def _ftp_analyze(session):
    """Run the LL-P2 streaming/windowed analysis over the session's segments.

    The whole segment spool is streamed through the coarse aggregator, but only
    the FTP window is materialized, so memory stays O(window) even after
    ``load-earlier`` reaches remote offset 0 (INV-L1, no O(n^2) re-analysis).
    """
    paths = [seg["path"] for seg in session.get("segments", [])]
    files = [session["current_path"]] if session.get("current_path") else []
    payload = coarse_overview_paths(paths, files,
                                    window_s=session.get("window_s"))
    payload["handle"] = session["id"]
    payload["session"] = session["id"]
    return payload


def _ftp_seal(session, payload):
    """Record analysis-derived span/records/reached_start on the session."""
    period = (payload.get("digest") or {}).get("period") or {}
    session["t_lo"] = period.get("start")
    session["t_hi"] = period.get("end")
    session["records_count"] = int(payload.get("records_count") or 0)
    session["spool_bytes"] = sum(int(seg["size"]) for seg in session["segments"])
    session["reached_start"] = bool(session["segments"]) and \
        int(session["segments"][0]["start"]) <= 0


def _ftp_meta_for(session, cap_hit=None):
    meta = _ftp_meta(
        session.get("current_path"), session["segments"],
        session["t_lo"], session["t_hi"], session["records_count"],
        session.get("updated_at"),
        session.get("cap_hit") if cap_hit is None else cap_hit)
    meta["window_s"] = session.get("window_s")
    meta["window_presets"] = list(FTP_WINDOW_PRESETS)
    return meta


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
    window_s = float(spec.get("window_s") or FTP_WINDOW_S)
    job = str(spec.get("job") or "").strip() or secrets.token_urlsafe(12)
    session_id = secrets.token_urlsafe(24)
    session = {
        "id": session_id,
        "cfg": cfg,
        "path_token": path_token,
        "current_path": resolved,
        "window_s": window_s,
        "segments": [],
        "spool_bytes": 0,
        "records_count": 0,
        "t_lo": None,
        "t_hi": None,
        "reached_start": False,
        "cap_hit": False,
        "updated_at": _utc_iso(),
    }
    _progress_set(job, 0, None)
    remaining = _CONFIG.max_log_bytes or None
    try:
        segments, _leftover, _t_lo, _t_hi, cap = _load_tail(
            cfg, window_s, job, remaining)
    except BaseException as exc:
        _progress_finish(job, ok=False, error=str(exc))
        raise
    session["segments"] = segments
    session["cap_hit"] = cap
    _progress_phase(job, "parse")
    try:
        _session_put(session_id, session)
        payload = _ftp_analyze(session)
        _ftp_seal(session, payload)
        payload["job"] = job
        payload["ftp"] = _ftp_meta_for(session)
    except BaseException as exc:
        _remove_segments(segments)
        with _FTP_LOCK:
            _FTP_SESSIONS.pop(session_id, None)
        _progress_finish(job, ok=False, error=str(exc))
        raise
    _progress_finish(job, ok=True)
    return payload


def ftp_refresh(session_id):
    """Append only newly written bytes (or rotate to a new day) then re-analyze."""
    session = _session_peek(session_id)
    if session is None:
        raise KeyError("unknown session")
    resolved = resolve_path(session["path_token"], _utc_date())
    cfg = dict(session["cfg"])
    cfg["path"] = resolved
    window_s = float(session.get("window_s") or FTP_WINDOW_S)
    remaining = _ftp_valve_remaining(session)
    rotated = resolved != session.get("current_path")
    prev_records = int(session.get("records_count") or 0)
    _progress_set(session_id, 0, None)
    new_bytes = 0
    try:
        if rotated:
            segments, _lo, _l, _hi, cap = _load_tail(
                cfg, window_s, session_id, remaining)
            session["segments"].extend(segments)
            session["current_path"] = resolved
            session["cap_hit"] = session.get("cap_hit") or cap
            new_bytes = sum(int(seg["size"]) for seg in segments)
        else:
            size = _remote_size(cfg)
            end = int(session["segments"][-1]["end"]) if session["segments"] else 0
            if size is None or size < end:
                _remove_segments(session["segments"])
                segments, _lo, _l, _hi, cap = _load_tail(
                    cfg, window_s, session_id, remaining)
                session["segments"] = segments
                session["current_path"] = resolved
                session["cap_hit"] = cap
                new_bytes = sum(int(seg["size"]) for seg in segments)
            elif size > end:
                want = size - end
                if remaining is not None:
                    want = min(want, remaining)
                data = _download_range(cfg, end, want)
                if data:
                    tail_seg = session["segments"][-1]
                    _spool_write(tail_seg["entry"], data, append=True)
                    tail_seg["size"] += len(data)
                    tail_seg["end"] = end + len(data)
                new_bytes = len(data)
                if _CONFIG.max_log_bytes and len(data) < (size - end):
                    session["cap_hit"] = True
    except BaseException as exc:
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    _progress_phase(session_id, "parse")
    try:
        payload = _ftp_analyze(session)
        _ftp_seal(session, payload)
        session["updated_at"] = _utc_iso()
        meta = _ftp_meta_for(session)
        meta["new_bytes"] = int(new_bytes)
        meta["new_records"] = int(session["records_count"]) - prev_records
        payload["ftp"] = meta
    except BaseException as exc:
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    _progress_finish(session_id, ok=True)
    return payload


def ftp_load_earlier(session_id, window_s=None):
    """Prepend an older window of segments until remote offset 0 is reached."""
    session = _session_peek(session_id)
    if session is None:
        raise KeyError("unknown session")
    segments = session.get("segments") or []
    oldest = segments[0] if segments else None
    if oldest is None or int(oldest["start"]) <= 0:
        payload = _ftp_analyze(session)
        _ftp_seal(session, payload)
        meta = _ftp_meta_for(session)
        meta["older_bytes"] = 0
        meta["older_records"] = 0
        payload["ftp"] = meta
        return payload

    cfg = dict(session["cfg"])
    cfg["path"] = oldest["remote_path"]
    step = float(window_s if window_s is not None
                 else (session.get("window_s") or FTP_WINDOW_S))
    remaining = _ftp_valve_remaining(session)
    cursor = int(oldest["start"])
    loaded = [0]
    older = []
    o_lo = o_hi = None
    cap_hit = False
    prev_records = int(session.get("records_count") or 0)
    _progress_set(session_id, 0, None)
    try:
        while True:
            prev_cursor = cursor
            aligned_start, seg, _leftover, s_lo, s_hi, cap = \
                _read_backward_segment(cfg, cursor, session_id, loaded,
                                       keep_tail_partial=False,
                                       remaining=remaining,
                                       progress_total=cursor or None)
            if seg is not None:
                older.append(seg)
            if s_lo is not None and (o_lo is None or s_lo < o_lo):
                o_lo = s_lo
            if s_hi is not None and (o_hi is None or s_hi > o_hi):
                o_hi = s_hi
            if cap:
                cap_hit = True
            cursor = aligned_start
            if o_lo is not None and o_hi is not None and (o_hi - o_lo) >= step:
                break
            if cursor <= 0 or cursor >= prev_cursor or seg is None:
                break
        older.reverse()
        session["segments"] = older + segments
        session["cap_hit"] = session.get("cap_hit") or cap_hit
    except BaseException as exc:
        _remove_segments(older)
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    _progress_phase(session_id, "parse")
    try:
        payload = _ftp_analyze(session)
        _ftp_seal(session, payload)
        session["updated_at"] = _utc_iso()
        meta = _ftp_meta_for(session)
        meta["older_bytes"] = sum(int(seg["size"]) for seg in older)
        meta["older_records"] = int(session["records_count"]) - prev_records
        payload["ftp"] = meta
    except BaseException as exc:
        _progress_finish(session_id, ok=False, error=str(exc))
        raise
    _progress_finish(session_id, ok=True)
    return payload


def ftp_close(session_id):
    """Drop an FTP session and its segment spool files; returns ``True`` if it existed."""
    with _FTP_LOCK:
        session = _FTP_SESSIONS.pop(session_id, None)
    if session is None:
        return False
    _remove_segments(session.get("segments"))
    return True


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

    def _unlimited_allowed(self):
        """Whether an unlimited (valve-off) ingest is currently permitted."""
        if _CONFIG.max_log_bytes > 0:
            return True
        if _CONFIG.allow_nonloopback_unlimited:
            return True
        try:
            host = self.server.server_address[0]
        except (AttributeError, IndexError, TypeError):
            host = None
        return _is_loopback(host)

    def _drain_body(self, length):
        """Consume an unprocessed body so the client can read our response."""
        try:
            if length is None:
                while self.rfile.read(SPOOL_CHUNK):
                    pass
            else:
                remaining = length
                while remaining > 0:
                    block = self.rfile.read(min(SPOOL_CHUNK, remaining))
                    if not block:
                        break
                    remaining -= len(block)
        except (OSError, TimeoutError):
            pass

    def _read_body(self):
        """Stream the request body into a disk spool; return its handle.

        Framing (deterministic): absent ``Content-Length`` -> 411; a
        present-but-malformed length -> stream to EOF (408 on timeout); a valid
        positive length -> exactly that many bytes (408 on shortfall). Ingest
        memory is O(SPOOL_CHUNK). Raises ``_HttpError`` on any failure.
        """
        if not _UPLOAD_SEM.acquire(blocking=False):
            raise _HttpError(429, {"ok": False, "code": "busy"}, close=True)
        try:
            return self._read_body_locked()
        finally:
            try:
                _UPLOAD_SEM.release()
            except ValueError:
                pass

    def _read_body_locked(self):
        raw = self.headers.get("Content-Length")
        if raw is None:
            raise _HttpError(411, {"ok": False, "code": "length_required"},
                             close=True)
        length = _parse_content_length(raw)
        malformed = length is None

        if not self._unlimited_allowed():
            try:
                self.connection.settimeout(_CONFIG.read_timeout)
            except OSError:
                pass
            self._drain_body(length)
            raise _HttpError(
                403, {"ok": False, "code": "unlimited_nonloopback_disabled"},
                close=True)

        free = spool_free_bytes(_spool_dir())
        if free is not None and free < SPOOL_CHUNK:
            raise _HttpError(507, {"ok": False, "code": "spool_full"})

        entry = _spool_create()
        try:
            self.connection.settimeout(_CONFIG.read_timeout)
        except OSError:
            pass
        try:
            written = receive_body(
                self.rfile, entry.path, length,
                valve=_CONFIG.max_log_bytes,
                session_limit=_CONFIG.spool_session_bytes,
                budget=_SPOOL_BUDGET)
        except _IngestError as exc:
            _spool_remove(entry.handle)
            raise _HttpError(exc.status, exc.payload(), close=exc.close) from exc
        except (BrokenPipeError, ConnectionResetError) as exc:
            _spool_remove(entry.handle)
            raise _HttpError(400, {"ok": False, "error": "connection closed"},
                             close=True) from exc
        except OSError as exc:
            _spool_remove(entry.handle)
            raise _HttpError(400, {"ok": False, "error": "read failed"},
                             close=True) from exc
        except BaseException:
            _spool_remove(entry.handle)
            raise
        finally:
            try:
                self.connection.settimeout(None)
            except OSError:
                pass
        if malformed:
            self.close_connection = True
        _spool_commit(entry, written)
        return entry

    def _respond_error(self, exc):
        if exc.close:
            self.close_connection = True
        self._send_json(exc.status, exc.payload)

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
        elif path == "/api/analyze/progress":
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
        elif parts.path == "/api/bundle":
            self._handle_bundle(query)
        elif parts.path == "/api/ftp/connect":
            self._handle_ftp(ftp_connect)
        elif parts.path == "/api/ftp/refresh":
            self._handle_ftp_refresh()
        elif parts.path == "/api/ftp/load-earlier":
            self._handle_ftp_load_earlier()
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

    def _read_spool_records(self, entry):
        return hldsmon.parse_records([entry.path])

    def _handle_analyze(self, query):
        handle = self._first(query, "handle", None)
        if handle:
            self._handle_analyze_window(handle, query)
            return
        job = (self._first(query, "job", None) or "").strip() or None
        try:
            entry = self._read_body()
        except _HttpError as exc:
            self._respond_error(exc)
            return
        files = self._query_files(query)
        total = entry.size or None
        if job:
            _progress_set(job, 0, total, phase="parse")

        def _on_progress(offset):
            _progress_set(job, offset, total, phase="parse")

        index = hldsmon.IndexBuilder()
        try:
            sections = _parse_sections(self._first(query, "sections", None))
            from_ts = _parse_epoch(self._first(query, "from", None))
            to_ts = _parse_epoch(self._first(query, "to", None))
            window_s = _parse_window(self._first(query, "window", None))
            payload = coarse_overview(
                entry.path, files, from_ts=from_ts, to_ts=to_ts,
                window_s=window_s, on_progress=_on_progress if job else None,
                index_builder=index, sections=sections)
        except Exception as exc:  # noqa: BLE001 - surface any analyzer error
            if job:
                _progress_finish(job, ok=False, error=str(exc))
            _spool_remove(entry.handle)
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        if job:
            _progress_finish(job, ok=True)
        entry.analysis = _build_analysis_session(payload, index)
        payload["handle"] = entry.handle
        self._send_json(200, payload)

    def _handle_analyze_window(self, handle, query):
        """Handle-form ``/api/analyze``: serve from the session, body forbidden."""
        content_length = self.headers.get("Content-Length")
        if content_length is not None and content_length.strip() not in ("", "0"):
            length = _parse_content_length(content_length)
            if length is not None:
                self._drain_body(length)
            else:
                self.close_connection = True
            self._send_json(400, {"ok": False,
                                  "code": "handle_body_conflict"})
            return
        entry = spool_get(handle)
        if entry is None:
            self._expired()
            return
        try:
            from_ts = _parse_epoch(self._first(query, "from", None))
            to_ts = _parse_epoch(self._first(query, "to", None))
            window_s = _parse_window(self._first(query, "window", None))
            sections = _parse_sections(self._first(query, "sections", None))
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        files = self._query_files(query)
        try:
            if from_ts is None and to_ts is None:
                payload = coarse_overview(entry.path, files, sections=sections)
                payload["handle"] = entry.handle
            else:
                payload = self._memoized_window(
                    entry, from_ts, to_ts, window_s, sections)
        except (FileNotFoundError, OSError):
            self._expired()
            return
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send_json(200, payload)

    def _memoized_window(self, entry, from_ts, to_ts, window_s, sections):
        """Serve a window through the per-handle memo (LRU = ``MEMO_LRU``)."""
        analysis = entry.analysis
        if analysis is None:
            return serve_window(entry, from_ts, to_ts, window_s, sections)
        key = (from_ts, to_ts, window_s,
               None if sections is None else tuple(sorted(sections)))
        with analysis.lock:
            cached = analysis.memo.get(key)
            if cached is not None:
                analysis.memo.move_to_end(key)
                return cached
            payload = serve_window(entry, from_ts, to_ts, window_s, sections)
            analysis.memo[key] = payload
            while len(analysis.memo) > MEMO_LRU:
                analysis.memo.popitem(last=False)
            _enforce_analysis_budget(analysis)
            return payload

    def _expired(self):
        self._send_json(410, {"ok": False, "code": "session_expired"})

    @staticmethod
    def _source_paths(handle):
        """Resolve a handle to spool path(s): local spool handle or FTP session."""
        entry = spool_get(handle)
        if entry is not None:
            return [entry.path]
        session = _session_peek(handle)
        if session is not None:
            paths = [seg["path"] for seg in session.get("segments", [])]
            return paths or None
        return None

    def _send_chunked(self, content_type, chunks):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if self.command == "HEAD":
            return
        try:
            for chunk in chunks:
                if not chunk:
                    continue
                self.wfile.write(b"%x\r\n" % len(chunk))
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def _resolve_type(self, query):
        rtype = self._first(query, "type", None)
        if rtype == "":
            return None, None
        if rtype is not None and rtype not in hldsmon.KNOWN_TYPES:
            return None, "unknown type: %r" % (rtype,)
        return rtype, None

    def _handle_export(self, query):
        handle = self._first(query, "handle", None)
        if handle:
            paths = self._source_paths(handle)
            if paths is None:
                self._expired()
                return
            rtype, err = self._resolve_type(query)
            if err:
                self._send_json(400, {"ok": False, "error": err})
                return
            try:
                lo = _parse_epoch(self._first(query, "from", None))
                hi = _parse_epoch(self._first(query, "to", None))
            except ValueError as exc:
                self._send_json(400, {"ok": False, "error": str(exc)})
                return
            try:
                chunks = export_spool_chunks(paths, rtype, lo, hi)
                self._send_chunked("text/csv; charset=utf-8", chunks)
            except Exception as exc:  # noqa: BLE001
                self._send_json(400, {"ok": False, "error": str(exc)})
            return
        try:
            entry = self._read_body()
        except _HttpError as exc:
            self._respond_error(exc)
            return
        try:
            rtype, err = self._resolve_type(query)
            if err:
                self._send_json(400, {"ok": False, "error": err})
                return
            records = self._read_spool_records(entry)
            csv_text = export_records(records, rtype)
            self._send(200, csv_text, "text/csv; charset=utf-8")
        except Exception as exc:  # noqa: BLE001
            self._send_json(400, {"ok": False, "error": str(exc)})
        finally:
            _spool_remove(entry.handle)

    def _handle_bundle(self, query):
        handle = self._first(query, "handle", None)
        if handle:
            paths = self._source_paths(handle)
            if paths is None:
                self._expired()
                return
            try:
                run = int(self._first(query, "run", ""))
                t = float(self._first(query, "t", ""))
                raw_userid = self._first(query, "userid", "")
                userid = int(raw_userid) if raw_userid not in (None, "") else None
                mins = self._parse_mins(query)
                payload = bundle_from_spool(paths, run, t, userid, mins)
            except (TypeError, ValueError) as exc:
                self._send_json(400, {"ok": False, "error": str(exc)})
                return
            self._send_json(200, payload)
            return
        try:
            entry = self._read_body()
        except _HttpError as exc:
            self._respond_error(exc)
            return
        try:
            raw_drop = self._first(query, "drop", None)
            try:
                drop_index = int(raw_drop)
            except (TypeError, ValueError):
                self._send_json(400, {
                    "ok": False,
                    "error": "drop index required (0-based position in file order)",
                })
                return
            mins = self._parse_mins(query)
            records = self._read_spool_records(entry)
            payload = bundle_records(records, drop_index, mins)
            self._send_json(200, payload)
        except Exception as exc:  # noqa: BLE001 - surface any analyzer error
            self._send_json(400, {"ok": False, "error": str(exc)})
        finally:
            _spool_remove(entry.handle)

    def _parse_mins(self, query):
        raw_mins = self._first(query, "mins", None)
        if raw_mins in (None, ""):
            return 2.0
        try:
            mins = float(raw_mins)
        except (TypeError, ValueError):
            raise ValueError("invalid mins")
        if mins < 0:
            raise ValueError("invalid mins")
        return mins

    def _body_or_error(self):
        """Return raw body bytes, or send an error response and ``None``.

        The body is spooled to disk (streamed, framing-checked) and released
        right away: the small JSON FTP specs do not become server handles.
        """
        try:
            entry = self._read_body()
        except _HttpError as exc:
            self._respond_error(exc)
            return None
        try:
            with open(entry.path, "rb") as fh:
                return fh.read()
        except OSError:
            self._send_json(400, {"ok": False, "error": "cannot read body"})
            return None
        finally:
            _spool_remove(entry.handle)

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
        except _IngestError as exc:
            self._send_json(exc.status, exc.payload())
            return
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
        except _IngestError as exc:
            self._send_json(exc.status, exc.payload())
            return
        except Exception as exc:  # noqa: BLE001 - never crash the server
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        self._send_json(200, payload)

    def _handle_ftp_load_earlier(self):
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
            payload = ftp_load_earlier(session_id, spec.get("window_s"))
        except KeyError:
            self._send_json(400, {"ok": False, "error": "unknown session"})
            return
        except _IngestError as exc:
            self._send_json(exc.status, exc.payload())
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


def _env_int(name, default):
    raw = os.environ.get(name)
    if raw in (None, ""):
        return default
    try:
        return int(raw)
    except (TypeError, ValueError):
        return default


def _env_float(name, default):
    raw = os.environ.get(name)
    if raw in (None, ""):
        return default
    try:
        return float(raw)
    except (TypeError, ValueError):
        return default


def build_parser():
    parser = argparse.ArgumentParser(
        prog="webui",
        description="Local offline web UI for HldsMonitoring JSONL telemetry.")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765,
                        help="TCP port; 0 picks a free port (default: 8765)")
    parser.add_argument("--max-log-mb", type=float,
                        default=_env_float("HLDSMON_WEBUI_MAX_MB", 0.0),
                        help="optional read valve in MiB; 0 = unlimited "
                             "(default). When >0, oversized input is rejected "
                             "with code log_too_large.")
    parser.add_argument("--spool-dir",
                        default=os.environ.get("HLDSMON_SPOOL_DIR") or None,
                        help="disk-backed spool directory (default: "
                             "<tempdir>/hldsmon-spool, tmpfs fallback "
                             "<cwd>/.hldsmon-spool)")
    parser.add_argument("--max-uploads", type=int,
                        default=_env_int("HLDSMON_WEBUI_MAX_UPLOADS",
                                         MAX_UPLOADS),
                        help="max concurrent uploads/sessions (default: %d)"
                             % MAX_UPLOADS)
    parser.add_argument("--read-timeout", type=float,
                        default=_env_float("HLDSMON_WEBUI_READ_TIMEOUT",
                                           READ_TIMEOUT_S),
                        help="socket/body read timeout in seconds "
                             "(default: %g)" % READ_TIMEOUT_S)
    parser.add_argument("--spool-session-mb", type=float,
                        default=_env_float("HLDSMON_WEBUI_SPOOL_SESSION_MB",
                                           SPOOL_SESSION_MB),
                        help="per-session spool budget in MiB (default: %d)"
                             % SPOOL_SESSION_MB)
    parser.add_argument("--spool-total-mb", type=float,
                        default=_env_float("HLDSMON_WEBUI_SPOOL_TOTAL_MB",
                                           SPOOL_TOTAL_MB),
                        help="process-wide spool budget in MiB (default: %d)"
                             % SPOOL_TOTAL_MB)
    parser.add_argument("--spool-ttl-sec", type=float,
                        default=_env_float("HLDSMON_WEBUI_SPOOL_TTL_SEC",
                                           SPOOL_TTL_S),
                        help="spool handle TTL in seconds (default: %g)"
                             % SPOOL_TTL_S)
    parser.add_argument("--allow-nonloopback-unlimited",
                        action="store_true",
                        help="permit unlimited ingest on a non-loopback bind "
                             "(otherwise rejected with 403)")
    parser.add_argument("--open", action="store_true", dest="open_browser",
                        help=argparse.SUPPRESS)
    parser.add_argument("--no-open", action="store_false", dest="open_browser",
                        help="do not open the browser automatically")
    parser.set_defaults(open_browser=True)
    return parser


def configure(args):
    """Apply parsed CLI/env knobs to the process-wide runtime config."""
    mb = float(args.max_log_mb or 0.0)
    _CONFIG.max_log_bytes = int(mb * _MIB) if mb > 0 else 0
    _CONFIG.spool_dir = args.spool_dir
    _CONFIG.allow_nonloopback_unlimited = bool(args.allow_nonloopback_unlimited)
    _CONFIG.max_uploads = max(1, int(args.max_uploads or MAX_UPLOADS))
    _CONFIG.read_timeout = float(args.read_timeout or READ_TIMEOUT_S)
    _CONFIG.spool_session_bytes = max(
        1, int((args.spool_session_mb or SPOOL_SESSION_MB) * _MIB))
    _CONFIG.spool_total_bytes = max(
        1, int((args.spool_total_mb or SPOOL_TOTAL_MB) * _MIB))
    _CONFIG.spool_ttl = float(args.spool_ttl_sec or SPOOL_TTL_S)
    _reset_upload_semaphore()
    _reset_spool_budget()


def warn_unlimited_nonloopback(host):
    """Warn (once) when a non-loopback bind serves unlimited ingest."""
    if _is_loopback(host):
        return False
    if _CONFIG.max_log_bytes > 0 or _CONFIG.allow_nonloopback_unlimited:
        return False
    sys.stderr.write(
        "WARNING: non-loopback bind %r with unlimited ingest disabled: "
        "uploads will be rejected (403 unlimited_nonloopback_disabled) until "
        "--max-log-mb > 0 or --allow-nonloopback-unlimited is set.\n" % (host,))
    return True


def main(argv=None):
    args = build_parser().parse_args(argv)
    configure(args)
    try:
        spool_startup_cleanup()
    except OSError:
        pass
    start_sweeper()
    warn_unlimited_nonloopback(args.host)
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
        stop_sweeper()
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
