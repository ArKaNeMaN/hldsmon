// HldsMonitoring — host test for the no-allocation JSONL writers (plan Phase 1).

#include "serialization.h"
#include "host_metrics.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

static bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

static int count_occurrences(const char* hay, const char* needle) {
    int n = 0;
    const char* p = hay;
    while ((p = std::strstr(p, needle)) != nullptr) {
        ++n;
        ++p;
    }
    return n;
}

static ClientSample make_client(uint8_t id, double t) {
    ClientSample c;
    std::memset(&c, 0, sizeof c);
    c.t = t;
    c.id = id;
    c.gen = 1712000000u;
    c.ping = 42;
    c.loss = 3;
    c.dloss = 1;
    c.choke = 2;
    c.dchoke = 2;
    c.delta_seq = 0;
    c.in_seq = 100234;
    c.in_ack = 100200;
    c.out_seq = 99980;
    c.rel_seq = 5;
    c.rel_fill = 3;
    c.rate = 20000;
    c.cleartime = 1.5f;
    c.updaterate = 101;
    c.cmdrate = 101;
    c.state = 7;
    c.fragbuf_total = 2;
    c.incomingready_mask = 3;
    c.flow_in_kbps = 12.3f;
    c.flow_out_kbps = 18.1f;
    c.flow_in_avg = 12.0f;
    c.flow_out_avg = 18.0f;
    c.msg_flags = 1;
    c.idle_ms = 1234;
    c.jit_min_ms = 4;
    c.jit_avg_ms = 11;
    c.jit_max_ms = 25;
    return c;
}

static ServerSample make_server() {
    ServerSample s;
    std::memset(&s, 0, sizeof s);
    s.t = 3.0;
    s.frame_ms_min = 3.1f;
    s.frame_ms_avg = 4.0f;
    s.frame_ms_max = 61.2f;
    s.hitch_gt50 = 2;
    s.hitch_gt100 = 0;
    s.cpu_pct = 41.2f;
    s.rss_kb = 120000;
    s.minflt = 10;
    s.majflt = 1;
    s.players = 17;
    s.real = 16;
    s.proxy = 0;
    s.fake = 1;
    s.in_kbps = 812.4f;
    s.out_kbps = 1400.1f;
    s.edicts = 245;
    s.edicts_churn = -3;
    s.edicts_churn_present = 1;
    std::strcpy(s.map, "de_dust2");
    s.uptime_s = 84213;
    s.fps = 100.0f;
    s.maxclients = 32;
    return s;
}

static ConfigSnapshot make_config_snapshot(uint16_t present) {
    ConfigSnapshot c;
    std::memset(&c, 0, sizeof c);
    c.present = present;
    c.sys_ticrate = 100.0f;
    c.host_framerate = 0.0f;
    c.sv_timeout = 65.0f;
    c.sv_maxrate = 0.0f;
    c.sv_minrate = 0.0f;
    c.sv_maxupdaterate = 101.0f;
    c.sv_minupdaterate = 10.0f;
    c.sv_maxcmdrate = 101.0f;
    c.sv_mincmdrate = 10.0f;
    return c;
}

static ConfigRecord make_config(uint16_t present) {
    ConfigRecord r;
    std::memset(&r, 0, sizeof r);
    r.t = 1755000000.0;
    r.rehlds_major = 3;
    r.rehlds_minor = 14;
    std::strcpy(r.map, "de_dust2");
    r.maxclients = 32;
    r.config = make_config_snapshot(present);
    return r;
}

int main() {
    char buf[8192];

    // ---------------- client ----------------
    {
        const ClientSample c = make_client(7, 2.0);
        const size_t n = write_client_json(buf, c);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(buf[0] == '{');
        CHECK(buf[n - 1] == '}');
        CHECK(has(buf, "\"type\":\"client\""));
        CHECK(has(buf, "\"t\":2.000"));
        CHECK(has(buf, "\"id\":7"));
        CHECK(has(buf, "\"gen\":1712000000"));
        CHECK(has(buf, "\"ping\":42"));
        CHECK(has(buf, "\"loss\":3"));
        CHECK(has(buf, "\"dloss\":1"));
        CHECK(has(buf, "\"choke\":2"));
        CHECK(has(buf, "\"dchoke\":2"));
        CHECK(has(buf, "\"delta_seq\":0"));
        CHECK(has(buf, "\"in_seq\":100234"));
        CHECK(has(buf, "\"in_ack\":100200"));
        CHECK(has(buf, "\"out_seq\":99980"));
        CHECK(has(buf, "\"rel_seq\":5"));
        CHECK(has(buf, "\"rel_fill\":3"));
        CHECK(has(buf, "\"rate\":20000"));
        CHECK(has(buf, "\"cleartime\":1.500"));
        CHECK(has(buf, "\"updaterate\":101"));
        CHECK(has(buf, "\"cmdrate\":101"));
        CHECK(has(buf, "\"state\":7"));
        CHECK(has(buf, "\"fragbuf_total\":2"));
        CHECK(has(buf, "\"incomingready_mask\":3"));
        CHECK(has(buf, "\"flow_in_kbps\":12.300"));
        CHECK(has(buf, "\"flow_out_kbps\":18.100"));
        CHECK(has(buf, "\"flow_in_avg\":12.000"));
        CHECK(has(buf, "\"flow_out_avg\":18.000"));
        CHECK(has(buf, "\"msg_flags\":1"));
        CHECK(has(buf, "\"jit_min_ms\":4"));
        CHECK(has(buf, "\"jit_avg_ms\":11"));
        CHECK(has(buf, "\"jit_max_ms\":25"));
        CHECK(has(buf, "\"msg_flags\":1,\"idle_ms\":1234,\"jit_min_ms\":4,"
                       "\"jit_avg_ms\":11,\"jit_max_ms\":25,\"name\":\"\""));
    }

    // ---------------- server ----------------
    {
        const ServerSample s = make_server();
        const size_t n = write_server_json(buf, s);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(has(buf, "\"type\":\"server\""));
        CHECK(has(buf, "\"t\":3.000"));
        CHECK(has(buf, "\"frame_ms\":{\"min\":3.100,\"avg\":4.000,\"max\":61.200}"));
        CHECK(has(buf, "\"hitch\":{\"gt50\":2,\"gt100\":0}"));
        CHECK(has(buf, "\"cpu_pct\":41.200"));
        CHECK(has(buf, "\"rss_kb\":120000"));
        CHECK(has(buf, "\"minflt\":10"));
        CHECK(has(buf, "\"majflt\":1"));
        CHECK(has(buf, "\"players\":17"));
        CHECK(has(buf, "\"real\":16"));
        CHECK(has(buf, "\"proxy\":0"));
        CHECK(has(buf, "\"fake\":1"));
        CHECK(has(buf, "\"in_kbps\":812.400"));
        CHECK(has(buf, "\"out_kbps\":1400.100"));
        CHECK(has(buf, "\"edicts\":245"));
        CHECK(has(buf, "\"edicts\":245,\"edicts_churn\":-3,\"map\":\"de_dust2\""));
        CHECK(has(buf, "\"map\":\"de_dust2\""));
        CHECK(has(buf, "\"uptime_s\":84213"));
        CHECK(has(buf, "\"fps\":100.000"));
        CHECK(has(buf, "\"maxclients\":32"));
    }

    // ---------------- config (all fields present) ----------------
    {
        const ConfigRecord r = make_config(0x1FFu);
        const size_t n = write_config_json(buf, r);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(buf[0] == '{');
        CHECK(buf[n - 1] == '}');
        CHECK(has(buf, "\"type\":\"config\""));
        CHECK(has(buf, "\"t\":1755000000.000"));
        CHECK(has(buf, "\"rehlds_major\":3"));
        CHECK(has(buf, "\"rehlds_minor\":14"));
        CHECK(has(buf, "\"map\":\"de_dust2\""));
        CHECK(has(buf, "\"maxclients\":32"));
        CHECK(has(buf, "\"sys_ticrate\":100.000"));
        CHECK(has(buf, "\"host_framerate\":0.000"));
        CHECK(has(buf, "\"sv_timeout\":65.000"));
        CHECK(has(buf, "\"sv_maxrate\":0.000"));
        CHECK(has(buf, "\"sv_minrate\":0.000"));
        CHECK(has(buf, "\"sv_maxupdaterate\":101.000"));
        CHECK(has(buf, "\"sv_minupdaterate\":10.000"));
        CHECK(has(buf, "\"sv_maxcmdrate\":101.000"));
        CHECK(has(buf, "\"sv_mincmdrate\":10.000"));
        CHECK(!has(buf, "null"));
        CHECK(has(buf, "\"maxclients\":32,\"sys_ticrate\":100.000,\"host_framerate\":0.000"));
    }

    // ---------------- config (absent cvars => literal null) ----------------
    {
        const ConfigRecord r = make_config(std::to_underlying(ConfigPresent::kCfgSvTimeout | ConfigPresent::kCfgSvMaxrate));
        const size_t n = write_config_json(buf, r);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(has(buf, "\"sys_ticrate\":null"));
        CHECK(has(buf, "\"host_framerate\":null"));
        CHECK(has(buf, "\"sv_timeout\":65.000"));
        CHECK(has(buf, "\"sv_maxrate\":0.000"));
        CHECK(has(buf, "\"sv_minrate\":null"));
        CHECK(has(buf, "\"sv_maxupdaterate\":null"));
        CHECK(has(buf, "\"sv_minupdaterate\":null"));
        CHECK(has(buf, "\"sv_maxcmdrate\":null"));
        CHECK(has(buf, "\"sv_mincmdrate\":null"));
    }

    // ---------------- host (all fields present) ----------------
    {
        HostRecord r;
        std::memset(&r, 0, sizeof r);
        r.t = 1755000000.0;
        r.udp_rcvbuf_errors = 1;
        r.udp_in_errors = 2;
        r.udp_in_csum_errors = 3;
        r.net_rx_dropped = 4;
        r.net_rx_missed = 5;
        r.net_rx_overruns = 6;
        r.steal_pct = 1.5f;
        r.io_read_bytes = 7;
        r.io_write_bytes = 8;
        r.disk_bavail_mb = 12345;
        r.present_mask = 0x03FFu;
        r.caveat_mask = std::to_underlying(HostCaveat::kCaveatUdpNetns | HostCaveat::kCaveatNetDev);
        const size_t n = write_host_json(buf, r);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(buf[0] == '{');
        CHECK(buf[n - 1] == '}');
        CHECK(has(buf, "\"type\":\"host\""));
        CHECK(has(buf, "\"t\":1755000000.000"));
        CHECK(has(buf, "\"udp_rcvbuf_errors\":1,\"udp_in_errors\":2,"
                       "\"udp_in_csum_errors\":3,\"net_rx_dropped\":4,"
                       "\"net_rx_missed\":5,\"net_rx_overruns\":6,"
                       "\"steal_pct\":1.500,\"io_read_bytes\":7,"
                       "\"io_write_bytes\":8,\"disk_bavail_mb\":12345,"
                       "\"present_mask\":1023,\"caveat_mask\":3"));
        CHECK(!has(buf, "null"));
    }

    // ---------------- host (first sample / unavailable => literal null) ----
    {
        HostRecord r;
        std::memset(&r, 0, sizeof r);
        r.t = 1.0;
        r.disk_bavail_mb = 12345;
        r.present_mask = std::to_underlying(HostPresent::kHostDiskBavailMb);  // only the instantaneous field
        r.caveat_mask = std::to_underlying(HostCaveat::kCaveatUdpNetns);     // caveated != null (M4)
        const size_t n = write_host_json(buf, r);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(has(buf, "\"udp_rcvbuf_errors\":null"));
        CHECK(has(buf, "\"udp_in_errors\":null"));
        CHECK(has(buf, "\"udp_in_csum_errors\":null"));
        CHECK(has(buf, "\"net_rx_dropped\":null"));
        CHECK(has(buf, "\"net_rx_missed\":null"));
        CHECK(has(buf, "\"net_rx_overruns\":null"));
        CHECK(has(buf, "\"steal_pct\":null"));
        CHECK(has(buf, "\"io_read_bytes\":null"));
        CHECK(has(buf, "\"io_write_bytes\":null"));
        CHECK(has(buf, "\"io_write_bytes\":null,\"disk_bavail_mb\":12345,"
                       "\"present_mask\":512,\"caveat_mask\":1"));
    }

    // ---------------- host (caveated value stays present) ----------------
    {
        HostRecord r;
        std::memset(&r, 0, sizeof r);
        r.t = 1.0;
        r.udp_rcvbuf_errors = 7;
        r.present_mask = std::to_underlying(HostPresent::kHostUdpRcvbufErrors);
        r.caveat_mask = std::to_underlying(HostCaveat::kCaveatUdpNetns);
        const size_t n = write_host_json(buf, r);
        CHECK(n > 0);
        CHECK(has(buf, "\"udp_rcvbuf_errors\":7,\"udp_in_errors\":null"));
        CHECK(has(buf, "\"present_mask\":1,\"caveat_mask\":1"));
    }

    // ---------------- drop (with window) ----------------
    {
        DropRecord d;
        std::memset(&d, 0, sizeof d);
        d.t = 1712345679.0;
        d.id = 7;
        d.gen = 1712000000u;
        std::strcpy(d.reason, "Reliable channel overflowed");
        std::strcpy(d.category, "overflow");
        d.crash = 0;
        std::strcpy(d.observed_via, "sv_dropclient");
        std::strcpy(d.detector, "movecmd");
        d.client.active = 1;
        d.client.spawned = 1;
        d.client.fully_connected = 1;
        d.client.connected = 1;
        d.client.proxy = 0;
        d.client.fake = 0;
        d.client.userid = 7;
        d.client.connecttime = 100.0f;
        d.client.cmdtime = 200.0f;
        d.client.connection_started = 50.0f;
        d.netchan.in_seq = 12345;
        d.netchan.in_ack = 12300;
        d.netchan.in_rel_ack = 1;
        d.netchan.in_rel_seq = 2;
        d.netchan.out_seq = 12000;
        d.netchan.rel_seq = 3;
        d.netchan.last_rel_seq = 3;
        d.netchan.rel_len = 128;
        d.netchan.rel_fill = 4;
        d.netchan.rate = 20000;
        d.netchan.cleartime = 1.5f;
        d.netchan.msg_flags = 1;
        d.netchan.last_received = 1000.0f;
        d.netchan.connect_time = 900.0f;
        d.netchan.sv_timeout = 65.0f;
        d.config = make_config_snapshot(0x1FFu);
        d.window_count = 3;
        for (uint8_t i = 0; i < 3; ++i) {
            d.window[i] = make_client(i, static_cast<double>(i));
        }
        d.server = make_server();
        d.neighbors.count = 7;
        d.neighbors.avg_loss = 1.5f;
        d.neighbors.avg_ping = 45.0f;
        d.neighbors.max_loss = 4.0f;

        const size_t n = write_drop_json(buf, d);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(buf[0] == '{');
        CHECK(buf[n - 1] == '}');
        CHECK(has(buf, "\"type\":\"drop\""));
        CHECK(has(buf, "\"id\":7"));
        CHECK(has(buf, "\"gen\":1712000000"));
        CHECK(has(buf, "\"reason\":\"Reliable channel overflowed\""));
        CHECK(has(buf, "\"category\":\"overflow\""));
        CHECK(has(buf, "\"crash\":false"));
        CHECK(has(buf, "\"observed_via\":\"sv_dropclient\""));
        CHECK(has(buf, "\"observed_via\":\"sv_dropclient\",\"detector\":\"movecmd\""));
        CHECK(has(buf, "\"client\":{"));
        CHECK(has(buf, "\"netchan\":{"));
        CHECK(has(buf, "\"config\":{\"sys_ticrate\":100.000,\"host_framerate\":0.000,\"sv_timeout\":65.000,\"sv_maxrate\":0.000,\"sv_minrate\":0.000,\"sv_maxupdaterate\":101.000,\"sv_minupdaterate\":10.000,\"sv_maxcmdrate\":101.000,\"sv_mincmdrate\":10.000}"));
        CHECK(has(buf, "\"window\":["));
        CHECK(count_occurrences(buf, "\"type\":\"client\"") == 3);
        CHECK(count_occurrences(buf, "\"sv_timeout\"") == 2);   // netchan (P1) + config
        CHECK(has(buf, "\"server\":{"));
        CHECK(has(buf, "\"neighbors\":{\"count\":7,\"avg_loss\":1.500,\"avg_ping\":45.000,\"max_loss\":4.000}"));
    }

    // ---------------- reject ----------------
    {
        RejectRecord r;
        std::memset(&r, 0, sizeof r);
        r.t = 0.0;
        std::strcpy(r.ip, "1.2.3.4");
        std::strcpy(r.steamid, "STEAM_0:0:1");
        std::strcpy(r.stage, "SV_CheckUserInfo:password");
        std::strcpy(r.detail, "bad password");
        const size_t n = write_reject_json(buf, r);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"reject\""));
        CHECK(has(buf, "\"ip\":\"1.2.3.4\""));
        CHECK(has(buf, "\"steamid\":\"STEAM_0:0:1\""));
        CHECK(has(buf, "\"stage\":\"SV_CheckUserInfo:password\""));
        CHECK(has(buf, "\"detail\":\"bad password\""));
    }

    // ---------------- connect ----------------
    {
        ConnectRecord c;
        std::memset(&c, 0, sizeof c);
        c.id = 5;
        std::strcpy(c.ip, "10.0.0.9");
        std::strcpy(c.steamid, "STEAM_0:1:42");
        std::strcpy(c.name, "Bob");
        const size_t n = write_connect_json(buf, c);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"connect\""));
        CHECK(has(buf, "\"id\":5"));
        CHECK(has(buf, "\"ip\":\"10.0.0.9\""));
        CHECK(has(buf, "\"steamid\":\"STEAM_0:1:42\""));
        CHECK(has(buf, "\"name\":\"Bob\""));
    }

    // ---------------- netcmd ----------------
    {
        NetCmdRecord r;
        std::memset(&r, 0, sizeof r);
        r.id = 1;
        r.cmd = 8;
        r.count = 42;
        const size_t n = write_netcmd_json(buf, r);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"netcmd\""));
        CHECK(has(buf, "\"id\":1"));
        CHECK(has(buf, "\"cmd\":8"));
        CHECK(has(buf, "\"count\":42"));
    }

    // ---------------- massdrop ----------------
    {
        MassDropRecord r;
        std::memset(&r, 0, sizeof r);
        r.window_s = 10;
        r.count = 5;
        std::strcpy(r.reason, "Timed out");
        std::strcpy(r.category, "timeout");
        const size_t n = write_massdrop_json(buf, r);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"massdrop\""));
        CHECK(has(buf, "\"window_s\":10"));
        CHECK(has(buf, "\"count\":5"));
        CHECK(has(buf, "\"reason\":\"Timed out\""));
        CHECK(has(buf, "\"category\":\"timeout\""));
    }

    // ---------------- packet ----------------
    {
        PacketStats p;
        std::memset(&p, 0, sizeof p);
        p.pps = 1000;
        p.bytes = 123456789u;
        p.sizes.lt128 = 10;
        p.sizes.lt512 = 20;
        p.sizes.lt1024 = 30;
        p.sizes.ge1024 = 40;
        p.unique_ips = 50;
        std::strcpy(p.top_ip, "9.9.9.9");
        std::strcpy(p.top_ips[0].ip, "9.9.9.9");
        p.top_ips[0].count = 30;
        std::strcpy(p.top_ips[1].ip, "8.8.8.8");
        p.top_ips[1].count = 20;
        std::strcpy(p.top_ips[2].ip, "7.7.7.7");
        p.top_ips[2].count = 10;
        const size_t n = write_packet_json(buf, p);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"packet\""));
        CHECK(has(buf, "\"pps\":1000"));
        CHECK(has(buf, "\"bytes\":123456789"));
        CHECK(has(buf, "\"sizes\":{\"lt128\":10,\"lt512\":20,\"lt1024\":30,\"ge1024\":40}"));
        CHECK(has(buf, "\"unique_ips\":50"));
        CHECK(has(buf, "\"top_ip\":\"9.9.9.9\""));
        CHECK(has(buf, "\"top_ips\":[{\"ip\":\"9.9.9.9\",\"count\":30},"
                       "{\"ip\":\"8.8.8.8\",\"count\":20},"
                       "{\"ip\":\"7.7.7.7\",\"count\":10}]"));
    }

    // ---------------- hitch ----------------
    {
        HitchRecord h;
        std::memset(&h, 0, sizeof h);
        h.duration_ms = 120;
        const size_t n = write_hitch_json(buf, h);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"hitch\""));
        CHECK(has(buf, "\"duration_ms\":120"));
    }

    // ---------------- mapchange ----------------
    {
        MapChangeRecord m;
        std::memset(&m, 0, sizeof m);
        std::strcpy(m.map, "de_nuke");
        const size_t n = write_mapchange_json(buf, m);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"mapchange\""));
        CHECK(has(buf, "\"map\":\"de_nuke\""));
    }

    // ---------------- voice ----------------
    {
        VoiceRecord v;
        std::memset(&v, 0, sizeof v);
        v.id = 3;
        v.last_voice_time = 12.5f;
        const size_t n = write_voice_json(buf, v);
        CHECK(n > 0);
        CHECK(has(buf, "\"type\":\"voice\""));
        CHECK(has(buf, "\"id\":3"));
        CHECK(has(buf, "\"last_voice_time\":12.500"));
    }

    // ---------------- json_escape helper ----------------
    {
        char esc[64];
        const char* raw = "a\"b\\c\n\t\x01";
        const size_t m = json_escape(raw, esc);
        CHECK(m == 17u);
        CHECK(std::strcmp(esc, "a\\\"b\\\\c\\n\\t\\u0001") == 0);
        // escape buffer too small => 0 and untouched
        char tiny[4];
        std::memset(tiny, 0x7f, sizeof tiny);
        CHECK(json_escape("abcd", tiny) == 0);
        CHECK(static_cast<unsigned char>(tiny[0]) == 0x7f);
    }

    // ---------------- escaping inside a record ----------------
    {
        DropRecord d;
        std::memset(&d, 0, sizeof d);
        d.t = 1.0;
        d.id = 1;
        std::strcpy(d.reason, "quote \" backslash \\ nl\n ctrl\x01");
        d.window_count = 0;
        const size_t n = write_drop_json(buf, d);
        CHECK(n > 0);
        CHECK(has(buf, "quote \\\" backslash \\\\ nl\\n ctrl\\u0001"));
    }

    // ---------------- capacity handling: no partial line ----------------
    {
        DropRecord d;
        std::memset(&d, 0, sizeof d);
        d.t = 1.0;
        d.id = 1;
        std::strcpy(d.reason, "x");
        d.window_count = 2;
        for (uint8_t i = 0; i < 2; ++i) d.window[i] = make_client(i, 1.0);

        char big[8192];
        const size_t need = write_drop_json(big, d);
        CHECK(need > 0);

        // cap == need is one byte short (no room for NUL): reject, write nothing.
        char sink[8192];
        std::memset(sink, 0x7f, sizeof sink);
        CHECK(write_drop_json(std::span<char>(sink, need), d) == 0);
        bool touched = false;
        for (size_t i = 0; i < sizeof sink; ++i) {
            if (static_cast<unsigned char>(sink[i]) != 0x7f) {
                touched = true;
                break;
            }
        }
        CHECK(!touched);

        // cap == 0: reject, write nothing.
        char zero[8];
        std::memset(zero, 0x7f, sizeof zero);
        CHECK(write_drop_json(std::span<char>(zero, 0), d) == 0);

        // exact fit (need + NUL) succeeds.
        char exact[8192];
        const size_t got = write_drop_json(std::span<char>(exact, need + 1), d);
        CHECK(got == need);
        CHECK(exact[need] == '\0');

        // client writer also rejects a short buffer.
        const ClientSample c = make_client(1, 1.0);
        char cbig[1024];
        const size_t cneed = write_client_json(cbig, c);
        CHECK(cneed > 0);
        char csink[1024];
        std::memset(csink, 0x7f, sizeof csink);
        CHECK(write_client_json(std::span<char>(csink, cneed), c) == 0);
        CHECK(static_cast<unsigned char>(csink[0]) == 0x7f);

        // host writer rejects a short buffer too.
        HostRecord h;
        std::memset(&h, 0, sizeof h);
        h.present_mask = 0x03FFu;
        char hbig[1024];
        const size_t hneed = write_host_json(hbig, h);
        CHECK(hneed > 0);
        char hsink[1024];
        std::memset(hsink, 0x7f, sizeof hsink);
        CHECK(write_host_json(std::span<char>(hsink, hneed), h) == 0);
        CHECK(static_cast<unsigned char>(hsink[0]) == 0x7f);
    }

    if (g_fail == 0) std::printf("test_serialization: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
