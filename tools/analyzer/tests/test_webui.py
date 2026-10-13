#!/usr/bin/env python3
"""Unit + HTTP tests for the HldsMonitoring web UI (``webui.py``).

Run from ``tools/analyzer``::

    python3 -m unittest discover -s tests -v
"""
import ftplib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

# Robust import whether run from tools/analyzer or discovered from the repo root.
_HERE = os.path.dirname(os.path.abspath(__file__))
_ANALYZER_DIR = os.path.dirname(_HERE)
if _ANALYZER_DIR not in sys.path:
    sys.path.insert(0, _ANALYZER_DIR)

import hldsmon  # noqa: E402
import webui  # noqa: E402


def _fixture_lines():
    """A small in-code JSONL log exercising every shape the API touches."""
    lines = [
        # three server samples (third omits frame_ms to force nulls)
        json.dumps({
            "type": "server", "t": 1000.0,
            "frame_ms": {"min": 1.0, "avg": 2.0, "max": 3.0},
            "cpu_pct": 10.0, "rss_kb": 1000, "players": 5, "real": 4,
            "edicts": 100, "fps": 60.0, "in_kbps": 1.5, "out_kbps": 2.5,
        }),
        json.dumps({
            "type": "server", "t": 1001.0,
            "frame_ms": {"min": 1.5, "avg": 2.5, "max": 3.5},
            "cpu_pct": 11.0, "rss_kb": 1010, "players": 6, "real": 5,
            "edicts": 101, "fps": 59.0, "in_kbps": 1.6, "out_kbps": 2.6,
        }),
        json.dumps({
            "type": "server", "t": 1002.0,
            "cpu_pct": 12.0, "rss_kb": 1020, "players": 7, "real": 6,
            "edicts": 102, "fps": 58.0, "in_kbps": 1.7, "out_kbps": 2.7,
        }),
        # two client samples for id 7 + one for id 8
        json.dumps({"type": "client", "t": 1000.5, "id": 7,
                    "ping": 40, "loss": 2, "choke": 0, "rel_fill": 3}),
        json.dumps({"type": "client", "t": 1001.5, "id": 7,
                    "ping": 50, "loss": 4, "choke": 1, "rel_fill": 5}),
        json.dumps({"type": "client", "t": 1002.5, "id": 8,
                    "ping": 30, "loss": 0, "choke": 0, "rel_fill": 1}),
        # one drop event
        json.dumps({"type": "drop", "t": 1003.0, "id": 9, "gen": 1712000000,
                    "reason": "Reliable channel overflowed", "category": "overflow",
                    "crash": False, "observed_via": "sv_dropclient"}),
        "",                 # blank line
        "{not json",        # malformed line
        '{"type":"server","t":1004.0,"frame_ms":{"min":0.5',  # truncated tail
    ]
    return lines


def fixture_bytes():
    return ("\n".join(_fixture_lines())).encode("utf-8")


class TestAnalyzeBytes(unittest.TestCase):
    def setUp(self):
        self.payload = webui.analyze_bytes(
            fixture_bytes(), files=["day1.jsonl", "day2.jsonl"])

    def test_digest_counts_only_valid_records(self):
        # 3 server + 3 client + 1 drop = 7 valid; blank/malformed/truncated skipped.
        self.assertEqual(self.payload["records_count"], 7)
        self.assertEqual(self.payload["digest"]["records"], 7)
        counts = self.payload["digest"]["counts_by_type"]
        self.assertEqual(counts["server"], 3)
        self.assertEqual(counts["client"], 3)
        self.assertEqual(counts["drop"], 1)

    def test_top_level_keys_and_files_echo(self):
        self.assertTrue(self.payload["ok"])
        self.assertEqual(self.payload["version"], 1)
        self.assertEqual(self.payload["files"], ["day1.jsonl", "day2.jsonl"])

    def test_series_lengths_and_values(self):
        series = self.payload["series"]
        for key, values in series.items():
            self.assertEqual(len(values), 3, "series %s length" % key)
        self.assertEqual(series["t"], [1000.0, 1001.0, 1002.0])
        self.assertEqual(series["frame_ms_min"], [1.0, 1.5, None])
        self.assertEqual(series["frame_ms_max"], [3.0, 3.5, None])
        self.assertEqual(series["real_players"], [4, 5, 6])
        self.assertEqual(series["players"], [5, 6, 7])
        self.assertEqual(series["out_kbps"], [2.5, 2.6, 2.7])

    def test_clients_sorted_with_aggregates(self):
        clients = self.payload["clients"]
        self.assertEqual([c["id"] for c in clients], [7, 8])
        by_id = dict((c["id"], c) for c in clients)
        self.assertEqual(by_id[7]["samples"], 2)
        self.assertEqual(by_id[7]["ping_avg"], 45.0)
        self.assertEqual(by_id[7]["ping_max"], 50)
        self.assertEqual(by_id[7]["loss_max"], 4)
        self.assertEqual(by_id[7]["session"], "0:i7:gNone")
        self.assertIsNone(by_id[7]["userid"])
        self.assertEqual(by_id[8]["samples"], 1)
        self.assertEqual(by_id[8]["ping_avg"], 30)

    def test_drops_extracted_with_iso(self):
        drops = self.payload["drops"]
        self.assertEqual(len(drops), 1)
        drop = drops[0]
        self.assertEqual(drop["id"], 9)
        self.assertEqual(drop["session"], "0:i9:g1712000000")
        self.assertEqual(drop["reason"], "Reliable channel overflowed")
        self.assertEqual(drop["category"], "overflow")
        self.assertEqual(drop["observed_via"], "sv_dropclient")
        self.assertFalse(drop["crash"])
        self.assertTrue(drop["iso"])

    def test_anomalies_is_list(self):
        self.assertIsInstance(self.payload["anomalies"], list)

    def test_new_payload_keys_present(self):
        for key in ("client_series", "client_identities", "events",
                    "log_end", "anomalies_excluded", "sessions",
                    "session_count"):
            self.assertIn(key, self.payload, "missing payload key %s" % key)
        self.assertIsInstance(self.payload["client_series"], dict)
        self.assertIsInstance(self.payload["client_identities"], dict)
        self.assertIsInstance(self.payload["sessions"], dict)
        self.assertIsInstance(self.payload["session_count"], int)
        self.assertIsInstance(self.payload["events"], list)
        self.assertIsInstance(self.payload["anomalies_excluded"], list)
        self.assertIn(self.payload["log_end"], ("graceful", "crash", "unknown"))

    def test_client_series_shape(self):
        series = self.payload["client_series"]
        self.assertIn("0:i7:gNone", series)
        self.assertIn("0:i8:gNone", series)
        self.assertEqual(len(series["0:i7:gNone"]["t"]), 2)
        self.assertEqual(series["0:i7:gNone"]["ping"], [40, 50])
        self.assertEqual(series["0:i7:gNone"]["loss"], [2, 4])
        for name in ("t", "ping", "loss", "choke", "rel_fill"):
            self.assertEqual(len(series["0:i7:gNone"][name]), 2, name)

    def test_client_identities_shape(self):
        identities = self.payload["client_identities"]
        self.assertIn("0:i7:gNone", identities)
        self.assertEqual(identities["0:i7:gNone"]["samples"], 2)

    def test_focus_points_payload_key(self):
        self.assertIn("focus_points", self.payload)
        self.assertIsInstance(self.payload["focus_points"], list)

    def test_drops_have_identity_fields(self):
        drop = self.payload["drops"][0]
        for key in ("name", "authid", "steamid64", "ip", "port", "slot",
                    "userid", "conn_s", "fake", "proxy", "session"):
            self.assertIn(key, drop, "missing drop identity field %s" % key)
        self.assertEqual(drop["name"], "")
        self.assertIsNone(drop["slot"])
        self.assertEqual(drop["session"], "0:i9:g1712000000")


def _session_fixture_bytes():
    lines = [
        json.dumps({"type": "client", "t": 1.0, "id": 0, "userid": 5,
                    "steamid64": "76561197960266730", "ping": 10}),
        json.dumps({"type": "drop", "t": 2.0, "id": 0, "userid": 5,
                    "reason": "Timeout", "category": "timeout",
                    "crash": False, "observed_via": "sv_dropclient"}),
        json.dumps({"type": "shutdown", "t": 3.0, "reason": "graceful"}),
        json.dumps({"type": "client", "t": 4.0, "id": 1, "userid": 9,
                    "steamid64": "76561197960266730", "ping": 20}),
    ]
    return ("\n".join(lines) + "\n").encode("utf-8")


class TestSessionAggregation(unittest.TestCase):
    def setUp(self):
        self.payload = webui.analyze_bytes(_session_fixture_bytes(),
                                           files=["run.jsonl"])

    def test_two_sessions_same_steamid_not_merged(self):
        sessions = [c["session"] for c in self.payload["clients"]]
        self.assertEqual(sorted(sessions), ["0:u5", "1:u9"])
        self.assertEqual(self.payload["session_count"], 2)
        self.assertEqual(self.payload["digest"]["sessions"], 2)

    def test_player_group_links_cross_run_sessions(self):
        groups = self.payload["sessions"]
        self.assertEqual(set(groups.keys()), {"76561197960266730"})
        self.assertEqual(groups["76561197960266730"]["sessions"],
                         ["0:u5", "1:u9"])

    def test_drop_session_matches_client_session(self):
        drop = self.payload["drops"][0]
        session_keys = set(c["session"] for c in self.payload["clients"])
        self.assertEqual(drop["session"], "0:u5")
        self.assertIn(drop["session"], session_keys)
        self.assertIn(drop["session"], self.payload["client_series"])


class TestExportBytes(unittest.TestCase):
    def test_export_type_filter(self):
        text = webui.export_bytes(fixture_bytes(), rtype="drop")
        lines = text.strip().splitlines()
        self.assertTrue(lines[0].startswith("type,t,"))
        self.assertIn("reason", lines[0])
        self.assertEqual(len(lines), 2)  # header + one drop

    def test_export_unknown_type_raises(self):
        with self.assertRaises(ValueError):
            webui.export_bytes(fixture_bytes(), rtype="bogus")


def _telemetry_fixture_bytes():
    """A log with server/config/host/client/drop records (stage-10 payload)."""
    entries = [
        {"type": "server", "t": 1000.0, "fps": 60.0},
        {"type": "config", "t": 1000.5, "map": "de_dust2", "maxclients": 32,
         "rehlds_major": 3, "rehlds_minor": 14, "sys_ticrate": 100.0,
         "sv_timeout": 65.0},
        {"type": "host", "t": 1001.0, "udp_rcvbuf_errors": 5,
         "udp_in_errors": None, "net_rx_dropped": 2, "steal_pct": 1.5,
         "io_read_bytes": 10, "io_write_bytes": 20, "disk_bavail_mb": 1000,
         "present_mask": 1023, "caveat_mask": 1},
        {"type": "host", "t": 1002.0, "udp_rcvbuf_errors": None,
         "udp_in_errors": 1, "net_rx_dropped": None, "steal_pct": None,
         "io_read_bytes": None, "io_write_bytes": None, "disk_bavail_mb": 999,
         "present_mask": 512, "caveat_mask": 1},
        {"type": "client", "t": 1002.5, "id": 7, "ping": 45},
        {"type": "config", "t": 1004.0, "map": "de_nuke", "maxclients": 32,
         "rehlds_major": 3, "rehlds_minor": 14, "sys_ticrate": 120.0,
         "sv_timeout": 65.0},
        {"type": "drop", "t": 1003.0, "id": 7, "reason": "Timeout",
         "category": "timeout", "window": [{"t": 1002.5, "ping": 45}]},
    ]
    return ("\n".join(json.dumps(e) for e in entries) + "\n").encode("utf-8")


class TestAnalyzeTelemetryPayload(unittest.TestCase):
    def setUp(self):
        self.payload = webui.analyze_bytes(_telemetry_fixture_bytes())

    def test_host_series_key(self):
        host = self.payload["host_series"]
        for key in ("t", "udp_rcvbuf_errors", "net_rx_dropped", "steal_pct",
                    "io_read_bytes", "io_write_bytes", "disk_bavail_mb",
                    "present_mask", "caveat_mask"):
            self.assertIn(key, host, key)
        self.assertEqual(host["t"], [1001.0, 1002.0])
        self.assertEqual(host["udp_rcvbuf_errors"], [5, None])
        self.assertEqual(host["udp_in_errors"], [None, 1])
        self.assertEqual(host["caveat_mask"], [1, 1])

    def test_host_caveats_map_covers_fields(self):
        caveats = self.payload["host_caveats"]
        self.assertEqual(set(caveats), set(hldsmon.HOST_FIELDS))
        self.assertEqual(caveats["udp_in_errors"],
                         {"source": "udp_netns", "bit": 1})
        self.assertEqual(caveats["net_rx_dropped"],
                         {"source": "net_dev", "bit": 2})
        self.assertEqual(caveats["disk_bavail_mb"]["source"], "disk")

    def test_config_array_with_diffs(self):
        config = self.payload["config"]
        self.assertEqual([c["t"] for c in config], [1000.5, 1004.0])
        self.assertEqual(config[0]["map"], "de_dust2")
        self.assertFalse(config[0]["diff"]["baseline"])
        second = config[1]["diff"]
        self.assertTrue(second["baseline"])
        self.assertEqual(second["changed"], [
            {"field": "sys_ticrate", "before": 100.0, "after": 120.0},
            {"field": "map", "before": "de_dust2", "after": "de_nuke"},
        ])
        self.assertEqual(second["added"], [])
        self.assertEqual(second["removed"], [])


class TestBundleBytes(unittest.TestCase):
    def setUp(self):
        self.data = _telemetry_fixture_bytes()
        self.bundle = webui.bundle_bytes(self.data, 0, 2.0)

    def test_envelope_shape(self):
        self.assertEqual(self.bundle["bundle_version"], 1)
        self.assertEqual(self.bundle["drop"]["reason"], "Timeout")
        self.assertEqual(self.bundle["window"],
                         {"start": 1003.0 - 120.0, "end": 1003.0 + 120.0})
        self.assertEqual(self.bundle["host"]["records"], 2)
        self.assertEqual(len(self.bundle["config"]), 2)
        self.assertEqual(self.bundle["records"], [])
        self.assertTrue(self.bundle["warnings"])

    def test_drop_index_out_of_range(self):
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, 1, 2.0)
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, -1, 2.0)

    def test_bad_drop_index_type(self):
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, "0", 2.0)
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, True, 2.0)

    def test_no_drops(self):
        data = json.dumps({"type": "server", "t": 1.0}).encode("utf-8")
        with self.assertRaises(ValueError):
            webui.bundle_bytes(data, 0, 2.0)

    def test_invalid_mins(self):
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, 0, -1)
        with self.assertRaises(ValueError):
            webui.bundle_bytes(self.data, 0, "soon")


class TestHTTP(unittest.TestCase):
    def setUp(self):
        self.spool_dir = tempfile.mkdtemp(prefix="llspool-")
        webui._CONFIG.spool_dir = self.spool_dir
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        self.server = webui.make_server("127.0.0.1", 0)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        webui._reset_spool_budget()
        webui._CONFIG.spool_dir = None
        shutil.rmtree(self.spool_dir, ignore_errors=True)

    def _request(self, method, path, data=None, headers=None):
        req = urllib.request.Request(
            self.base + path, data=data, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, resp.headers, resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers, exc.read()

    def test_get_index_html(self):
        status, headers, body = self._request("GET", "/")
        self.assertEqual(status, 200)
        self.assertTrue(headers.get("Content-Type").startswith("text/html"))
        self.assertIn(b"<!DOCTYPE html>", body)

    def test_get_health(self):
        status, headers, body = self._request("GET", "/api/health")
        self.assertEqual(status, 200)
        self.assertTrue(headers.get("Content-Type").startswith("application/json"))
        data = json.loads(body.decode("utf-8"))
        self.assertTrue(data["ok"])
        self.assertEqual(data["service"], "hldsmon-webui")
        self.assertEqual(data["version"], 1)

    def test_post_analyze(self):
        status, headers, body = self._request(
            "POST", "/api/analyze?files=day1.jsonl",
            data=fixture_bytes(),
            headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200)
        data = json.loads(body.decode("utf-8"))
        self.assertTrue(data["ok"])
        for key in ("digest", "series", "clients", "drops", "anomalies"):
            self.assertIn(key, data)
        for key in ("client_series", "client_identities", "events",
                    "log_end", "anomalies_excluded"):
            self.assertIn(key, data, "missing payload key %s" % key)
        self.assertEqual(data["records_count"], 7)
        self.assertEqual(data["files"], ["day1.jsonl"])

    def test_post_export_csv(self):
        status, headers, body = self._request(
            "POST", "/api/export?type=drop", data=fixture_bytes())
        self.assertEqual(status, 200)
        self.assertTrue(headers.get("Content-Type").startswith("text/csv"))
        self.assertIn("type", body.decode("utf-8").splitlines()[0])

    def test_post_export_unknown_type(self):
        status, headers, body = self._request(
            "POST", "/api/export?type=bogus", data=fixture_bytes())
        self.assertEqual(status, 400)
        data = json.loads(body.decode("utf-8"))
        self.assertFalse(data["ok"])

    def test_post_bundle(self):
        status, headers, body = self._request(
            "POST", "/api/bundle?drop=0&mins=2",
            data=_telemetry_fixture_bytes(),
            headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200)
        self.assertTrue(
            headers.get("Content-Type").startswith("application/json"))
        data = json.loads(body.decode("utf-8"))
        self.assertEqual(data["bundle_version"], 1)
        self.assertEqual(data["drop"]["reason"], "Timeout")
        self.assertEqual(data["window"],
                         {"start": 1003.0 - 120.0, "end": 1003.0 + 120.0})
        self.assertEqual(data["host"]["records"], 2)
        self.assertIn("config", data)
        self.assertIn("records", data)

    def test_post_bundle_requires_valid_drop_index(self):
        for path in ("/api/bundle",
                     "/api/bundle?drop=abc",
                     "/api/bundle?drop=99"):
            status, _, body = self._request(
                "POST", path, data=_telemetry_fixture_bytes())
            self.assertEqual(status, 400, path)
            data = json.loads(body.decode("utf-8"))
            self.assertFalse(data["ok"])

    def test_post_bundle_invalid_mins(self):
        for mins in ("abc", "-1"):
            status, _, body = self._request(
                "POST", "/api/bundle?drop=0&mins=" + mins,
                data=_telemetry_fixture_bytes())
            self.assertEqual(status, 400, mins)

    def test_unknown_path_404(self):
        status, headers, body = self._request("GET", "/nope")
        self.assertEqual(status, 404)


def _server_record(t, **overrides):
    rec = {
        "type": "server", "t": t,
        "frame_ms": {"min": 1.0, "avg": 2.0, "max": 3.0},
        "cpu_pct": 10.0, "rss_kb": 1000, "players": 5, "real": 4,
        "edicts": 100, "fps": 60.0, "in_kbps": 1.5, "out_kbps": 2.5,
    }
    rec.update(overrides)
    return rec


def _drop_record(t, cid):
    return {"type": "drop", "t": t, "id": cid, "gen": 1, "reason": "Timeout",
            "category": "timeout", "crash": False, "observed_via": "sv_dropclient"}


def ftp_jsonl(entries):
    return ("\n".join(json.dumps(e) for e in entries) + "\n").encode("utf-8")


class _FakeRemote:
    def __init__(self, data=b""):
        self.data = data
        self.no_size = False
        self.calls = []

    def size(self, cfg):
        return None if self.no_size else len(self.data)

    def range(self, cfg, start, length):
        start = int(start)
        length = int(length)
        self.calls.append((start, length))
        return bytes(self.data[start:start + length])

    def download(self, cfg, dest_path, offset=0, on_bytes=None, max_bytes=None):
        offset = int(offset or 0)
        self.calls.append(("download", offset))
        payload = bytes(self.data[offset:])
        if max_bytes is not None:
            payload = payload[:max_bytes]
        with open(dest_path, "wb") as fh:
            fh.write(payload)
        return len(payload), False


class _MultiPathRemote:
    """A fake FTP host whose content depends on ``cfg['path']`` (for days)."""

    def __init__(self, files):
        self.files = dict(files)
        self.calls = []

    def size(self, cfg):
        return len(self.files.get(cfg["path"], b""))

    def range(self, cfg, start, length):
        data = self.files.get(cfg["path"], b"")
        return bytes(data[int(start):int(start) + int(length)])

    def download(self, cfg, dest_path, offset=0, on_bytes=None, max_bytes=None):
        data = self.files.get(cfg["path"], b"")[int(offset or 0):]
        if max_bytes is not None:
            data = data[:max_bytes]
        with open(dest_path, "wb") as fh:
            fh.write(data)
        return len(data), False


class _RestlessFtp:
    """A minimal FTP client lacking SIZE and REST (drives the spool fallback)."""

    def __init__(self, data):
        self.data = data

    def size(self, path):
        raise OSError("SIZE not understood")

    def transfercmd(self, cmd, rest=None):
        raise ftplib.error_perm("REST not understood")

    def retrbinary(self, cmd, callback, rest=None):
        for i in range(0, len(self.data), 7):
            callback(self.data[i:i + 7])

    def voidresp(self):
        pass

    def quit(self):
        pass

    def close(self):
        pass


class TestSplitTail(unittest.TestCase):
    def test_complete_lines_no_leftover(self):
        lines, leftover = webui.split_tail(b"", b"one\ntwo\n")
        self.assertEqual(lines, [b"one", b"two"])
        self.assertEqual(leftover, b"")

    def test_trailing_partial_line_becomes_leftover(self):
        lines, leftover = webui.split_tail(b"", b'{"a":1}\n{"b":2}')
        self.assertEqual(lines, [b'{"a":1}'])
        self.assertEqual(leftover, b'{"b":2}')

    def test_chunk_split_across_calls_reassembles_line(self):
        lines1, left1 = webui.split_tail(b"", b'{"a":1}\n{"b"')
        self.assertEqual(lines1, [b'{"a":1}'])
        self.assertEqual(left1, b'{"b"')
        lines2, left2 = webui.split_tail(left1, b':2}\n{"c":3}\n')
        self.assertEqual(lines2, [b'{"b":2}', b'{"c":3}'])
        self.assertEqual(left2, b"")

    def test_empty_input(self):
        self.assertEqual(webui.split_tail(b"", b""), ([], b""))

    def test_str_input_is_accepted(self):
        lines, leftover = webui.split_tail("", "x\ny")
        self.assertEqual(lines, [b"x"])
        self.assertEqual(leftover, b"y")


class TestResolvePath(unittest.TestCase):
    def test_short_token_substituted(self):
        self.assertEqual(
            webui.resolve_path("/logs/hldsmon-{date}.jsonl", "2026-10-10"),
            "/logs/hldsmon-2026-10-10.jsonl")

    def test_iso_token_substituted(self):
        self.assertEqual(
            webui.resolve_path("/a/{YYYY-MM-DD}/b", "2026-10-10"),
            "/a/2026-10-10/b")

    def test_both_tokens_substituted(self):
        self.assertEqual(
            webui.resolve_path("/{YYYY-MM-DD}/{date}.jsonl", "2026-10-10"),
            "/2026-10-10/2026-10-10.jsonl")

    def test_no_token_unchanged(self):
        self.assertEqual(webui.resolve_path("/x/y.jsonl", "2026-10-10"),
                         "/x/y.jsonl")

    def test_utc_date_helper_format(self):
        self.assertRegex(webui._utc_date(), r"^\d{4}-\d{2}-\d{2}$")


class TestFtpEndpoints(unittest.TestCase):
    def setUp(self):
        self._orig_seams = (webui._remote_size, webui._download_range,
                            webui._download, webui.FTP_BACK_CHUNK)
        self._orig_valve = webui._CONFIG.max_log_bytes
        webui._CONFIG.max_log_bytes = 0
        self.remote = _FakeRemote()
        webui._remote_size = self.remote.size
        webui._download_range = self.remote.range
        webui._download = self.remote.download
        webui.FTP_BACK_CHUNK = 512
        self.spool_dir = tempfile.mkdtemp(prefix="llspool-")
        webui._CONFIG.spool_dir = self.spool_dir
        with webui._FTP_LOCK:
            webui._FTP_SESSIONS.clear()
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        self.server = webui.make_server("127.0.0.1", 0)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.port

    def tearDown(self):
        (webui._remote_size, webui._download_range,
         webui._download, webui.FTP_BACK_CHUNK) = self._orig_seams
        with webui._FTP_LOCK:
            webui._FTP_SESSIONS.clear()
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        webui._reset_spool_budget()
        webui._CONFIG.max_log_bytes = self._orig_valve
        webui._CONFIG.spool_dir = None
        shutil.rmtree(self.spool_dir, ignore_errors=True)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def _post_json(self, path, obj):
        data = json.dumps(obj).encode("utf-8")
        req = urllib.request.Request(
            self.base + path, data=data, method="POST",
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read().decode("utf-8"))

    def _connect(self, path="/logs/h-{date}.jsonl", **extra):
        spec = {"host": "ftp.local", "port": 21, "user": "u",
                "password": "s3cr3t", "path": path}
        spec.update(extra)
        return self._post_json("/api/ftp/connect", spec)

    def test_connect_returns_full_payload_and_session(self):
        base = ftp_jsonl([_server_record(1000.0), _server_record(1001.0),
                          _server_record(1002.0), _drop_record(1003.0, 9)])
        self.remote.data = base
        status, data = self._connect()
        self.assertEqual(status, 200)
        self.assertTrue(data["ok"])
        self.assertIn("session", data)
        self.assertTrue(data["session"])
        for key in ("digest", "series", "clients", "drops", "anomalies",
                    "client_series", "client_identities", "events", "log_end"):
            self.assertIn(key, data, "missing payload key %s" % key)
        self.assertEqual(data["ftp"]["bytes"], len(base))
        self.assertEqual(data["ftp"]["records"], 4)
        self.assertEqual(data["records_count"], 4)
        self.assertNotIn("{date}", data["ftp"]["path"])
        self.assertTrue(data["ftp"]["path"].startswith("/logs/h-"))

    def test_connect_never_echoes_credentials(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0)])
        status, data = self._connect(password="topsecret", user="admin")
        self.assertEqual(status, 200)
        blob = json.dumps(data)
        self.assertNotIn("topsecret", blob)
        self.assertNotIn("password", blob)
        # server-side record retains the credential (RAM only)
        with webui._FTP_LOCK:
            stored = webui._FTP_SESSIONS[data["session"]]
        self.assertEqual(stored["cfg"]["password"], "topsecret")

    def test_progress_endpoint_reports_and_finishes(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0)])
        job = "job-progress-1"
        status, data = self._post_json(
            "/api/ftp/connect", {"host": "h", "path": "/x.jsonl", "job": job})
        self.assertEqual(status, 200)
        self.assertEqual(data.get("job"), job)
        entry = webui.progress_get(job)
        self.assertIsNotNone(entry)
        self.assertTrue(entry["finished"])
        self.assertEqual(entry["pct"], 100.0)
        with urllib.request.urlopen(
                self.base + "/api/ftp/progress?id=" + job, timeout=10) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        self.assertTrue(body["ok"])
        self.assertTrue(body["progress"]["finished"])

    def test_progress_endpoint_unknown_id(self):
        with urllib.request.urlopen(
                self.base + "/api/ftp/progress?id=nope", timeout=10) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        self.assertTrue(body["ok"])
        self.assertIsNone(body["progress"])

    def test_refresh_appends_only_new_records(self):
        base = ftp_jsonl([_server_record(1000.0), _server_record(1001.0),
                          _server_record(1002.0), _drop_record(1003.0, 9)])
        self.remote.data = base
        _, connect = self._connect()
        sid = connect["session"]
        appended = ftp_jsonl([_server_record(1004.0), _server_record(1005.0),
                              _drop_record(1006.0, 10)])
        self.remote.data = base + appended
        status, data = self._post_json("/api/ftp/refresh", {"session": sid})
        self.assertEqual(status, 200)
        self.assertTrue(data["ok"])
        self.assertGreater(data["ftp"]["new_records"], 0)
        self.assertEqual(data["ftp"]["new_bytes"], len(appended))
        self.assertEqual(data["ftp"]["records"], 7)
        self.assertEqual(data["records_count"], 7)
        counts = data["digest"]["counts_by_type"]
        self.assertEqual(counts["server"], 5)
        self.assertEqual(counts["drop"], 2)

    def test_refresh_reassembles_line_split_across_reads(self):
        base = ftp_jsonl([_server_record(1000.0)])
        self.remote.data = base
        _, connect = self._connect()
        sid = connect["session"]
        self.remote.data = base + b'{"type":"server","t":2000.0'
        _, data1 = self._post_json("/api/ftp/refresh", {"session": sid})
        self.assertEqual(data1["records_count"], 1)
        self.assertEqual(data1["ftp"]["new_records"], 0)
        self.remote.data = base + b'{"type":"server","t":2000.0}\n'
        _, data2 = self._post_json("/api/ftp/refresh", {"session": sid})
        self.assertEqual(data2["records_count"], 2)
        self.assertEqual(data2["ftp"]["new_records"], 1)

    def test_refresh_rotation_resets_on_shrink(self):
        base = ftp_jsonl([_server_record(1000.0), _server_record(1001.0),
                          _server_record(1002.0)])
        self.remote.data = base
        _, connect = self._connect()
        sid = connect["session"]
        self.remote.data = ftp_jsonl([_server_record(3000.0)])
        status, data = self._post_json("/api/ftp/refresh", {"session": sid})
        self.assertEqual(status, 200)
        self.assertEqual(data["records_count"], 1)
        self.assertEqual(data["ftp"]["records"], 1)

    def test_connect_loads_only_recent_window(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i) for i in range(40)])
        status, data = self._connect(window_s=5.0)
        self.assertEqual(status, 200)
        self.assertFalse(data["ftp"]["reached_start"])
        self.assertLess(data["ftp"]["records"], 40)
        self.assertGreaterEqual(data["ftp"]["end_t"] - data["ftp"]["start_t"], 5.0)

    def test_load_earlier_prepends_older_without_garbling(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i) for i in range(40)])
        _, conn = self._connect(window_s=5.0)
        sid = conn["session"]
        status, data = self._post_json("/api/ftp/load-earlier", {"session": sid})
        self.assertEqual(status, 200)
        self.assertGreater(data["ftp"]["older_records"], 0)
        self.assertLess(data["ftp"]["start_t"], conn["ftp"]["start_t"])
        self.assertEqual(data["records_count"],
                         conn["records_count"] + data["ftp"]["older_records"])

    def test_load_earlier_until_reached_start_then_noop(self):
        n = 40
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i) for i in range(n)])
        _, data = self._connect(window_s=5.0)
        sid = data["session"]
        for _ in range(60):
            if data["ftp"]["reached_start"]:
                break
            _, data = self._post_json("/api/ftp/load-earlier", {"session": sid})
        self.assertTrue(data["ftp"]["reached_start"])
        self.assertEqual(data["records_count"], n)
        _, again = self._post_json("/api/ftp/load-earlier", {"session": sid})
        self.assertTrue(again["ftp"]["reached_start"])
        self.assertEqual(again["ftp"]["older_records"], 0)
        self.assertEqual(again["records_count"], n)

    def test_close_then_unknown_session_400(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0)])
        _, connect = self._connect()
        sid = connect["session"]
        status, data = self._post_json("/api/ftp/close", {"session": sid})
        self.assertEqual(status, 200)
        self.assertTrue(data["ok"])
        status, data = self._post_json("/api/ftp/refresh", {"session": sid})
        self.assertEqual(status, 400)
        self.assertEqual(data["error"], "unknown session")
        status, data = self._post_json("/api/ftp/close", {"session": "nope"})
        self.assertEqual(status, 400)
        self.assertFalse(data["ok"])

    def test_unknown_session_refresh_400(self):
        status, data = self._post_json("/api/ftp/refresh",
                                       {"session": "does-not-exist"})
        self.assertEqual(status, 400)
        self.assertEqual(data["error"], "unknown session")

    def test_connect_validation_errors_400(self):
        status, data = self._post_json("/api/ftp/connect", {"path": "/x"})
        self.assertEqual(status, 400)
        self.assertFalse(data["ok"])
        self.assertIn("host", data["error"])
        status, data = self._post_json("/api/ftp/connect", {"host": "h"})
        self.assertEqual(status, 400)
        self.assertIn("path", data["error"])

    def test_invalid_json_body_400(self):
        req = urllib.request.Request(
            self.base + "/api/ftp/connect", data=b"{not json", method="POST",
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                status = resp.status
                body = resp.read()
        except urllib.error.HTTPError as exc:
            status = exc.code
            body = exc.read()
        self.assertEqual(status, 400)
        self.assertFalse(json.loads(body.decode("utf-8"))["ok"])

    def _post_bytes(self, path, data=b""):
        req = urllib.request.Request(
            self.base + path, data=data, method="POST",
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()

    def _session_segments(self, session_id):
        with webui._FTP_LOCK:
            return list(webui._FTP_SESSIONS[session_id]["segments"])

    def _all_times(self, session_id):
        paths = [seg["path"] for seg in self._session_segments(session_id)]
        return [hldsmon._num(rec.get("t"))
                for rec in webui._iter_spool_records(paths)]

    def test_connect_stores_segment_spool_not_records(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i)
                                      for i in range(4)])
        status, data = self._connect()
        self.assertEqual(status, 200)
        with webui._FTP_LOCK:
            stored = webui._FTP_SESSIONS[data["session"]]
        self.assertIn("segments", stored)
        self.assertNotIn("records", stored)
        self.assertTrue(stored["segments"])
        for seg in stored["segments"]:
            self.assertTrue(os.path.exists(seg["path"]))
        self.assertEqual(stored["spool_bytes"], len(self.remote.data))

    def test_connect_without_size_streams_full_retr_to_spool(self):
        n = 2000
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i)
                                      for i in range(n)])
        self.remote.no_size = True
        status, data = self._connect()
        self.assertEqual(status, 200)
        self.assertTrue(data["ftp"]["reached_start"])
        self.assertEqual(data["records_count"], n)
        self.assertEqual(data["ftp"]["bytes"], len(self.remote.data))
        self.assertEqual(data["ftp"]["segments"], 1)

    def test_valve_hit_sets_cap_hit_and_limit_bytes(self):
        body = ftp_jsonl([_server_record(1000.0 + i) for i in range(40)])
        self.remote.data = body
        valve = len(body) // 2
        webui._CONFIG.max_log_bytes = valve
        status, data = self._connect(window_s=100000.0)
        self.assertEqual(status, 200)
        self.assertTrue(data["ftp"]["cap_hit"])
        self.assertEqual(data["ftp"]["limit_bytes"], valve)
        self.assertTrue(data["ftp"]["truncated"])
        self.assertLessEqual(data["ftp"]["bytes"], valve)

    def test_valve_off_omits_cap_hit(self):
        self.remote.data = ftp_jsonl([_server_record(1000.0)])
        status, data = self._connect()
        self.assertEqual(status, 200)
        self.assertNotIn("cap_hit", data["ftp"])
        self.assertNotIn("limit_bytes", data["ftp"])

    def test_valve_above_file_size_is_not_cap_hit(self):
        body = ftp_jsonl([_server_record(1000.0 + i) for i in range(2)])
        self.assertLess(len(body), webui.FTP_BACK_CHUNK)
        self.remote.data = body
        webui._CONFIG.max_log_bytes = len(body)
        status, data = self._connect(window_s=100000.0)
        self.assertEqual(status, 200)
        self.assertFalse(data["ftp"]["cap_hit"])
        self.assertEqual(data["ftp"]["bytes"], len(body))
        self.assertTrue(data["ftp"]["reached_start"])

    def test_load_earlier_to_start_is_continuous(self):
        n = 40
        self.remote.data = ftp_jsonl([_server_record(1000.0 + i)
                                      for i in range(n)])
        _, data = self._connect(window_s=5.0)
        sid = data["session"]
        for _ in range(60):
            if data["ftp"]["reached_start"]:
                break
            _, data = self._post_json("/api/ftp/load-earlier", {"session": sid})
        self.assertTrue(data["ftp"]["reached_start"])
        self.assertEqual(data["records_count"], n)
        times = [t for t in self._all_times(sid) if t is not None]
        self.assertEqual(times, [1000.0 + i for i in range(n)])

    def test_export_and_bundle_available_for_ftp_handle(self):
        drop = dict(_drop_record(1001.0, 9), userid=9)
        self.remote.data = ftp_jsonl([_server_record(1000.0), drop,
                                      _server_record(1002.0)])
        _, conn = self._connect()
        handle = conn["handle"]
        self.assertEqual(handle, conn["session"])
        status, raw = self._post_bytes("/api/export?handle=%s&type=drop" % handle)
        self.assertEqual(status, 200)
        self.assertEqual(
            len(raw.decode("utf-8").strip().splitlines()) - 1, 1)
        key = conn["drops"][0]["key"]
        status, raw = self._post_bytes(
            "/api/bundle?handle=%s&run=%d&t=%s&userid=%s"
            % (handle, key["run"], key["t"], key["userid"]))
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["drop"]["reason"], "Timeout")

    def test_day_rotation_keeps_segments_without_loss_or_duplicates(self):
        day1 = ftp_jsonl([_server_record(1000.0 + i) for i in range(6)])
        day2 = ftp_jsonl([_server_record(1006.0 + i) for i in range(5)])
        remote = _MultiPathRemote({
            "/logs/hldsmon-2026-10-10.jsonl": day1,
            "/logs/hldsmon-2026-10-11.jsonl": day2,
        })
        saved = (webui._remote_size, webui._download_range, webui._download)
        saved_date = webui._utc_date
        holder = {"date": "2026-10-10"}
        webui._remote_size = remote.size
        webui._download_range = remote.range
        webui._download = remote.download
        webui._utc_date = lambda: holder["date"]
        try:
            status, conn = self._connect(path="/logs/hldsmon-{date}.jsonl")
            self.assertEqual(status, 200)
            self.assertEqual(conn["records_count"], 6)
            self.assertEqual(conn["ftp"]["path"],
                             "/logs/hldsmon-2026-10-10.jsonl")
            sid = conn["session"]
            holder["date"] = "2026-10-11"
            status, data = self._post_json("/api/ftp/refresh",
                                           {"session": sid})
            self.assertEqual(status, 200)
            self.assertEqual(data["records_count"], 11)
            self.assertEqual(data["ftp"]["path"],
                             "/logs/hldsmon-2026-10-11.jsonl")
        finally:
            webui._utc_date = saved_date
            (webui._remote_size, webui._download_range, webui._download) = saved
        self.assertEqual(
            set(seg["remote_path"] for seg in self._session_segments(sid)),
            {"/logs/hldsmon-2026-10-10.jsonl",
             "/logs/hldsmon-2026-10-11.jsonl"})
        times = [t for t in self._all_times(sid) if t is not None]
        self.assertEqual(times, [1000.0 + i for i in range(11)])


class TestFtpRestlessFallback(unittest.TestCase):
    def test_download_range_streams_full_file_to_spool_then_slices(self):
        data = b"".join(b"0123456789abcdef" for _ in range(100))
        saved = webui._ftp_open
        webui._ftp_open = lambda cfg: _RestlessFtp(data)
        try:
            got = webui._download_range({"path": "/x.jsonl"}, 17, 40)
        finally:
            webui._ftp_open = saved
        self.assertEqual(got, data[17:57])


# ---------------------------------------------------------------------------
# Stage 12 (large-logs) LL-P1: streaming spool ingest, deterministic framing,
# optional read valve, loopback guard, spool budgets/cleanup, BOM, upload
# concurrency/read-timeout and ingest memory (QAS-L1/L2/L7/L8/L9/L11/L12/L20).
# ---------------------------------------------------------------------------

#: Max allowed growth of the server's peak RSS when the ingested body doubles
#: (plan LL-P1 / QAS-L1 / QAS-L20).
RSS_SLOPE_MAX_MIB = 64


def _raw_http(port, raw, read_timeout=5.0, half_close=True):
    """Send ``raw`` bytes to ``port``; return the full response bytes."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=read_timeout)
    try:
        sock.sendall(raw)
        if half_close:
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        out = b""
        while True:
            try:
                block = sock.recv(65536)
            except socket.timeout:
                break
            if not block:
                break
            out += block
        return out
    finally:
        try:
            sock.close()
        except OSError:
            pass


def _http_status(raw):
    head = raw.split(b"\r\n", 1)[0].decode("latin1")
    parts = head.split()
    return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None


def _http_json_body(raw):
    idx = raw.find(b"\r\n\r\n")
    if idx < 0:
        return None
    try:
        return json.loads(raw[idx + 4:].decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return None


class _LLSpoolEnv(unittest.TestCase):
    """Per-test runtime + disk-spool isolation for the large-logs stage."""

    host = "127.0.0.1"

    def setUp(self):
        c = webui._CONFIG
        self._saved = (c.max_log_bytes, c.spool_dir,
                       c.allow_nonloopback_unlimited, c.max_uploads,
                       c.read_timeout, c.spool_session_bytes,
                       c.spool_total_bytes, c.spool_ttl)
        self._saved_sem = webui._UPLOAD_SEM
        self._saved_budget = webui._SPOOL_BUDGET
        self.spool_dir = tempfile.mkdtemp(prefix="llspool-")
        c.spool_dir = self.spool_dir
        c.max_log_bytes = 0
        c.allow_nonloopback_unlimited = False
        c.max_uploads = webui.MAX_UPLOADS
        c.read_timeout = webui.READ_TIMEOUT_S
        c.spool_session_bytes = webui.SPOOL_SESSION_MB * webui._MIB
        c.spool_total_bytes = webui.SPOOL_TOTAL_MB * webui._MIB
        c.spool_ttl = webui.SPOOL_TTL_S
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        webui._reset_upload_semaphore()
        webui._reset_spool_budget()
        self.server = webui.make_server(self.host, 0)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.port

    def tearDown(self):
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        c = webui._CONFIG
        (c.max_log_bytes, c.spool_dir, c.allow_nonloopback_unlimited,
         c.max_uploads, c.read_timeout, c.spool_session_bytes,
         c.spool_total_bytes, c.spool_ttl) = self._saved
        webui._UPLOAD_SEM = self._saved_sem
        webui._SPOOL_BUDGET = self._saved_budget
        shutil.rmtree(self.spool_dir, ignore_errors=True)

    def _spool_files(self):
        try:
            return [n for n in os.listdir(self.spool_dir)
                    if n.startswith(webui._SPOOL_PREFIX)]
        except OSError:
            return []

    def _post(self, path, data, headers=None):
        req = urllib.request.Request(
            self.base + path, data=data, method="POST",
            headers=headers or {})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, resp.headers, resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers, exc.read()


class TestContentLengthFraming(_LLSpoolEnv):
    def test_absent_content_length_is_411(self):
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Type: application/octet-stream\r\n\r\n")
        self.assertEqual(_http_status(resp), 411)
        self.assertEqual(_http_json_body(resp)["code"], "length_required")
        self.assertEqual(self._spool_files(), [])

    def test_malformed_content_length_streams_to_eof(self):
        body = fixture_bytes()
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: abc\r\n\r\n" + body)
        self.assertEqual(_http_status(resp), 200)
        payload = _http_json_body(resp)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["records_count"], 7)
        self.assertIn("handle", payload)

    def test_valid_content_length_reads_exactly_n(self):
        body = fixture_bytes()
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.assertEqual(_http_status(resp), 200)
        self.assertEqual(_http_json_body(resp)["records_count"], 7)

    def test_short_body_is_408(self):
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 100\r\n\r\n" + b"{}\n")
        self.assertEqual(_http_status(resp), 408)
        self.assertEqual(_http_json_body(resp)["code"], "request_timeout")
        self.assertEqual(self._spool_files(), [])

    def test_parse_content_length_contract(self):
        self.assertEqual(webui._parse_content_length("10"), 10)
        self.assertIsNone(webui._parse_content_length("0"))
        self.assertIsNone(webui._parse_content_length("-3"))
        self.assertIsNone(webui._parse_content_length("abc"))
        self.assertIsNone(webui._parse_content_length(None))


class TestReadValve(_LLSpoolEnv):
    def test_unlimited_by_default_accepts_body(self):
        status, _, raw = self._post("/api/analyze", fixture_bytes() * 500)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw)["ok"])

    def test_over_valve_is_413_log_too_large(self):
        webui._CONFIG.max_log_bytes = 100
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 413)
        payload = json.loads(raw)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["code"], "log_too_large")
        self.assertEqual(payload["limit_bytes"], 100)
        self.assertEqual(self._spool_files(), [])

    def test_valve_applies_to_export_and_bundle(self):
        webui._CONFIG.max_log_bytes = 10
        for path in ("/api/export", "/api/bundle?drop=0"):
            status, _, raw = self._post(path, fixture_bytes())
            self.assertEqual(status, 413, path)
            self.assertEqual(json.loads(raw)["code"], "log_too_large", path)

    def test_under_valve_accepts(self):
        webui._CONFIG.max_log_bytes = 10 * 1024 * 1024
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw)["ok"])


class TestLoopbackGuard(_LLSpoolEnv):
    host = "0.0.0.0"

    def test_nonloopback_unlimited_without_optin_is_403(self):
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(raw)["code"],
                         "unlimited_nonloopback_disabled")

    def test_nonloopback_with_optin_ok(self):
        webui._CONFIG.allow_nonloopback_unlimited = True
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw)["ok"])

    def test_nonloopback_with_valve_ok(self):
        webui._CONFIG.max_log_bytes = 10 * 1024 * 1024
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 200)


class TestLoopbackHelpers(unittest.TestCase):
    def test_is_loopback(self):
        for host in ("127.0.0.1", "127.0.0.53", "::1", "localhost", None):
            self.assertTrue(webui._is_loopback(host), host)
        for host in ("0.0.0.0", "192.168.1.5", "example.com", "::"):
            self.assertFalse(webui._is_loopback(host), host)

    def test_warn_unlimited_nonloopback(self):
        saved = (webui._CONFIG.max_log_bytes,
                 webui._CONFIG.allow_nonloopback_unlimited)
        try:
            webui._CONFIG.max_log_bytes = 0
            webui._CONFIG.allow_nonloopback_unlimited = False
            self.assertFalse(webui.warn_unlimited_nonloopback("127.0.0.1"))
            self.assertTrue(webui.warn_unlimited_nonloopback("0.0.0.0"))
            webui._CONFIG.allow_nonloopback_unlimited = True
            self.assertFalse(webui.warn_unlimited_nonloopback("0.0.0.0"))
            webui._CONFIG.max_log_bytes = 1
            self.assertFalse(webui.warn_unlimited_nonloopback("0.0.0.0"))
        finally:
            (webui._CONFIG.max_log_bytes,
             webui._CONFIG.allow_nonloopback_unlimited) = saved


class TestSpoolBacking(unittest.TestCase):
    _MOUNTS = ("tmpfs /tmp tmpfs rw,nosuid 0 0\n"
               "/dev/sda1 / ext4 rw 0 0\n"
               "/dev/sdb1 /mnt/data xfs rw 0 0\n")

    def test_tmpfs(self):
        self.assertEqual(webui.spool_backing_kind(self._MOUNTS, "/tmp/x"),
                         "tmpfs")

    def test_disk(self):
        self.assertEqual(webui.spool_backing_kind(self._MOUNTS, "/mnt/data/y"),
                         "disk")

    def test_root_match(self):
        self.assertEqual(webui.spool_backing_kind(self._MOUNTS, "/home/z"),
                         "disk")

    def test_unknown(self):
        self.assertEqual(webui.spool_backing_kind("", "/tmp/x"), "unknown")

    def test_longest_mount_wins(self):
        mounts = "/dev/sda1 / ext4 rw 0 0\ntmpfs /data/tmp tmpfs rw 0 0\n"
        self.assertEqual(webui.spool_backing_kind(mounts, "/data/tmp/logs"),
                         "tmpfs")

    def test_configured_dir_honoured(self):
        self.assertEqual(webui.choose_spool_dir("/x/y", self._MOUNTS), "/x/y")

    def test_default_tmpfs_falls_back_to_cwd(self):
        tmp = tempfile.gettempdir()
        mounts = "tmpfs %s tmpfs rw 0 0\n/dev/sda1 / ext4 rw 0 0\n" % tmp
        expected = os.path.join(os.getcwd(), ".hldsmon-spool")
        self.assertEqual(webui.choose_spool_dir(None, mounts), expected)

    def test_default_disk_kept(self):
        tmp = tempfile.gettempdir()
        mounts = "/dev/sda1 %s ext4 rw 0 0\n" % tmp
        self.assertEqual(webui.choose_spool_dir(None, mounts),
                         os.path.join(tmp, "hldsmon-spool"))

    def test_missing_mounts_default_is_tempdir(self):
        self.assertEqual(webui.choose_spool_dir(None, ""),
                         os.path.join(tempfile.gettempdir(),
                                      "hldsmon-spool"))


class TestSpoolFreeBytes(unittest.TestCase):
    def test_normal_dir_returns_non_negative_int(self):
        value = webui.spool_free_bytes(tempfile.gettempdir())
        self.assertIsInstance(value, int)
        self.assertGreaterEqual(value, 0)

    def test_missing_path_returns_none(self):
        missing = os.path.join(tempfile.gettempdir(),
                               "llspool-missing-" + "x" * 8)
        self.assertIsNone(webui.spool_free_bytes(missing))

    def test_no_statvfs_and_disk_usage_failure_returns_none(self):
        had_statvfs = hasattr(webui.os, "statvfs")
        saved_statvfs = getattr(webui.os, "statvfs", None)
        saved_usage = webui.shutil.disk_usage

        def _boom(path):
            raise OSError("disk_usage unavailable")

        webui.shutil.disk_usage = _boom
        if had_statvfs:
            del webui.os.statvfs
        try:
            self.assertIsNone(webui.spool_free_bytes(tempfile.gettempdir()))
        finally:
            webui.shutil.disk_usage = saved_usage
            if had_statvfs:
                webui.os.statvfs = saved_statvfs

    def test_disk_usage_without_statvfs_returns_int(self):
        class _Usage:
            free = 123456

        had_statvfs = hasattr(webui.os, "statvfs")
        saved_statvfs = getattr(webui.os, "statvfs", None)
        saved_usage = webui.shutil.disk_usage
        webui.shutil.disk_usage = lambda path: _Usage()
        if had_statvfs:
            del webui.os.statvfs
        try:
            self.assertEqual(webui.spool_free_bytes("x"), 123456)
        finally:
            webui.shutil.disk_usage = saved_usage
            if had_statvfs:
                webui.os.statvfs = saved_statvfs


class TestWindowsSpoolFreeFallback(_LLSpoolEnv):
    def test_analyze_survives_missing_statvfs(self):
        saved_usage = webui.shutil.disk_usage
        had_statvfs = hasattr(webui.os, "statvfs")
        saved_statvfs = getattr(webui.os, "statvfs", None)

        def _boom(path):
            raise OSError("disk_usage unavailable")

        webui.shutil.disk_usage = _boom
        if had_statvfs:
            del webui.os.statvfs
        try:
            status, _, raw = self._post("/api/analyze", fixture_bytes())
        finally:
            webui.shutil.disk_usage = saved_usage
            if had_statvfs:
                webui.os.statvfs = saved_statvfs
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw)["ok"])


class TestSpoolBudgets(_LLSpoolEnv):
    def test_session_budget_exhaustion_is_spool_full(self):
        webui._CONFIG.spool_session_bytes = 10
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 507)
        self.assertEqual(json.loads(raw)["code"], "spool_full")
        self.assertEqual(self._spool_files(), [])

    def test_process_budget_exhaustion_is_spool_full(self):
        webui._CONFIG.spool_total_bytes = 10
        webui._reset_spool_budget()
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 507)
        self.assertEqual(json.loads(raw)["code"], "spool_full")
        self.assertEqual(self._spool_files(), [])


class TestSpoolCleanup(unittest.TestCase):
    def setUp(self):
        self._saved = (webui._CONFIG.spool_dir, webui._CONFIG.spool_ttl)
        self._saved_budget = webui._SPOOL_BUDGET
        self.dir = tempfile.mkdtemp(prefix="llspool-")
        webui._CONFIG.spool_dir = self.dir
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        webui._reset_spool_budget()

    def tearDown(self):
        with webui._SPOOL_LOCK:
            webui._SPOOLS.clear()
        (webui._CONFIG.spool_dir, webui._CONFIG.spool_ttl) = self._saved
        webui._SPOOL_BUDGET = self._saved_budget
        shutil.rmtree(self.dir, ignore_errors=True)

    def _entry_with_file(self, payload=b"x"):
        entry = webui._spool_create()
        with open(entry.path, "wb") as fh:
            fh.write(payload)
        webui._spool_commit(entry, len(payload))
        return entry

    def test_sweep_removes_expired(self):
        webui._CONFIG.spool_ttl = 10.0
        entry = self._entry_with_file()
        self.assertIs(webui.spool_get(entry.handle), entry)
        removed = webui.spool_sweep(now=time.time() + 100)
        self.assertEqual(removed, 1)
        self.assertFalse(os.path.exists(entry.path))
        self.assertIsNone(webui.spool_get(entry.handle))

    def test_get_reaps_expired(self):
        webui._CONFIG.spool_ttl = 10.0
        entry = self._entry_with_file()
        entry.created = time.time() - 100
        self.assertIsNone(webui.spool_get(entry.handle))
        self.assertFalse(os.path.exists(entry.path))

    def test_remove_deletes_file(self):
        entry = self._entry_with_file(b"data")
        webui._spool_remove(entry.handle)
        self.assertFalse(os.path.exists(entry.path))
        self.assertIsNone(webui.spool_get(entry.handle))

    def test_startup_cleanup_removes_orphans_only(self):
        stray = os.path.join(self.dir, webui._SPOOL_PREFIX + "orphan.jsonl")
        keep = os.path.join(self.dir, "keep.txt")
        for path in (stray, keep):
            with open(path, "wb") as fh:
                fh.write(b"x")
        removed = webui.spool_startup_cleanup(self.dir)
        self.assertEqual(removed, 1)
        self.assertFalse(os.path.exists(stray))
        self.assertTrue(os.path.exists(keep))


class TestIngestErrorCleanup(_LLSpoolEnv):
    def test_read_error_removes_spool(self):
        original = webui.receive_body

        def _boom(*args, **kwargs):
            raise BrokenPipeError("client gone")

        webui.receive_body = _boom
        try:
            status, _, raw = self._post("/api/analyze", fixture_bytes())
        finally:
            webui.receive_body = original
        self.assertEqual(status, 400)
        self.assertEqual(self._spool_files(), [])
        with webui._SPOOL_LOCK:
            self.assertEqual(webui._SPOOLS, {})

    def test_timeout_removes_spool(self):
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 99\r\n\r\n" + b"partial")
        self.assertEqual(_http_status(resp), 408)
        self.assertEqual(self._spool_files(), [])


class TestBomIngest(_LLSpoolEnv):
    def test_bom_stripped_on_http_ingest(self):
        status, _, raw = self._post("/api/analyze",
                                    webui._BOM + fixture_bytes())
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["records_count"], 7)

    def test_bom_stripped_in_byte_api(self):
        payload = webui.analyze_bytes(webui._BOM + fixture_bytes())
        self.assertEqual(payload["records_count"], 7)

    def test_strip_bom_helper(self):
        self.assertEqual(webui._strip_bom(webui._BOM + b"x"), b"x")
        self.assertEqual(webui._strip_bom(b"x"), b"x")
        self.assertEqual(webui._strip_bom(webui._BOM), b"")


class TestSpoolHandleRegistry(_LLSpoolEnv):
    def test_analyze_returns_live_handle(self):
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 200)
        handle = json.loads(raw)["handle"]
        self.assertTrue(handle)
        entry = webui.spool_get(handle)
        self.assertIsNotNone(entry)
        self.assertTrue(os.path.exists(entry.path))
        webui._spool_remove(handle)
        self.assertIsNone(webui.spool_get(handle))
        self.assertEqual(self._spool_files(), [])


class TestUploadLimitAndTimeout(_LLSpoolEnv):
    def test_busy_when_no_upload_slot(self):
        webui._UPLOAD_SEM = threading.BoundedSemaphore(0)
        status, _, raw = self._post("/api/analyze", fixture_bytes())
        self.assertEqual(status, 429)
        self.assertEqual(json.loads(raw)["code"], "busy")

    def test_read_timeout_on_stalled_body_is_408(self):
        webui._CONFIG.read_timeout = 0.3
        resp = _raw_http(
            self.port,
            b"POST /api/analyze HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 100\r\n\r\n" + b"12345",
            read_timeout=5.0, half_close=False)
        self.assertEqual(_http_status(resp), 408)
        self.assertEqual(_http_json_body(resp)["code"], "request_timeout")
        self.assertEqual(self._spool_files(), [])

    def test_resource_guard_defaults(self):
        self.assertEqual(webui.MAX_UPLOADS, 4)
        self.assertEqual(webui.READ_TIMEOUT_S, 30.0)
        self.assertEqual(webui.SPOOL_SESSION_MB, 2048)
        self.assertEqual(webui.SPOOL_TOTAL_MB, 8192)
        self.assertEqual(webui.SPOOL_TTL_S, 3600)


_RSS_DRIVER = r"""
import os, resource, sys
sys.path.insert(0, sys.argv[2])
import webui

size = int(sys.argv[1])

class Source:
    def __init__(self, n):
        self.n = n
    def read(self, k):
        if self.n <= 0:
            return b""
        m = min(k, self.n)
        self.n -= m
        return b"x" * m

webui.receive_body(Source(size), os.devnull, size)
print(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
"""


class TestIngestMemory(unittest.TestCase):
    def _rss_kib(self, mib):
        size = mib * 1024 * 1024
        out = subprocess.run(
            [sys.executable, "-c", _RSS_DRIVER, str(size), _ANALYZER_DIR],
            capture_output=True, text=True, timeout=180)
        self.assertEqual(out.returncode, 0, out.stderr)
        return int(out.stdout.strip().splitlines()[-1])

    def test_ingest_memory_is_not_proportional_to_body(self):
        small = self._rss_kib(300)
        large = self._rss_kib(600)
        self.assertLessEqual(large - small, RSS_SLOPE_MAX_MIB * 1024)


# ---------------------------------------------------------------------------
# Stage 12 (large-logs) LL-P2: windowed analysis, coarse streaming aggregator,
# handle-based export/bundle. Covers QAS-L3/L6/L10/L13 (L4/L5 are the LL-P3
# FTP-segment scope and are not exercised here).
# ---------------------------------------------------------------------------

#: Analysis-memory bounds (plan LL-P2 / QAS-L10/L3).
RSS_ANALYSIS_MAX_MIB = 256
RSS_WINDOW_SENS_MIB = 32


def _windowed_log_bytes(span_records=240, step=30.0, base=10000.0):
    """A log spanning ``span_records*step`` seconds with periodic client/drop."""
    lines = []
    for i in range(span_records):
        tt = base + step * i
        lines.append(json.dumps({"type": "server", "t": tt, "fps": 60.0,
                                 "players": 3}))
        if i % 10 == 0:
            lines.append(json.dumps({"type": "client", "t": tt + 1, "id": 7,
                                     "userid": 7, "ping": 40, "loss": 1}))
            lines.append(json.dumps({"type": "drop", "t": tt + 2, "id": 7,
                                     "userid": 7, "reason": "Timeout",
                                     "category": "timeout"}))
    return ("\n".join(lines) + "\n").encode("utf-8")


def _reference_window(records, lo, hi):
    """Naive full-set client-side window filter (the LL-P2 reference)."""
    out = []
    for rec, run in webui._iter_pairs(records):
        t = hldsmon._record_ts(rec)
        if t is not None and lo <= t <= hi:
            out.append((rec, run))
    return out


class TestAnalyzeWindowContract(_LLSpoolEnv):
    def setUp(self):
        super().setUp()
        self.body = _windowed_log_bytes()
        self.records = webui._records_from_bytes(self.body)
        self.total = len(self.records)
        self.end = 10000.0 + 30.0 * 239

    def _analyze(self, query="", body=None):
        status, _, raw = self._post("/api/analyze" + query,
                                    self.body if body is None else body)
        self.assertEqual(status, 200, raw)
        return json.loads(raw)

    def test_response_contract_keys_and_no_records(self):
        payload = self._analyze()
        for key in ("ok", "version", "files", "handle", "window", "coarse",
                    "window_count", "digest", "clients", "sessions",
                    "session_count", "client_identities", "drops", "anomalies",
                    "anomalies_excluded", "events", "log_end", "config",
                    "records_count", "host_caveats", "series", "client_series",
                    "host_series", "focus_points"):
            self.assertIn(key, payload, "missing key %s" % key)
        self.assertNotIn("records", payload)
        self.assertIsInstance(payload["window"], dict)
        self.assertIn("from", payload["window"])
        self.assertIn("to", payload["window"])
        self.assertIsInstance(payload["handle"], str)
        self.assertIsInstance(payload["window_count"], int)
        self.assertTrue(payload["coarse"])
        self.assertEqual(payload["records_count"], self.total)
        for drop in payload["drops"]:
            self.assertEqual(set(drop["key"]), {"run", "t", "userid"})

    def test_default_window_ends_at_last_record_and_excludes_old(self):
        payload = self._analyze()
        self.assertEqual(payload["window"]["to"], self.end)
        self.assertEqual(payload["window"]["from"],
                         self.end - webui.ANALYZE_DEFAULT_WINDOW_S)
        self.assertLess(payload["window_count"], self.total)
        # the digest/aggregates stay full-day regardless of the window
        self.assertEqual(payload["digest"]["records"], self.total)

    def test_explicit_window_matches_client_filter(self):
        lo, hi = 10000.0, 10500.0
        payload = self._analyze("?from=%s&to=%s" % (lo, hi))
        self.assertFalse(payload["coarse"])
        self.assertEqual(payload["window"], {"from": lo, "to": hi})
        ref = _reference_window(self.records, lo, hi)
        self.assertEqual(payload["window_count"], len(ref))
        ref_server = [hldsmon.server_series_value(r, "t")
                      for r, _ in ref if r.get("type") == "server"]
        self.assertEqual(payload["series"]["t"], ref_server)
        for key, values in payload["series"].items():
            self.assertEqual(len(values), len(ref_server), key)

    def test_windowed_full_resolution_and_default_downsampled(self):
        payload = self._analyze("?from=10000&to=10500")
        ref_server = [r for r, _ in _reference_window(self.records, 10000.0,
                                                      10500.0)
                      if r.get("type") == "server"]
        self.assertEqual(len(payload["series"]["t"]), len(ref_server))
        self.assertLessEqual(len(payload["client_series"]["0:u7"]["t"]),
                             len(ref_server))

    def test_one_ended_window_uses_window_param(self):
        from_payload = self._analyze("?from=10000&window=300")
        self.assertEqual(from_payload["window"], {"from": 10000.0,
                                                   "to": 10300.0})
        ref = _reference_window(self.records, 10000.0, 10300.0)
        self.assertEqual(from_payload["window_count"], len(ref))
        to_payload = self._analyze("?to=10300&window=300")
        self.assertEqual(to_payload["window"], {"from": 10000.0,
                                                "to": 10300.0})

    def test_invalid_window_inputs_are_400(self):
        for query in ("?window=abc", "?window=0", "?from=abc", "?to=xyz"):
            status, _, raw = self._post("/api/analyze" + query, self.body)
            self.assertEqual(status, 400, query)
            self.assertFalse(json.loads(raw)["ok"])

    def test_constants(self):
        self.assertEqual(webui.ANALYZE_DEFAULT_WINDOW_S, 1800.0)
        self.assertEqual(webui.SERIES_MAX_POINTS, 2000)
        self.assertEqual(hldsmon.SERIES_MAX_POINTS, 2000)


class TestCoarseDownsampling(_LLSpoolEnv):
    def test_coarse_series_capped_at_series_max_points(self):
        n = webui.SERIES_MAX_POINTS + 5000
        lines = [json.dumps({"type": "server", "t": 5000.0 + i, "fps": 60.0})
                 for i in range(n)]
        body = ("\n".join(lines) + "\n").encode("utf-8")
        status, _, raw = self._post("/api/analyze", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertTrue(payload["coarse"])
        self.assertLessEqual(len(payload["series"]["t"]),
                             webui.SERIES_MAX_POINTS)
        self.assertEqual(payload["records_count"], n)

        full = self._post("/api/analyze?from=0&to=1000000", body)
        windowed = json.loads(full[2])
        self.assertFalse(windowed["coarse"])
        self.assertEqual(len(windowed["series"]["t"]), n)


class TestExportBundleViaHandle(_LLSpoolEnv):
    def _handle(self, body=None):
        body = _windowed_log_bytes() if body is None else body
        status, _, raw = self._post("/api/analyze", body)
        self.assertEqual(status, 200)
        return json.loads(raw)

    def _post_empty(self, path):
        return self._post(path, b"")

    def test_export_via_handle_full_and_windowed(self):
        payload = self._handle()
        handle = payload["handle"]
        status, _, raw = self._post_empty(
            "/api/export?handle=%s&type=drop" % handle)
        self.assertEqual(status, 200)
        lines = raw.decode("utf-8").strip().splitlines()
        self.assertTrue(lines[0].startswith("type,t,"))
        self.assertEqual(len(lines) - 1, len(payload["drops"]))

        status, _, raw = self._post_empty(
            "/api/export?handle=%s&type=drop&from=10000&to=10500" % handle)
        self.assertEqual(status, 200)
        self.assertEqual(len(raw.decode("utf-8").strip().splitlines()) - 1, 2)

    def test_bundle_via_handle_matches_reference(self):
        payload = self._handle()
        handle = payload["handle"]
        key = payload["drops"][0]["key"]
        status, _, raw = self._post_empty(
            "/api/bundle?handle=%s&run=%d&t=%s&userid=%s"
            % (handle, key["run"], key["t"], key["userid"]))
        self.assertEqual(status, 200)
        bundle = json.loads(raw)
        records = webui._records_from_bytes(_windowed_log_bytes())
        ref = webui.bundle_records(records, 0, 2.0)
        self.assertEqual(bundle["drop"], ref["drop"])
        self.assertEqual(bundle["window"], ref["window"])
        self.assertEqual(bundle["config"], ref["config"])
        self.assertEqual(bundle["host"], ref["host"])

    def test_bundle_unknown_key_is_400(self):
        handle = self._handle()["handle"]
        status, _, raw = self._post_empty(
            "/api/bundle?handle=%s&run=99&t=1&userid=7" % handle)
        self.assertEqual(status, 400)
        self.assertFalse(json.loads(raw)["ok"])

    def test_session_expired_on_evicted_handle(self):
        handle = self._handle()["handle"]
        webui._spool_remove(handle)
        for path in ("/api/export?handle=%s" % handle,
                     "/api/bundle?handle=%s&run=0&t=1&userid=7" % handle):
            status, _, raw = self._post_empty(path)
            self.assertEqual(status, 410, path)
            self.assertEqual(json.loads(raw)["code"], "session_expired")

    def test_small_body_path_preserved(self):
        status, _, raw = self._post("/api/export?type=drop",
                                    _windowed_log_bytes())
        self.assertEqual(status, 200)
        self.assertTrue(raw.decode("utf-8").startswith("type,t,"))
        status, _, raw = self._post("/api/bundle?drop=0&mins=2",
                                    _windowed_log_bytes())
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["drop"]["reason"], "Timeout")


class TestWindowRunPreservation(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="llwindow-")
        self.path = os.path.join(self.dir, "run.jsonl")
        lines = [
            {"type": "client", "t": 100.0, "id": 7, "userid": 7, "ping": 10},
            {"type": "shutdown", "t": 200.0, "reason": "graceful"},
            {"type": "client", "t": 300.0, "id": 7, "userid": 7, "ping": 20},
            {"type": "drop", "t": 310.0, "id": 7, "userid": 7,
             "reason": "Timeout", "category": "timeout"},
        ]
        with open(self.path, "w", encoding="utf-8") as fh:
            for rec in lines:
                fh.write(json.dumps(rec) + "\n")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_window_records_inclusive_and_run_aware(self):
        pairs = webui.window_records(self.path, 250.0, 320.0)
        self.assertEqual([hldsmon._record_ts(r) for r, _ in pairs],
                         [300.0, 310.0])
        for rec, run in pairs:
            self.assertEqual(run, 1)
            self.assertEqual(hldsmon.session_key(rec, run), "1:u7")
        self.assertEqual(webui.window_records(self.path, 1000.0, 2000.0), [])

    def test_coarse_overview_window_keeps_global_run_index(self):
        payload = webui.coarse_overview(self.path, from_ts=250.0, to_ts=320.0)
        self.assertFalse(payload["coarse"])
        self.assertIn("1:u7", payload["client_series"])
        names = [c["session"] for c in payload["clients"]]
        self.assertIn("1:u7", names)


#: Driver: analyze one window of a spool in a fresh process and print
#: ``window_count`` then peak RSS (KiB). Used by :class:`TestAnalysisMemory`.
_ANALYZE_RSS_DRIVER = r"""
import resource, sys
sys.path.insert(0, sys.argv[2])
import webui
payload = webui.coarse_overview(sys.argv[1],
                                from_ts=float(sys.argv[3]),
                                to_ts=float(sys.argv[4]))
print(payload["window_count"])
print(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
"""


def _write_big_log(path, n):
    """Write ``n`` one-second server records (+ periodic drops) to ``path``."""
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(n):
            t = 1000000.0 + i
            fh.write('{"type":"server","t":%.1f,"fps":60.0,"players":3,'
                     '"cpu_pct":10.0,"rss_kb":1000,"edicts":100}\n' % t)
            if i % 997 == 0:
                fh.write('{"type":"drop","t":%.1f,"id":7,"userid":7,'
                         '"reason":"Timeout","category":"timeout"}\n'
                         % (t + 0.5))


class TestAnalysisMemory(unittest.TestCase):
    """QAS-L10/L3: a window request stays bounded and is not file-proportional."""

    N = 800000

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="llanalyze-")
        cls.path = os.path.join(cls.dir, "big.jsonl")
        _write_big_log(cls.path, cls.N)
        cls.end = 1000000.0 + cls.N - 1

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def _run(self, lo, hi):
        out = subprocess.run(
            [sys.executable, "-c", _ANALYZE_RSS_DRIVER, self.path,
             _ANALYZER_DIR, str(lo), str(hi)],
            capture_output=True, text=True, timeout=300)
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = out.stdout.strip().splitlines()
        return int(lines[0]), int(lines[1])

    def test_analysis_memory_bounded_and_window_sensitive(self):
        five_count, five_rss = self._run(self.end - 300, self.end)
        thirty_count, thirty_rss = self._run(self.end - 1800, self.end)
        self.assertLessEqual(thirty_rss, RSS_ANALYSIS_MAX_MIB * 1024)
        self.assertLessEqual(abs(thirty_rss - five_rss),
                             RSS_WINDOW_SENS_MIB * 1024)
        self.assertGreater(thirty_count, five_count)
        self.assertLess(five_count, self.N)


if __name__ == "__main__":
    unittest.main()