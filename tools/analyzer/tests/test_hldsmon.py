#!/usr/bin/env python3
"""Unit tests for the HldsMonitoring offline analyzer.

Run from ``tools/analyzer``::

    python3 -m unittest discover -s tests -v
"""
import io
import json
import os
import sys
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import hldsmon  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def fixture(name):
    return os.path.join(FIXTURES, name)


def run_cli(*argv):
    """Invoke the CLI, returning (exit_code, stdout)."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = hldsmon.main(list(argv))
    return code, buf.getvalue()


def write_jsonl(records):
    import tempfile
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        for rec in records:
            fh.write(json.dumps(rec) + "\n")
    return path


GRACEFUL_RECORDS = [
    {"type": "server", "t": 1000.0, "frame_ms": {"min": 1.0, "max": 2.0}},
    {"type": "client", "t": 1001.0, "id": 1, "ping": 10},
    {"type": "mapchange", "t": 1005.0, "map": "de_nuke"},
    {"type": "crash", "t": 1010.0, "signal": 11},
    {"type": "shutdown", "t": 1050.0, "reason": "graceful"},
]

CRASH_RECORDS = [
    {"type": "client", "t": 2000.0, "id": 2, "ping": 5},
    {"type": "shutdown", "t": 1990.0, "reason": "graceful"},
    {"type": "crash", "t": 2000.0, "signal": 6},
]

NEITHER_RECORDS = [
    {"type": "server", "t": 3000.0},
]


def exclusion_fixture():
    return [
        {"type": "server", "t": 0.0},
        {"type": "hitch", "t": 100.0, "duration_ms": 120},
        {"type": "hitch", "t": 101.0, "duration_ms": 120},
        {"type": "hitch", "t": 102.0, "duration_ms": 120},
        {"type": "mapchange", "t": 101.0, "map": "de_dust2"},
        {"type": "client", "t": 300.0, "id": 5, "loss": 50},
        {"type": "client", "t": 300.5, "id": 5, "loss": 52},
        {"type": "client", "t": 495.0, "id": 6, "loss": 50},
        {"type": "client", "t": 496.0, "id": 6, "loss": 52},
        {"type": "shutdown", "t": 500.0, "reason": "graceful"},
    ]


class TestParserTolerance(unittest.TestCase):
    def test_truncated_final_line_ignored(self):
        records = hldsmon.parse_records([fixture("truncated.jsonl")])
        # server, client parsed; the unterminated final hitch line is dropped.
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["type"], "server")
        self.assertEqual(records[1]["type"], "client")

    def test_dirty_file_skips_blank_unknown_and_malformed(self):
        records = hldsmon.parse_records([fixture("dirty.jsonl")])
        types = [r["type"] for r in records]
        self.assertEqual(types, ["server", "client", "hitch"])

    def test_iter_records_never_raises_on_malformed(self):
        # A stream assembled in-memory via a temp file of garbage lines.
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl",
                                         delete=False) as fh:
            fh.write("\n")
            fh.write("  \n")
            fh.write("{not json\n")
            fh.write("[1,2,3]\n")
            fh.write('{"type":"nope","t":1}\n')
            fh.write('{"type":"hitch","t":2,"duration_ms":5}\n')
            path = fh.name
        try:
            records = hldsmon.parse_records([path])
        finally:
            os.unlink(path)
        self.assertEqual([r["type"] for r in records], ["hitch"])

    def test_parse_records_accepts_single_path_and_multiple(self):
        one = hldsmon.parse_records(fixture("truncated.jsonl"))
        two = hldsmon.parse_records([fixture("truncated.jsonl"),
                                     fixture("types_ok.jsonl")])
        self.assertEqual(len(one), 2)
        self.assertEqual(len(two), 2 + 15)


class TestSummary(unittest.TestCase):
    def test_summary_all_types(self):
        code, out = run_cli("summary", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Records: 15", out)
        self.assertIn("Players seen: 2 (7, 8)", out)
        self.assertIn("Reliable channel overflowed", out)
        self.assertIn("Hitch lines: 1", out)
        self.assertIn("Lag spikes (server frames >100ms): 0", out)
        self.assertIn("Config: 1 records", out)
        self.assertIn("Host: 1 records", out)
        for rtype in ("server", "client", "drop", "reject", "connect",
                      "netcmd", "massdrop", "packet", "hitch", "mapchange",
                      "voice", "config", "host", "degraded", "retention"):
            self.assertIn(rtype, out)

    def test_summary_deterministic(self):
        first = run_cli("summary", fixture("types_ok.jsonl"))[1]
        second = run_cli("summary", fixture("types_ok.jsonl"))[1]
        self.assertEqual(first, second)


class TestDrops(unittest.TestCase):
    def test_drops_listing(self):
        code, out = run_cli("drops", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Drops: 1", out)
        self.assertIn("Reliable channel overflowed", out)

    def test_drops_reason_filter(self):
        code, out = run_cli("drops", fixture("anomalies.jsonl"),
                            "--reason", "timeout")
        self.assertEqual(code, 0)
        self.assertIn("Drops: 3", out)

    def test_drops_client_filter(self):
        code, out = run_cli("drops", fixture("anomalies.jsonl"),
                            "--client", "11")
        self.assertEqual(code, 0)
        self.assertIn("Drops: 1", out)
        self.assertIn("id=11", out)

    def test_drops_since_until_epoch(self):
        code, out = run_cli("drops", fixture("anomalies.jsonl"),
                            "--since", "200.5", "--until", "201.5")
        self.assertEqual(code, 0)
        self.assertIn("Drops: 1", out)
        self.assertIn("id=11", out)

    def test_drops_since_until_iso(self):
        code, out = run_cli("drops", fixture("anomalies.jsonl"),
                            "--since", "1970-01-01T00:03:21.5+00:00")
        self.assertEqual(code, 0)
        self.assertIn("Drops: 1", out)  # only the 202 drop survives
        self.assertIn("id=12", out)


class TestWindow(unittest.TestCase):
    def test_window_around(self):
        code, out = run_cli("window", fixture("anomalies.jsonl"),
                            "--around", "201", "--secs", "1.5")
        self.assertEqual(code, 0)
        self.assertIn('"id":10', out)
        self.assertIn('"id":11', out)
        self.assertIn('"id":12', out)
        self.assertNotIn('"duration_ms"', out)

    def test_window_iso_around(self):
        code, out = run_cli("window", fixture("anomalies.jsonl"),
                            "--around", "1970-01-01T00:03:30Z", "--secs", "0.1")
        self.assertEqual(code, 0)
        self.assertIn("hitch", out)

    def test_window_bad_timestamp(self):
        code, out = run_cli("window", fixture("anomalies.jsonl"),
                            "--around", "not-a-time", "--secs", "1")
        self.assertEqual(code, 2)


class TestClients(unittest.TestCase):
    def test_clients_aggregates(self):
        code, out = run_cli("clients", fixture("anomalies.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Clients: 2", out)
        # client 3: 5 samples, ping avg (30+35+40+45+50)/5 = 40.
        self.assertIn("40", out)
        # client 4 peaked at 52 loss.
        self.assertIn("52", out)

    def test_clients_single(self):
        code, out = run_cli("clients", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Clients: 1", out)


class TestAnomalies(unittest.TestCase):
    def test_anomalies_detected(self):
        code, out = run_cli("anomalies", fixture("anomalies.jsonl"),
                            "--no-exclude")
        self.assertEqual(code, 0)
        self.assertIn("mass drops: 3 drops", out)
        self.assertIn("hitch cluster: 3 hitches", out)
        self.assertIn("reliable-fill: client 3", out)
        self.assertIn("loss spike: client 4", out)

    def test_anomalies_none_on_clean(self):
        code, out = run_cli("anomalies", fixture("truncated.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Anomalies: 0", out)


class TestDigest(unittest.TestCase):
    def test_digest_is_valid_json(self):
        code, out = run_cli("digest", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual(data["records"], 15)
        self.assertEqual(data["drop_count"], 1)
        self.assertEqual(data["players"], [7, 8])
        self.assertEqual(data["hitch_lines"], 1)
        self.assertEqual(data["counts_by_type"]["voice"], 1)
        self.assertEqual(data["counts_by_type"]["config"], 1)
        self.assertEqual(data["counts_by_type"]["host"], 1)
        self.assertEqual(data["top_drop_reasons"],
                         [["Reliable channel overflowed", 1]])

    def test_digest_config_block(self):
        data = hldsmon.build_digest(
            hldsmon.parse_records([fixture("types_ok.jsonl")]))
        self.assertEqual(data["config"]["records"], 1)
        latest = data["config"]["latest"]
        self.assertEqual(latest["map"], "de_dust2")
        self.assertEqual(latest["maxclients"], 32)
        self.assertEqual(latest["rehlds_major"], 3)
        self.assertEqual(latest["sv_maxupdaterate"], 101.0)

    def test_digest_config_null_tolerated(self):
        records = [{"type": "config", "t": 1.0, "map": "de_nuke",
                    "maxclients": 16, "sys_ticrate": None, "sv_maxrate": None}]
        data = hldsmon.build_digest(records)
        self.assertEqual(data["config"]["records"], 1)
        latest = data["config"]["latest"]
        self.assertIsNone(latest["sys_ticrate"])
        self.assertIsNone(latest["sv_maxrate"])

    def test_digest_deterministic(self):
        a = run_cli("digest", fixture("types_ok.jsonl"))[1]
        b = run_cli("digest", fixture("types_ok.jsonl"))[1]
        self.assertEqual(a, b)


class TestHost(unittest.TestCase):
    def test_known_types_include_host(self):
        self.assertIn("host", hldsmon.KNOWN_TYPES)

    def test_summary_host_line(self):
        code, out = run_cli("summary", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Host: 1 records", out)

    def test_host_bit_constants_match_schema(self):
        bits = list(hldsmon.HOST_PRESENT_BITS.values())
        self.assertEqual(len(bits), len(hldsmon.HOST_FIELDS))
        self.assertEqual(len(bits), len(set(bits)))
        self.assertEqual(hldsmon.HOST_PRESENT_BITS["udp_rcvbuf_errors"], 1)
        self.assertEqual(hldsmon.HOST_PRESENT_BITS["disk_bavail_mb"], 1 << 9)
        self.assertEqual(hldsmon.HOST_CAVEAT_BITS["udp_netns"], 1)
        self.assertEqual(hldsmon.HOST_CAVEAT_BITS["net_dev"], 1 << 1)
        self.assertEqual(hldsmon.HOST_CAVEAT_BITS["truncated"], 1 << 5)

    def test_host_series_null_tolerated(self):
        series = hldsmon.host_series(
            hldsmon.parse_records([fixture("host.jsonl")]))
        self.assertEqual(len(series["t"]), 3)
        # First sample has no Δ baseline => null, never a healthy zero.
        self.assertEqual(series["udp_rcvbuf_errors"], [12, None, 3])
        self.assertEqual(series["steal_pct"], [1.25, None, None])
        self.assertEqual(series["disk_bavail_mb"], [12345, 12300, None])
        self.assertEqual(series["present_mask"], [1023, 512, 1])
        self.assertEqual(series["caveat_mask"], [3, 1, 1])

    def test_digest_host_block_caveated_not_null(self):
        data = hldsmon.build_digest(
            hldsmon.parse_records([fixture("host.jsonl")]))
        self.assertEqual(data["host"]["records"], 3)
        latest = data["host"]["latest"]
        # Caveated value stays present (design M4), unavailable disk is null.
        self.assertEqual(latest["udp_rcvbuf_errors"], 3)
        self.assertIsNone(latest["disk_bavail_mb"])
        self.assertEqual(latest["present_mask"], 1)
        self.assertEqual(latest["caveat_mask"], 1)
        self.assertEqual(latest["caveats"], ["udp_netns"])

    def test_digest_host_null_tolerated(self):
        records = [{"type": "host", "t": 1.0, "udp_rcvbuf_errors": None,
                    "present_mask": 512, "caveat_mask": 0,
                    "disk_bavail_mb": 12345}]
        data = hldsmon.build_digest(records)
        latest = data["host"]["latest"]
        self.assertIsNone(latest["udp_rcvbuf_errors"])
        self.assertIsNone(latest["steal_pct"])
        self.assertEqual(latest["disk_bavail_mb"], 12345)
        self.assertEqual(latest["caveats"], [])


class TestEventEnrich(unittest.TestCase):
    def test_known_types_include_degraded_retention(self):
        self.assertIn("degraded", hldsmon.KNOWN_TYPES)
        self.assertIn("retention", hldsmon.KNOWN_TYPES)

    def test_degraded_retention_parse(self):
        records = hldsmon.parse_records([fixture("degraded.jsonl"),
                                         fixture("retention.jsonl")])
        self.assertEqual([r["type"] for r in records],
                         ["degraded", "degraded", "retention"])
        self.assertEqual(records[0]["error_count"], 3)
        self.assertEqual(records[2]["removed"], 2)

    def test_summary_degraded_retention_blocks(self):
        code, out = run_cli("summary", fixture("degraded.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Records: 2", out)
        self.assertIn("degraded", out)
        code, out = run_cli("summary", fixture("retention.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Records: 1", out)
        self.assertIn("retention", out)


class TestExportCsv(unittest.TestCase):
    def test_export_all(self):
        code, out = run_cli("export-csv", fixture("types_ok.jsonl"))
        self.assertEqual(code, 0)
        lines = out.strip().splitlines()
        self.assertEqual(len(lines), 1 + 15)  # header + records
        self.assertTrue(lines[0].startswith("type,t,"))

    def test_export_type_filter(self):
        code, out = run_cli("export-csv", fixture("types_ok.jsonl"),
                            "--type", "drop")
        self.assertEqual(code, 0)
        lines = out.strip().splitlines()
        self.assertEqual(len(lines), 1 + 1)
        self.assertIn("reason", lines[0])
        self.assertIn("Reliable channel overflowed", lines[1])

    def test_export_unknown_type(self):
        code, out = run_cli("export-csv", fixture("types_ok.jsonl"),
                            "--type", "bogus")
        self.assertEqual(code, 2)


class TestNewEventTypes(unittest.TestCase):
    def test_known_types_include_shutdown_and_crash(self):
        self.assertIn("shutdown", hldsmon.KNOWN_TYPES)
        self.assertIn("crash", hldsmon.KNOWN_TYPES)

    def test_known_types_include_config(self):
        self.assertIn("config", hldsmon.KNOWN_TYPES)

    def test_shutdown_and_crash_parse(self):
        path = write_jsonl([
            {"type": "shutdown", "t": 10.0, "reason": "graceful"},
            {"type": "crash", "t": 20.0, "signal": 11},
            {"type": "crash", "t": "bad"},
        ])
        self.addCleanup(os.unlink, path)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write('{"type":"shutdown","t":30.0\n')  # truncated tail
            fh.write("not json at all\n")
            fh.write("[1,2,3]\n")
        records = hldsmon.parse_records([path])
        self.assertEqual([r["type"] for r in records],
                         ["shutdown", "crash", "crash"])


class TestLogEndState(unittest.TestCase):
    def test_graceful_when_shutdown_is_terminal(self):
        self.assertEqual(hldsmon.log_end_state(GRACEFUL_RECORDS), "graceful")

    def test_graceful_when_only_shutdown(self):
        self.assertEqual(
            hldsmon.log_end_state([{"type": "shutdown", "t": 1.0}]),
            "graceful")

    def test_crash_when_crash_is_terminal(self):
        self.assertEqual(hldsmon.log_end_state(CRASH_RECORDS), "crash")

    def test_crash_when_only_crash(self):
        self.assertEqual(
            hldsmon.log_end_state([{"type": "crash", "t": 1.0, "signal": 9}]),
            "crash")

    def test_unknown_when_neither(self):
        self.assertEqual(hldsmon.log_end_state(NEITHER_RECORDS), "unknown")
        self.assertEqual(hldsmon.log_end_state([]), "unknown")


class TestExclusionWindows(unittest.TestCase):
    def test_windows_kinds_and_intervals_sorted(self):
        windows = hldsmon.exclusion_windows(GRACEFUL_RECORDS)
        self.assertEqual(windows, [
            {"kind": "warmup", "start": 1000.0, "end": 1010.0,
             "reason": "warmup"},
            {"kind": "mapchange", "start": 1000.0, "end": 1020.0,
             "reason": "de_nuke"},
            {"kind": "shutdown", "start": 1035.0, "end": 1050.0,
             "reason": "graceful"},
        ])
        starts = [w["start"] for w in windows]
        self.assertEqual(starts, sorted(starts))

    def test_crash_terminal_window(self):
        windows = hldsmon.exclusion_windows(CRASH_RECORDS)
        terminal = [w for w in windows if w["kind"] == "crash"]
        self.assertEqual(len(terminal), 1)
        self.assertEqual(terminal[0]["reason"], "crash")
        self.assertEqual(terminal[0]["start"], 1985.0)
        self.assertEqual(terminal[0]["end"], 2000.0)

    def test_terminal_window_start_clamped_to_zero(self):
        windows = hldsmon.exclusion_windows(
            [{"type": "server", "t": 1.0},
             {"type": "shutdown", "t": 5.0, "reason": "graceful"}])
        terminal = [w for w in windows if w["kind"] == "shutdown"][0]
        self.assertEqual(terminal["start"], 0.0)
        self.assertEqual(terminal["end"], 5.0)

    def test_no_windows_without_timestamps(self):
        self.assertEqual(
            hldsmon.exclusion_windows([{"type": "shutdown", "reason": "x"}]),
            [])


class TestFindAnomaliesExclusion(unittest.TestCase):
    def setUp(self):
        self.records = exclusion_fixture()

    def test_no_exclude_keeps_all_findings(self):
        findings = hldsmon.find_anomalies(self.records, exclude=False)
        self.assertEqual(len(findings), 3)
        self.assertTrue(any("hitch cluster: 3 hitches" in f for f in findings))
        self.assertTrue(any("loss spike: client 5" in f for f in findings))
        self.assertTrue(any("loss spike: client 6" in f for f in findings))

    def test_exclude_suppresses_window_findings(self):
        detail = hldsmon.find_anomalies_detail(self.records, exclude=True)
        self.assertEqual(detail["findings"],
                         ["loss spike: client 5 peaked at 52"])
        self.assertEqual(len(detail["excluded"]), 2)
        self.assertTrue(
            detail["excluded"][0].startswith("mapchange: hitch cluster:"))
        self.assertTrue(
            detail["excluded"][1].startswith("shutdown: loss spike: client 6"))

    def test_find_anomalies_default_excludes(self):
        self.assertEqual(hldsmon.find_anomalies(self.records),
                         hldsmon.find_anomalies(self.records, exclude=True))
        self.assertEqual(len(hldsmon.find_anomalies(self.records)), 1)


class TestClientSeries(unittest.TestCase):
    def test_parallel_arrays_and_nulls(self):
        records = [
            {"type": "client", "t": 1.0, "id": 7, "ping": 10, "loss": 1,
             "choke": 0, "rel_fill": 5, "dloss": 0, "delta_seq": 100,
             "idle_ms": 0},
            {"type": "client", "t": 2.0, "id": 7, "ping": 20, "loss": 2,
             "choke": 1, "rel_fill": 6, "dloss": 1, "delta_seq": 90,
             "idle_ms": 500},
            {"type": "client", "t": 3.0, "id": 8, "ping": 30},
            {"type": "server", "t": 4.0},
        ]
        series = hldsmon.client_series(records)
        self.assertEqual(set(series.keys()), {"0:i7:gNone", "0:i8:gNone"})
        s7 = series["0:i7:gNone"]
        self.assertEqual(set(s7.keys()),
                         {"t", "ping", "loss", "choke", "rel_fill",
                          "dloss", "delta_seq", "idle_ms",
                          "flow_in_kbps", "flow_out_kbps"})
        self.assertEqual(s7["t"], [1.0, 2.0])
        self.assertEqual(s7["ping"], [10, 20])
        self.assertEqual(s7["delta_seq"], [100, 90])
        self.assertEqual(s7["idle_ms"], [0, 500])
        self.assertEqual(series["0:i8:gNone"]["t"], [3.0])
        self.assertEqual(series["0:i8:gNone"]["ping"], [30])
        self.assertEqual(series["0:i8:gNone"]["loss"], [None])
        self.assertEqual(series["0:i8:gNone"]["rel_fill"], [None])
        self.assertEqual(series["0:i8:gNone"]["idle_ms"], [None])
        for key in series:
            lengths = {len(v) for v in series[key].values()}
            self.assertEqual(len(lengths), 1)

    def test_idle_ms_non_numeric_becomes_none(self):
        series = hldsmon.client_series([
            {"type": "client", "t": 1.0, "id": 7, "idle_ms": 1500},
            {"type": "client", "t": 2.0, "id": 7, "idle_ms": "x"},
            {"type": "client", "t": 3.0, "id": 7, "idle_ms": True},
        ])
        self.assertEqual(series["0:i7:gNone"]["idle_ms"], [1500, None, None])


    def test_non_numeric_values_become_none(self):
        series = hldsmon.client_series(
            [{"type": "client", "t": "bad", "id": 7, "ping": "x"}])
        self.assertEqual(series["0:i7:gNone"]["t"], [None])
        self.assertEqual(series["0:i7:gNone"]["ping"], [None])

    def test_flow_keys_and_nulls(self):
        series = hldsmon.client_series([
            {"type": "client", "t": 1.0, "id": 7,
             "flow_in_kbps": 4.5, "flow_out_kbps": 12.0},
            {"type": "client", "t": 2.0, "id": 7, "flow_in_kbps": "x"},
        ])
        s7 = series["0:i7:gNone"]
        self.assertEqual(s7["flow_in_kbps"], [4.5, None])
        self.assertEqual(s7["flow_out_kbps"], [12.0, None])


class TestLossRate(unittest.TestCase):
    def test_rolling_mean_window_and_gaps(self):
        self.assertEqual(hldsmon.rolling_mean([1, 2, 3, 4], 3),
                         [1, 1.5, 2, 3])
        self.assertEqual(hldsmon.rolling_mean([], 3), [])
        self.assertEqual(hldsmon.rolling_mean([5, 7], 0), [5, 7])
        self.assertEqual(hldsmon.rolling_mean([None, 10, None], 2),
                         [None, 10, 10])
        self.assertEqual(hldsmon.rolling_mean([None, None], 2),
                         [None, None])

    def test_loss_rate_fixture(self):
        records = hldsmon.parse_records([fixture("client_quality.jsonl")])
        rates = hldsmon.loss_rate_series(records)
        self.assertEqual(set(rates.keys()), {"0:u7"})
        vals = rates["0:u7"]
        self.assertEqual(len(vals), 6)
        self.assertEqual(vals[0], 0)
        self.assertEqual(vals[1], 0)
        self.assertAlmostEqual(vals[2], 100.0 / 3.0)
        self.assertEqual(vals[3], 25)
        self.assertEqual(vals[4], 20)
        self.assertEqual(vals[5], 30)

    def test_loss_rate_missing_sample_stays_none(self):
        records = [
            {"type": "client", "t": 1.0, "id": 7, "loss": 10},
            {"type": "client", "t": 2.0, "id": 7},
            {"type": "client", "t": 3.0, "id": 7, "loss": 20},
        ]
        rates = hldsmon.loss_rate_series(records, window=2)
        self.assertEqual(rates["0:i7:gNone"], [10, 10, 20])


class TestClientIdentities(unittest.TestCase):
    def setUp(self):
        self.records = [
            {"type": "client", "t": 10.0, "id": 7, "name": "Alice",
             "authid": "STEAM_0:0:1", "steamid64": "76561190000000001",
             "ip": "1.2.3.4", "port": 27005, "slot": 0, "userid": 7,
             "conn_s": 1.0, "fake": False, "proxy": False},
            {"type": "client", "t": 12.0, "id": 7, "name": "Alice2",
             "authid": "", "ip": "", "port": 27006, "userid": 7, "fake": True},
            {"type": "client", "t": 11.0, "id": 7, "name": "", "userid": 7},
            {"type": "client", "t": 20.0, "id": 8},
        ]
        self.identities = hldsmon.client_identities(self.records)

    def test_last_non_empty_identity(self):
        ident = self.identities["0:u7"]
        self.assertEqual(ident["name"], "Alice2")
        self.assertEqual(ident["authid"], "STEAM_0:0:1")
        self.assertEqual(ident["steamid64"], "76561190000000001")
        self.assertEqual(ident["ip"], "1.2.3.4")
        self.assertEqual(ident["port"], 27006)
        self.assertEqual(ident["slot"], 0)
        self.assertEqual(ident["userid"], 7)
        self.assertEqual(ident["conn_s"], 1.0)
        self.assertTrue(ident["fake"])
        self.assertFalse(ident["proxy"])

    def test_session_metadata(self):
        ident = self.identities["0:u7"]
        self.assertEqual(ident["session"], "0:u7")
        self.assertEqual(ident["run"], 0)
        self.assertEqual(ident["id"], 7)

    def test_counts_and_period(self):
        ident = self.identities["0:u7"]
        self.assertEqual(ident["samples"], 3)
        self.assertEqual(ident["first_t"], 10.0)
        self.assertEqual(ident["last_t"], 12.0)

    def test_defaults_for_missing_identity(self):
        ident = self.identities["0:i8:gNone"]
        self.assertEqual(ident["name"], "")
        self.assertEqual(ident["authid"], "")
        self.assertIsNone(ident["port"])
        self.assertIsNone(ident["userid"])
        self.assertIsNone(ident["fake"])
        self.assertIsNone(ident["proxy"])
        self.assertEqual(ident["samples"], 1)
        self.assertEqual(ident["first_t"], 20.0)
        self.assertEqual(ident["last_t"], 20.0)


class TestSessionKeys(unittest.TestCase):
    def test_session_key_uses_userid(self):
        self.assertEqual(
            hldsmon.session_key({"id": 0, "userid": 5}, 2), "2:u5")

    def test_session_key_falls_back_to_id_gen(self):
        self.assertEqual(
            hldsmon.session_key({"id": 3, "gen": 9}, 0), "0:i3:g9")
        self.assertEqual(
            hldsmon.session_key({"id": 3}, 0), "0:i3:gNone")

    def test_session_key_rejects_bad_userid(self):
        self.assertEqual(
            hldsmon.session_key({"id": 7, "userid": -1, "gen": 1}, 0),
            "0:i7:g1")
        self.assertEqual(
            hldsmon.session_key({"id": 7, "userid": 1.5, "gen": 1}, 0),
            "0:i7:g1")
        self.assertEqual(
            hldsmon.session_key({"id": 7, "userid": True, "gen": 1}, 0),
            "0:i7:g1")

    def test_session_key_none_without_userid_or_id(self):
        self.assertIsNone(hldsmon.session_key({"userid": None}, 0))

    def test_run_segmentation_on_shutdown_and_crash(self):
        records = [
            {"type": "client", "t": 1.0, "id": 0, "userid": 5},
            {"type": "mapchange", "t": 1.5, "map": "de_dust2"},
            {"type": "client", "t": 2.0, "id": 1, "userid": 6},
            {"type": "shutdown", "t": 3.0, "reason": "graceful"},
            {"type": "client", "t": 4.0, "id": 0, "userid": 5},
            {"type": "crash", "t": 5.0, "signal": 11},
            {"type": "client", "t": 6.0, "id": 0, "userid": 5},
        ]
        self.assertEqual(
            set(hldsmon.client_identities(records)),
            {"0:u5", "0:u6", "1:u5", "2:u5"})
        runs = {s: hldsmon.client_identities(records)[s]["run"]
                for s in hldsmon.client_identities(records)}
        self.assertEqual(runs["0:u5"], 0)
        self.assertEqual(runs["1:u5"], 1)
        self.assertEqual(runs["2:u5"], 2)

    def test_same_userid_across_runs_two_sessions(self):
        records = [
            {"type": "client", "t": 1.0, "id": 0, "userid": 5,
             "steamid64": "76561197960266730", "ping": 10},
            {"type": "shutdown", "t": 2.0, "reason": "graceful"},
            {"type": "client", "t": 3.0, "id": 1, "userid": 5,
             "steamid64": "76561197960266730", "ping": 20},
        ]
        self.assertEqual(set(hldsmon.client_series(records)),
                         {"0:u5", "1:u5"})
        self.assertEqual(set(hldsmon.aggregate_clients(records)),
                         {"0:u5", "1:u5"})

    def test_same_steamid_across_runs_one_player_group(self):
        records = [
            {"type": "client", "t": 1.0, "id": 0, "userid": 5,
             "steamid64": "76561197960266730"},
            {"type": "shutdown", "t": 2.0, "reason": "graceful"},
            {"type": "client", "t": 3.0, "id": 1, "userid": 9,
             "steamid64": "76561197960266730"},
        ]
        groups = hldsmon.player_groups(records)
        self.assertEqual(set(groups.keys()), {"76561197960266730"})
        group = groups["76561197960266730"]
        self.assertEqual(group["sessions"], ["0:u5", "1:u9"])
        self.assertEqual(group["first_t"], 1.0)
        self.assertEqual(group["last_t"], 3.0)

    def test_player_key_prefers_steamid_then_authid(self):
        self.assertEqual(
            hldsmon.player_key({"steamid64": "76561197960266730",
                                "authid": "STEAM_0:1:1",
                                "session": "0:u1"}),
            "76561197960266730")
        self.assertEqual(
            hldsmon.player_key({"steamid64": "",
                                "authid": "STEAM_0:1:1",
                                "session": "0:u1"}),
            "STEAM_0:1:1")

    def test_player_key_sentinels_fall_back_to_session(self):
        for sentinel in ("", "BOT", "STEAM_ID_LAN", "STEAM_ID_PENDING",
                         "VALVE_ID_LAN", "UNKNOWN"):
            self.assertEqual(
                hldsmon.player_key({"steamid64": "", "authid": sentinel,
                                    "session": "0:u4"}),
                "0:u4")
        self.assertEqual(
            hldsmon.player_key({"steamid64": "", "authid": "BOT"}), None)

    def test_player_groups_session_order_by_first_t(self):
        records = [
            {"type": "client", "t": 10.0, "id": 0, "userid": 1,
             "steamid64": "76561197960266730"},
            {"type": "shutdown", "t": 11.0, "reason": "graceful"},
            {"type": "client", "t": 5.0, "id": 0, "userid": 2,
             "steamid64": "76561197960266730"},
        ]
        group = hldsmon.player_groups(records)["76561197960266730"]
        self.assertEqual(group["sessions"], ["1:u2", "0:u1"])


class TestDigestExtended(unittest.TestCase):
    def test_digest_new_and_old_keys(self):
        digest = hldsmon.build_digest(GRACEFUL_RECORDS, files=["x.jsonl"])
        self.assertEqual(digest["log_end"], "graceful")
        self.assertIsInstance(digest["exclusions"], list)
        self.assertIsInstance(digest["anomalies_excluded"], int)
        for key in ("files", "records", "counts_by_type", "period", "players",
                    "sessions", "drop_count", "top_drop_reasons", "hitch_lines",
                    "lag_spikes", "log_end", "exclusions",
                    "anomalies_excluded"):
            self.assertIn(key, digest)

    def test_digest_anomalies_excluded_counts(self):
        digest = hldsmon.build_digest(exclusion_fixture())
        detail = hldsmon.find_anomalies_detail(exclusion_fixture())
        self.assertEqual(digest["anomalies_excluded"], len(detail["excluded"]))
        self.assertEqual(digest["anomalies_excluded"], 2)


class TestAnomaliesCliExclusion(unittest.TestCase):
    def _path(self):
        path = write_jsonl(exclusion_fixture())
        self.addCleanup(os.unlink, path)
        return path

    def test_default_excludes_and_reports_count(self):
        code, out = run_cli("anomalies", self._path())
        self.assertEqual(code, 0)
        self.assertIn("Anomalies: 1", out)
        self.assertIn("Excluded by shutdown/mapchange/warmup: 2", out)

    def test_no_exclude_keeps_all(self):
        code, out = run_cli("anomalies", self._path(), "--no-exclude")
        self.assertEqual(code, 0)
        self.assertIn("Anomalies: 3", out)
        self.assertNotIn("Excluded by shutdown/mapchange/warmup:", out)

    def test_summary_respects_no_exclude(self):
        path = self._path()
        code, out = run_cli("summary", path)
        self.assertEqual(code, 0)
        self.assertIn("Anomalies: 1", out)
        code, out = run_cli("summary", path, "--no-exclude")
        self.assertEqual(code, 0)
        self.assertIn("Anomalies: 3", out)


def focus_fixture():
    """One silent client that times out, plus a short noisy blip per run."""
    return [
        # Session 0:u2: idle_ms ramps 10 -> 6200 ms at 1 Hz, then times out.
        {"type": "client", "t": 100.0, "id": 2, "userid": 2, "slot": 2,
         "name": "Alice", "steamid64": "76561197960266730", "idle_ms": 10},
        {"type": "client", "t": 101.0, "id": 2, "userid": 2, "idle_ms": 1200},
        {"type": "client", "t": 102.0, "id": 2, "userid": 2, "idle_ms": 2400},
        {"type": "client", "t": 103.0, "id": 2, "userid": 2, "idle_ms": 3600},
        {"type": "client", "t": 104.0, "id": 2, "userid": 2, "idle_ms": 4900},
        {"type": "client", "t": 105.0, "id": 2, "userid": 2, "idle_ms": 6200},
        {"type": "drop", "t": 130.0, "id": 2, "userid": 2, "reason": "Timed out"},
        # Session 0:u3: short, non-monotonic blip -> must not be flagged.
        {"type": "client", "t": 100.0, "id": 3, "userid": 3, "idle_ms": 100},
        {"type": "client", "t": 101.0, "id": 3, "userid": 3, "idle_ms": 300},
        {"type": "client", "t": 102.0, "id": 3, "userid": 3, "idle_ms": 200},
        {"type": "client", "t": 103.0, "id": 3, "userid": 3, "idle_ms": 400},
    ]


class TestFocusPoints(unittest.TestCase):
    def setUp(self):
        self.records = focus_fixture()

    def test_single_idle_hang_shape(self):
        points = hldsmon.focus_points(self.records)
        self.assertEqual(len(points), 1)
        fp = points[0]
        self.assertEqual(fp["kind"], "idle_hang")
        self.assertEqual(fp["session"], "0:u2")
        self.assertEqual(fp["player"], "76561197960266730")
        self.assertEqual(fp["start_t"], 100.0)
        self.assertEqual(fp["end_t"], 105.0)
        self.assertEqual(fp["duration_s"], 5.0)
        self.assertEqual(fp["peak_idle_ms"], 6200.0)
        self.assertTrue(fp["drop_after"])
        self.assertEqual(fp["slot"], 2)
        self.assertEqual(fp["userid"], 2)
        self.assertEqual(fp["name"], "Alice")
        self.assertEqual(fp["steamid64"], "76561197960266730")

    def test_short_nonmonotonic_blip_not_flagged(self):
        sessions = [fp["session"] for fp in hldsmon.focus_points(self.records)]
        self.assertNotIn("0:u3", sessions)

    def test_idle_min_s_threshold(self):
        self.assertEqual(
            hldsmon.focus_points(self.records, idle_min_s=6.0), [])

    def test_idle_growth_ms_threshold(self):
        self.assertEqual(
            hldsmon.focus_points(self.records, idle_growth_ms=10000.0), [])

    def test_missing_drop_clears_drop_after(self):
        records = [r for r in self.records if r["type"] != "drop"]
        points = hldsmon.focus_points(records)
        self.assertEqual(len(points), 1)
        self.assertFalse(points[0]["drop_after"])

    def test_malformed_input_returns_list(self):
        self.assertEqual(hldsmon.focus_points([]), [])
        weird = [
            {"type": "client", "t": "x", "id": 1, "userid": 1,
             "idle_ms": "bad"},
            {"type": "client", "t": None, "id": 1, "userid": 1, "idle_ms": 5},
            {"type": "not-a-real-type", "t": 1.0},
        ]
        self.assertIsInstance(hldsmon.focus_points(weird), list)


class TestDigestFocusPoints(unittest.TestCase):
    def test_digest_counts_focus_points(self):
        digest = hldsmon.build_digest(focus_fixture())
        self.assertEqual(digest["focus_point_count"], 1)

    def test_digest_focus_point_count_zero_when_clean(self):
        digest = hldsmon.build_digest(GRACEFUL_RECORDS)
        self.assertEqual(digest["focus_point_count"], 0)


class TestModnetKnownType(unittest.TestCase):
    def test_known_types_include_modnet(self):
        self.assertIn("modnet", hldsmon.KNOWN_TYPES)

    def test_modnet_without_schema_fields_is_tolerated(self):
        records = [
            {"type": "modnet", "t": "bad", "module": 5, "so": None,
             "in_kbps": "x", "conn_fail": True},
            {"type": "server", "t": 1.0},
        ]
        series = hldsmon.modnet_series(records)
        self.assertEqual(set(series), {"5||0"})
        self.assertEqual(series["5||0"]["t"], [None])
        self.assertEqual(series["5||0"]["conn_fail"], [None])
        self.assertEqual(series["5||0"]["in_kbps"], [None])
        self.assertEqual(hldsmon.correlate_modnet(records), [])
        summary = hldsmon.modnet_summary(records)
        self.assertEqual(summary["5"]["in_bytes"], 0)
        self.assertIsNone(summary["5"]["peak_in_kbps"])


class TestModnetSeries(unittest.TestCase):
    def setUp(self):
        self.records = hldsmon.parse_records([fixture("modnet.jsonl")])
        self.series = hldsmon.modnet_series(self.records)

    def test_keys_split_by_module_and_so(self):
        self.assertEqual(set(self.series), {
            "easy_http_amxx_i386|easy_http_amxx_i386.so|0",
            "sockets_amxx_i386|sockets_amxx_i386.so|0",
        })

    def test_easy_http_arrays_and_gap_as_zero(self):
        entry = self.series["easy_http_amxx_i386|easy_http_amxx_i386.so|0"]
        # No sample at 3001/3003..: a quiet interval adds nothing (gap == zero).
        self.assertEqual(entry["t"], [3000.0, 3002.0, 3010.0])
        self.assertEqual(entry["conns"], [1, 0, 0])
        self.assertEqual(entry["conn_fail"], [0, 0, 3])
        self.assertEqual(entry["connect_attempts"], [1, 0, 3])
        self.assertEqual(entry["dns_calls"], [1, 0, 1])
        self.assertEqual(entry["in_bytes"], [1024, 204800, 0])
        self.assertEqual(entry["out_bytes"], [2048, 102400, 0])
        self.assertEqual(entry["in_kbps"], [1.0, 200.0, 0.0])
        self.assertEqual(entry["out_kbps"], [2.0, 100.0, 0.0])

    def test_sockets_arrays(self):
        entry = self.series["sockets_amxx_i386|sockets_amxx_i386.so|0"]
        self.assertEqual(entry["t"], [3001.0, 3100.0])
        self.assertEqual(entry["conn_fail"], [2, 0])
        self.assertEqual(entry["in_bytes"], [0, 4096])
        self.assertEqual(entry["in_kbps"], [0.0, 4.0])

    def test_parallel_array_lengths(self):
        for entry in self.series.values():
            lengths = {len(values) for values in entry.values()}
            self.assertEqual(len(lengths), 1)

    def test_missing_fields_become_none(self):
        series = hldsmon.modnet_series([
            {"type": "modnet", "t": 1.0, "module": "m", "so": "m.so",
             "in_bytes": 10},
        ])
        self.assertEqual(series["m|m.so|0"]["in_bytes"], [10])
        self.assertEqual(series["m|m.so|0"]["conn_fail"], [None])
        self.assertEqual(series["m|m.so|0"]["in_kbps"], [None])

    def test_run_boundary_splits_series(self):
        records = [
            {"type": "modnet", "t": 1.0, "module": "m", "so": "m.so",
             "in_bytes": 10},
            {"type": "shutdown", "t": 2.0, "reason": "graceful"},
            {"type": "modnet", "t": 3.0, "module": "m", "so": "m.so",
             "in_bytes": 20},
            {"type": "modnet", "t": 3.5, "module": "m", "so": "other.so",
             "in_bytes": 5},
        ]
        series = hldsmon.modnet_series(records)
        self.assertEqual(set(series),
                         {"m|m.so|0", "m|m.so|1", "m|other.so|1"})
        self.assertEqual(series["m|m.so|0"]["in_bytes"], [10])
        self.assertEqual(series["m|m.so|1"]["in_bytes"], [20])
        self.assertEqual(series["m|other.so|1"]["in_bytes"], [5])

    def test_modnet_key_helper(self):
        self.assertEqual(
            hldsmon.modnet_key({"module": "a", "so": "a.so"}, 2), "a|a.so|2")
        self.assertEqual(hldsmon.modnet_key({}, 0), "||0")


class TestModnetSummary(unittest.TestCase):
    def setUp(self):
        self.records = hldsmon.parse_records([fixture("modnet.jsonl")])
        self.summary = hldsmon.modnet_summary(self.records)

    def test_modules_present(self):
        self.assertEqual(set(self.summary),
                         {"easy_http_amxx_i386", "sockets_amxx_i386"})

    def test_easy_http_totals_and_peaks(self):
        easy = self.summary["easy_http_amxx_i386"]
        self.assertEqual(easy["in_bytes"], 205824)
        self.assertEqual(easy["out_bytes"], 104448)
        self.assertEqual(easy["conns"], 1)
        self.assertEqual(easy["conn_fail"], 3)
        self.assertEqual(easy["connect_attempts"], 4)
        self.assertEqual(easy["dns_calls"], 2)
        self.assertEqual(easy["peak_in_kbps"], 200.0)
        self.assertEqual(easy["peak_out_kbps"], 100.0)

    def test_sockets_totals_and_peaks(self):
        sockets = self.summary["sockets_amxx_i386"]
        self.assertEqual(sockets["in_bytes"], 4096)
        self.assertEqual(sockets["out_bytes"], 8192)
        self.assertEqual(sockets["conns"], 1)
        self.assertEqual(sockets["conn_fail"], 2)
        self.assertEqual(sockets["connect_attempts"], 4)
        self.assertEqual(sockets["dns_calls"], 0)
        self.assertEqual(sockets["peak_in_kbps"], 4.0)
        self.assertEqual(sockets["peak_out_kbps"], 8.0)

    def test_empty_records(self):
        self.assertEqual(hldsmon.modnet_summary([]), {})
        self.assertEqual(hldsmon.modnet_summary(GRACEFUL_RECORDS), {})

    def test_deterministic(self):
        self.assertEqual(hldsmon.modnet_summary(self.records),
                         hldsmon.modnet_summary(self.records))


class TestCorrelateModnet(unittest.TestCase):
    def setUp(self):
        self.records = hldsmon.parse_records([fixture("modnet.jsonl")])

    def test_correlation_rows(self):
        expected = [
            {"event_t": 3001.5, "kind": "hitch",
             "module": "easy_http_amxx_i386",
             "so": "easy_http_amxx_i386.so", "metric": "in_kbps",
             "value": 200.0},
            {"event_t": 3001.5, "kind": "hitch",
             "module": "easy_http_amxx_i386",
             "so": "easy_http_amxx_i386.so", "metric": "out_kbps",
             "value": 100.0},
            {"event_t": 3001.5, "kind": "hitch",
             "module": "sockets_amxx_i386",
             "so": "sockets_amxx_i386.so", "metric": "conn_fail",
             "value": 2},
            {"event_t": 3010.5, "kind": "drop",
             "module": "easy_http_amxx_i386",
             "so": "easy_http_amxx_i386.so", "metric": "conn_fail",
             "value": 3},
        ]
        self.assertEqual(hldsmon.correlate_modnet(self.records), expected)

    def test_narrow_window_excludes_off_grid_samples(self):
        self.assertEqual(
            hldsmon.correlate_modnet(self.records, window_s=0.1), [])

    def test_no_modnet_anomalies_yields_nothing(self):
        records = [
            {"type": "modnet", "t": 1.0, "module": "m", "so": "m.so",
             "in_kbps": 1.0, "conn_fail": 0},
            {"type": "drop", "t": 1.0, "id": 1, "reason": "Timeout"},
        ]
        self.assertEqual(hldsmon.correlate_modnet(records), [])

    def test_deterministic(self):
        self.assertEqual(hldsmon.correlate_modnet(self.records),
                         hldsmon.correlate_modnet(self.records))

    def test_empty_input(self):
        self.assertEqual(hldsmon.correlate_modnet([]), [])


class TestModnetDigestAndCli(unittest.TestCase):
    def setUp(self):
        self.records = hldsmon.parse_records([fixture("modnet.jsonl")])

    def test_digest_modnet_block(self):
        block = hldsmon.build_digest(self.records)["modnet"]
        self.assertEqual(block["records"], 5)
        self.assertEqual(block["in_bytes"], 209920)
        self.assertEqual(block["out_bytes"], 112640)
        self.assertEqual(block["conn_fail"], 5)
        self.assertEqual(block["modules"], [
            ["easy_http_amxx_i386", 205824, 104448, 3],
            ["sockets_amxx_i386", 4096, 8192, 2],
        ])

    def test_digest_counts_by_type_has_modnet(self):
        digest = hldsmon.build_digest(self.records)
        self.assertEqual(digest["counts_by_type"]["modnet"], 5)

    def test_digest_cli_deterministic(self):
        first = run_cli("digest", fixture("modnet.jsonl"))[1]
        second = run_cli("digest", fixture("modnet.jsonl"))[1]
        self.assertEqual(first, second)
        data = json.loads(first)
        self.assertEqual(data["modnet"]["records"], 5)

    def test_summary_cli_modnet_line(self):
        code, out = run_cli("summary", fixture("modnet.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn(
            "Modnet: 5 records, modules: easy_http_amxx_i386, sockets_amxx_i386",
            out)
        self.assertEqual(run_cli("summary", fixture("modnet.jsonl"))[1], out)

    def test_summary_cli_without_modnet(self):
        code, out = run_cli("summary", fixture("truncated.jsonl"))
        self.assertEqual(code, 0)
        self.assertIn("Modnet: 0 records, modules: (none)", out)


class TestConfigAt(unittest.TestCase):
    def setUp(self):
        self.records = [
            {"type": "config", "t": 100.0, "map": "a"},
            {"type": "server", "t": 150.0},
            {"type": "config", "t": 200.0, "map": "b"},
            {"type": "config", "t": 300.0, "map": "c"},
        ]

    def test_last_at_or_before(self):
        self.assertEqual(hldsmon.config_at(self.records, 250.0)["map"], "b")

    def test_boundary_is_inclusive(self):
        self.assertEqual(hldsmon.config_at(self.records, 200.0)["map"], "b")

    def test_tie_later_file_order_wins(self):
        records = [
            {"type": "config", "t": 10.0, "map": "first"},
            {"type": "config", "t": 10.0, "map": "second"},
        ]
        self.assertEqual(hldsmon.config_at(records, 10.0)["map"], "second")

    def test_none_before_all(self):
        self.assertIsNone(hldsmon.config_at(self.records, 99.9))

    def test_returns_the_record_not_a_copy(self):
        self.assertIs(hldsmon.config_at(self.records, 300.0), self.records[3])

    def test_unusable_timestamp_is_none(self):
        self.assertIsNone(hldsmon.config_at(self.records, None))
        self.assertIsNone(hldsmon.config_at(self.records, "nope"))

    def test_no_config_records(self):
        self.assertIsNone(
            hldsmon.config_at([{"type": "server", "t": 1.0}], 5.0))


class TestConfigDiff(unittest.TestCase):
    def test_changed_fields_carry_both_values(self):
        before = {"type": "config", "t": 1.0, "sys_ticrate": 100.0,
                  "map": "a", "maxclients": 32}
        after = {"type": "config", "t": 2.0, "sys_ticrate": 110.0,
                 "map": "a", "maxclients": 16}
        diff = hldsmon.config_diff(before, after)
        self.assertTrue(diff["baseline"])
        self.assertEqual(diff["changed"], [
            {"field": "sys_ticrate", "before": 100.0, "after": 110.0},
            {"field": "maxclients", "before": 32, "after": 16},
        ])
        self.assertEqual(diff["added"], [])
        self.assertEqual(diff["removed"], [])

    def test_null_side_is_set_or_cleared_not_changed(self):
        before = {"sys_ticrate": None, "sv_timeout": 65.0, "map": "de_dust2"}
        after = {"sys_ticrate": 100.0, "sv_timeout": None, "map": "de_dust2"}
        diff = hldsmon.config_diff(before, after)
        self.assertEqual(diff["changed"], [])
        self.assertEqual(diff["added"], [
            {"field": "sys_ticrate", "before": None, "after": 100.0}])
        self.assertEqual(diff["removed"], [
            {"field": "sv_timeout", "before": 65.0, "after": None}])

    def test_missing_key_equals_null(self):
        diff = hldsmon.config_diff({"sv_minrate": None}, {})
        self.assertEqual(diff["changed"], [])
        self.assertEqual(diff["added"], [])
        self.assertEqual(diff["removed"], [])

    def test_no_baseline_is_explicit_and_empty(self):
        after = {"sys_ticrate": 100.0, "map": "de_dust2"}
        for before in (None, "garbage", 7, []):
            diff = hldsmon.config_diff(before, after)
            self.assertFalse(diff["baseline"], repr(before))
            self.assertEqual(diff["changed"], [])
            self.assertEqual(diff["added"], [])
            self.assertEqual(diff["removed"], [])

    def test_missing_after_clears_present_fields(self):
        diff = hldsmon.config_diff(
            {"sys_ticrate": 100.0, "map": "a", "rehlds_major": 3}, None)
        self.assertTrue(diff["baseline"])
        self.assertEqual([e["field"] for e in diff["removed"]],
                         ["sys_ticrate", "map", "rehlds_major"])
        self.assertEqual(diff["added"], [])

    def test_non_numeric_value_is_absent(self):
        diff = hldsmon.config_diff({"sys_ticrate": "fast"},
                                   {"sys_ticrate": 100.0})
        self.assertEqual([e["field"] for e in diff["added"]], ["sys_ticrate"])
        self.assertEqual(diff["changed"], [])

    def test_equal_snapshot_has_no_changes(self):
        rec = {"sys_ticrate": 100, "map": "a", "maxclients": 32.0,
               "rehlds_major": 3, "rehlds_minor": 14}
        diff = hldsmon.config_diff(rec, dict(rec, t=99.0))
        self.assertTrue(diff["baseline"])
        self.assertEqual(diff["changed"], [])
        self.assertEqual(diff["added"], [])
        self.assertEqual(diff["removed"], [])

    def test_field_set_is_config_fields_plus_context(self):
        self.assertEqual(
            tuple(hldsmon.CONFIG_DIFF_FIELDS),
            tuple(hldsmon.CONFIG_FIELDS) + ("map", "maxclients",
                                            "rehlds_major", "rehlds_minor"))

    def test_unknown_field_is_ignored(self):
        diff = hldsmon.config_diff({"fps": 60.0}, {"fps": 30.0})
        self.assertEqual(diff["changed"], [])


class TestConfigSeries(unittest.TestCase):
    def test_snapshots_carry_field_set_and_diffs(self):
        records = [
            {"type": "config", "t": 100.0, "map": "a", "sys_ticrate": 100.0},
            {"type": "config", "t": 200.0, "map": "b", "sys_ticrate": 100.0},
        ]
        series = hldsmon.config_series(records)
        self.assertEqual([s["t"] for s in series], [100.0, 200.0])
        for snap in series:
            for field in hldsmon.CONFIG_DIFF_FIELDS:
                self.assertIn(field, snap)
        self.assertFalse(series[0]["diff"]["baseline"])
        self.assertEqual(series[1]["diff"]["changed"],
                         [{"field": "map", "before": "a", "after": "b"}])

    def test_window_baseline_is_taken_before_low(self):
        records = [
            {"type": "config", "t": 10.0, "sys_ticrate": 100.0},
            {"type": "config", "t": 100.0, "sys_ticrate": 120.0},
            {"type": "config", "t": 300.0, "sys_ticrate": 90.0},
        ]
        series = hldsmon.config_series(records, low=50.0, high=200.0)
        self.assertEqual(len(series), 1)
        self.assertTrue(series[0]["diff"]["baseline"])
        self.assertEqual(series[0]["diff"]["changed"],
                         [{"field": "sys_ticrate",
                           "before": 100.0, "after": 120.0}])

    def test_untimed_config_records_are_skipped(self):
        series = hldsmon.config_series(
            [{"type": "config"}, {"type": "config", "t": 5.0}])
        self.assertEqual([s["t"] for s in series], [5.0])


class TestHostFieldCaveats(unittest.TestCase):
    def test_every_host_field_is_mapped(self):
        self.assertEqual(set(hldsmon.HOST_FIELD_CAVEATS),
                         set(hldsmon.HOST_FIELDS))

    def test_expected_sources(self):
        self.assertEqual(hldsmon.HOST_FIELD_CAVEATS, {
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
        })

    def test_sources_are_known_caveat_bits(self):
        for source in hldsmon.HOST_FIELD_CAVEATS.values():
            self.assertIn(source, hldsmon.HOST_CAVEAT_BITS)

    def test_bits_are_flattened_sources(self):
        for field, source in hldsmon.HOST_FIELD_CAVEATS.items():
            self.assertEqual(hldsmon.HOST_FIELD_CAVEAT_BITS[field],
                             hldsmon.HOST_CAVEAT_BITS[source])


def _bundle_records():
    return [
        {"type": "server", "t": 1000.0, "fps": 60.0},
        {"type": "config", "t": 1000.5, "map": "de_dust2", "maxclients": 32,
         "rehlds_major": 3, "rehlds_minor": 14, "sys_ticrate": 100.0,
         "sv_timeout": 65.0},
        {"type": "host", "t": 1001.0, "udp_rcvbuf_errors": 5,
         "present_mask": 1, "caveat_mask": 1},
        {"type": "drop", "t": 1002.0, "id": 7, "reason": "Timeout",
         "category": "timeout", "window": [{"t": 1001.0, "ping": 40}]},
        {"type": "client", "t": 1002.0, "id": 7, "ping": 45},
        {"type": "server", "t": 1010.0, "fps": 59.0},
    ]


class TestIncidentBundle(unittest.TestCase):
    def setUp(self):
        self.records = _bundle_records()
        self.drop = self.records[3]
        self.bundle = hldsmon.incident_bundle(
            self.records, self.drop, mins=0.05)

    def test_envelope_keys_and_version(self):
        self.assertEqual(self.bundle["bundle_version"], 1)
        for key in ("drop", "window", "config", "host", "records", "warnings"):
            self.assertIn(key, self.bundle)
        self.assertIsInstance(self.bundle["warnings"], list)

    def test_drop_is_embedded_as_is(self):
        self.assertIs(self.bundle["drop"], self.drop)

    def test_window_bounds_around_drop(self):
        self.assertEqual(self.bundle["window"],
                         {"start": 1002.0 - 3.0, "end": 1002.0 + 3.0})

    def test_dedup_default_omits_raw_records(self):
        self.assertEqual(self.bundle["records"], [])
        self.assertTrue(any("dedup policy" in w for w in self.bundle["warnings"]))

    def test_forced_records_exclude_the_embedded_drop(self):
        bundle = hldsmon.incident_bundle(
            self.records, self.drop, mins=0.05, include_records=True)
        types = [r["type"] for r in bundle["records"]]
        self.assertEqual(types, ["server", "config", "host", "client"])
        self.assertNotIn(self.drop, bundle["records"])
        self.assertTrue(any("dedup policy" in w for w in bundle["warnings"]))

    def test_no_center_uses_full_span_and_keeps_records(self):
        bundle = hldsmon.incident_bundle(self.records, None)
        self.assertIsNone(bundle["drop"])
        self.assertEqual(bundle["window"], {"start": 1000.0, "end": 1010.0})
        self.assertEqual(bundle["records"], self.records)

    def test_timestamp_center(self):
        bundle = hldsmon.incident_bundle(self.records, 1002.0, mins=0.05)
        self.assertIsNone(bundle["drop"])
        self.assertEqual(bundle["window"]["start"], 999.0)
        self.assertEqual([r["type"] for r in bundle["records"]],
                         ["server", "config", "host", "drop", "client"])

    def test_config_block_carries_snapshot(self):
        self.assertEqual(len(self.bundle["config"]), 1)
        snap = self.bundle["config"][0]
        self.assertEqual(snap["map"], "de_dust2")
        self.assertEqual(snap["sys_ticrate"], 100.0)
        self.assertFalse(snap["diff"]["baseline"])

    def test_config_block_falls_back_to_active_snapshot(self):
        bundle = hldsmon.incident_bundle(self.records, 1010.0, mins=0.001)
        self.assertEqual(len(bundle["config"]), 1)
        self.assertEqual(bundle["config"][0]["t"], 1000.5)
        self.assertFalse(bundle["config"][0]["diff"]["baseline"])

    def test_host_block_carries_series(self):
        host = self.bundle["host"]
        self.assertEqual(host["records"], 1)
        self.assertEqual(host["series"]["t"], [1001.0])
        self.assertEqual(host["series"]["udp_rcvbuf_errors"], [5])
        self.assertEqual(host["series"]["caveat_mask"], [1])

    def test_bad_mins_rejected(self):
        with self.assertRaises(ValueError):
            hldsmon.incident_bundle(self.records, self.drop, mins=-1)
        with self.assertRaises(ValueError):
            hldsmon.incident_bundle(self.records, self.drop, mins="soon")

    def test_bad_drop_rejected(self):
        with self.assertRaises(ValueError):
            hldsmon.incident_bundle(self.records, "drop")
        with self.assertRaises(ValueError):
            hldsmon.incident_bundle(self.records, {"type": "drop"})

    def test_deterministic(self):
        other = hldsmon.incident_bundle(self.records, self.drop, mins=0.05)
        self.assertEqual(json.dumps(other, sort_keys=True),
                         json.dumps(self.bundle, sort_keys=True))


class TestBundleReader(unittest.TestCase):
    def setUp(self):
        self.records = _bundle_records()

    def test_reingest_round_trip(self):
        bundle = hldsmon.incident_bundle(self.records, None, mins=2.0)
        expected = bundle["records"]
        got = hldsmon.read_bundle(bundle)
        self.assertEqual(got, expected)
        path = write_jsonl(got)
        try:
            self.assertEqual(hldsmon.parse_records([path]), expected)
        finally:
            os.remove(path)

    def test_reader_from_file(self):
        import tempfile
        bundle = hldsmon.incident_bundle(self.records, None)
        fd, path = tempfile.mkstemp(suffix=".json")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(bundle, fh)
            self.assertEqual(hldsmon.read_bundle(path), bundle["records"])
        finally:
            os.remove(path)

    def test_reader_deduplicated_bundle_yields_the_drop(self):
        bundle = hldsmon.incident_bundle(self.records, self.records[3], mins=0.05)
        self.assertEqual(hldsmon.read_bundle(bundle), [self.records[3]])

    def test_reader_skips_unknown_types_in_records(self):
        bundle = {"bundle_version": 1,
                  "records": [{"type": "bogus", "t": 1.0},
                              {"type": "server", "t": 2.0},
                              "not a record", None]}
        self.assertEqual([r["type"] for r in hldsmon.read_bundle(bundle)],
                         ["server"])

    def test_reader_sorts_by_time_with_untimed_last(self):
        bundle = {"bundle_version": 1,
                  "drop": {"type": "drop", "t": 5.0},
                  "records": [{"type": "server", "t": 9.0},
                              {"type": "server", "t": 1.0}]}
        self.assertEqual([r["t"] for r in hldsmon.read_bundle(bundle)],
                         [1.0, 5.0, 9.0])

    def test_reader_rejects_non_bundle(self):
        for bad in ({"records": []}, [], {}, 7, None):
            with self.assertRaises(ValueError):
                hldsmon.read_bundle(bad)
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".json")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write("{not json")
            with self.assertRaises(ValueError):
                hldsmon.read_bundle(path)
        finally:
            os.remove(path)


class TestBundleSizeCap(unittest.TestCase):
    def _records(self):
        records = [
            {"type": "client", "t": 1000.0 + i * 0.1, "id": i % 5,
             "ping": 40 + i, "loss": i % 3}
            for i in range(400)
        ]
        records.append({"type": "drop", "t": 1002.0, "id": 7,
                        "reason": "Timeout", "category": "timeout"})
        return records

    def test_records_dropped_to_fit(self):
        records = self._records()
        drop = records[-1]
        base = hldsmon.incident_bundle(records, drop, mins=0.05,
                                       include_records=False)
        cap = len(json.dumps(base, ensure_ascii=False).encode("utf-8")) + 400
        bundle = hldsmon.incident_bundle(records, drop, mins=0.05,
                                         include_records=True, max_bytes=cap)
        self.assertEqual(bundle["records"], [])
        self.assertLessEqual(
            len(json.dumps(bundle, ensure_ascii=False).encode("utf-8")), cap)
        self.assertTrue(any("raw window records dropped" in w
                            for w in bundle["warnings"]))
        self.assertFalse(any("trimmed" in w for w in bundle["warnings"]))

    def test_drop_window_trimmed_to_fit(self):
        drop = {"type": "drop", "t": 1002.0, "id": 7, "reason": "Timeout",
                "category": "timeout",
                "window": [{"t": 1000.0 + i * 0.01, "ping": 40 + i,
                            "loss": i % 4} for i in range(40)]}
        records = [drop] + [
            {"type": "client", "t": 1000.0 + i * 0.01, "id": i % 5,
             "ping": 40 + i} for i in range(40)]
        cap = 1500
        bundle = hldsmon.incident_bundle(records, drop, mins=0.05,
                                         max_bytes=cap, include_records=True)
        self.assertLessEqual(
            len(json.dumps(bundle, ensure_ascii=False).encode("utf-8")), cap)
        self.assertLess(len(bundle["drop"]["window"]), 40)
        self.assertTrue(any("trimmed" in w for w in bundle["warnings"]))
        self.assertEqual(len(drop["window"]), 40)  # input untouched

    def test_impossible_cap_warns_and_keeps_the_drop(self):
        records = self._records()
        drop = records[-1]
        bundle = hldsmon.incident_bundle(records, drop, mins=0.05,
                                         max_bytes=10)
        self.assertEqual(bundle["records"], [])
        self.assertIs(bundle["drop"], drop)
        self.assertTrue(any("exceeds" in w for w in bundle["warnings"]))


if __name__ == "__main__":
    unittest.main()
