#!/usr/bin/env python3
"""Unit + HTTP tests for the HldsMonitoring web UI (``webui.py``).

Run from ``tools/analyzer``::

    python3 -m unittest discover -s tests -v
"""
import json
import os
import sys
import threading
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
        self.calls = []

    def size(self, cfg):
        return len(self.data)

    def range(self, cfg, start, length):
        start = int(start)
        length = int(length)
        self.calls.append((start, length))
        return bytes(self.data[start:start + length])

    def download(self, cfg, offset=0):
        offset = int(offset or 0)
        self.calls.append(("download", offset))
        return len(self.data), bytes(self.data[offset:])


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
        self.remote = _FakeRemote()
        webui._remote_size = self.remote.size
        webui._download_range = self.remote.range
        webui._download = self.remote.download
        webui.FTP_BACK_CHUNK = 512
        with webui._FTP_LOCK:
            webui._FTP_SESSIONS.clear()
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


if __name__ == "__main__":
    unittest.main()
