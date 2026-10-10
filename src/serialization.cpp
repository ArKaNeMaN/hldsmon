// HldsMonitoring — no-allocation JSONL serialization (plan Phase 1, INV-2).
//
// Strategy: every writer runs the emitter twice over a tiny sink (`JsonBuf`).
// Pass 1 measures the exact byte length with a null sink and an "infinite"
// capacity; if the caller's buffer cannot hold the line plus its NUL, the
// function returns 0 WITHOUT touching `out` (guaranteeing no partial line).
// Pass 2 writes the same bytes. No heap, no std::string/std::vector, no
// exceptions/RTTI, no snprintf — integers are rendered by repeated division and
// floats by integer/fraction decomposition (see append_fixed).

#include "serialization.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace hldsmon {
namespace {

// Append-only byte sink. `out == nullptr` means measure-only; in that mode cap
// is SIZE_MAX and `overflow` never trips. On overflow the sink stops advancing.
struct JsonBuf {
    char*  out;
    size_t cap;
    size_t pos;
    bool   overflow;

    void raw(const char* s, size_t n) {
        if (overflow) return;
        if (pos + n > cap) { overflow = true; return; }
        if (out) std::memcpy(out + pos, s, n);
        pos += n;
    }
    void ch(char c) {
        if (overflow) return;
        if (pos + 1 > cap) { overflow = true; return; }
        if (out) out[pos] = c;
        ++pos;
    }
    void lit(const char* s) { raw(s, std::strlen(s)); }
};

// ---- primitive appenders -------------------------------------------------

const char kHexDigits[] = "0123456789abcdef";

void append_hex_escape(JsonBuf& b, unsigned char c) {
    char buf[6];
    buf[0] = '\\';
    buf[1] = 'u';
    buf[2] = '0';
    buf[3] = '0';
    buf[4] = kHexDigits[(c >> 4) & 0x0Fu];
    buf[5] = kHexDigits[c & 0x0Fu];
    b.raw(buf, sizeof(buf));
}

void append_u64(JsonBuf& b, uint64_t v) {
    char tmp[20];
    int i = 0;
    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v != 0) {
            tmp[i++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (i > 0) b.ch(tmp[--i]);
}

void append_i64(JsonBuf& b, int64_t v) {
    if (v < 0) {
        b.ch('-');
        append_u64(b, static_cast<uint64_t>(-(v + 1)) + 1u);
    } else {
        append_u64(b, static_cast<uint64_t>(v));
    }
}

// Fixed-precision decimal for finite doubles. Domain-bounded by all callers
// (epoch seconds, ms, percentages, KB/s) so the integer part fits in uint64.
void append_fixed(JsonBuf& b, double v, int prec) {
    if (!std::isfinite(v)) {
        append_u64(b, 0);
        return;
    }
    if (prec < 0) prec = 0;
    if (prec > 9) prec = 9;
    static const uint64_t kPow10[10] = {1u,      10u,      100u,      1000u,
                                        10000u,  100000u,  1000000u,  10000000u,
                                        100000000u, 1000000000u};
    const uint64_t scale = kPow10[prec];
    const bool neg = (v < 0.0);
    if (neg) v = -v;

    double ipart_d = 0.0;
    const double frac = std::modf(v, &ipart_d);
    if (ipart_d >= 9.0e18) { // unreachable for schema fields; avoid UB
        append_u64(b, 0);
        return;
    }
    uint64_t ip = static_cast<uint64_t>(ipart_d);
    uint64_t frac_int = static_cast<uint64_t>(frac * static_cast<double>(scale) + 0.5);
    if (frac_int >= scale) {
        frac_int -= scale;
        ip += 1u;
    }
    if (neg && (ip != 0u || frac_int != 0u)) b.ch('-');
    append_u64(b, ip);
    if (prec > 0) {
        b.ch('.');
        uint64_t p = scale / 10u;
        for (int k = 0; k < prec; ++k) {
            const int d = static_cast<int>((frac_int / p) % 10u);
            b.ch(static_cast<char>('0' + d));
            p /= 10u;
            if (p == 0u) p = 1u;
        }
    }
}

// Quoted, escaped JSON string value.
void append_str(JsonBuf& b, const char* s) {
    b.ch('"');
    if (s) {
        for (const char* p = s; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
                case '"':  b.raw("\\\"", 2); break;
                case '\\': b.raw("\\\\", 2); break;
                case '\b': b.raw("\\b", 2); break;
                case '\f': b.raw("\\f", 2); break;
                case '\n': b.raw("\\n", 2); break;
                case '\r': b.raw("\\r", 2); break;
                case '\t': b.raw("\\t", 2); break;
                default:
                    if (c < 0x20) {
                        append_hex_escape(b, c);
                    } else {
                        b.ch(static_cast<char>(c));
                    }
            }
        }
    }
    b.ch('"');
}

void kv_key(JsonBuf& b, const char* k) {
    b.ch('"');
    b.lit(k);
    b.raw("\":", 2);
}
void kv_u64(JsonBuf& b, const char* k, uint64_t v) {
    kv_key(b, k);
    append_u64(b, v);
}
void kv_i64(JsonBuf& b, const char* k, int64_t v) {
    kv_key(b, k);
    append_i64(b, v);
}
void kv_fixed(JsonBuf& b, const char* k, double v, int prec) {
    kv_key(b, k);
    append_fixed(b, v, prec);
}
void kv_str(JsonBuf& b, const char* k, const char* s) {
    kv_key(b, k);
    append_str(b, s);
}
void kv_bool(JsonBuf& b, const char* k, bool v) {
    kv_key(b, k);
    b.lit(v ? "true" : "false");
}

// ---- per-record emitters -------------------------------------------------

// Identity block shared by `client` and `drop` records (P1). Emits a leading
// comma so callers can append it after their last value key.
template <class Rec>
void emit_identity(JsonBuf& b, const Rec& r) {
    b.ch(',');
    kv_str(b, "name", r.name);
    b.ch(',');
    kv_str(b, "authid", r.authid);
    b.ch(',');
    kv_str(b, "steamid64", r.steamid64);
    b.ch(',');
    kv_str(b, "ip", r.ip);
    b.ch(',');
    kv_i64(b, "port", r.port);
    b.ch(',');
    kv_i64(b, "userid", r.userid);
    b.ch(',');
    kv_i64(b, "slot", r.slot);
    b.ch(',');
    kv_fixed(b, "conn_s", r.conn_s, 3);
    b.ch(',');
    kv_bool(b, "fake", r.fake);
    b.ch(',');
    kv_bool(b, "proxy", r.proxy);
}

void emit_client(JsonBuf& b, const ClientSample& r) {
    b.ch('{');
    kv_str(b, "type", "client");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "id", r.id);
    b.ch(',');
    kv_u64(b, "gen", r.gen);
    b.ch(',');
    kv_u64(b, "ping", r.ping);
    b.ch(',');
    kv_u64(b, "loss", r.loss);
    b.ch(',');
    kv_u64(b, "dloss", r.dloss);
    b.ch(',');
    kv_u64(b, "choke", r.choke);
    b.ch(',');
    kv_u64(b, "dchoke", r.dchoke);
    b.ch(',');
    kv_i64(b, "delta_seq", r.delta_seq);
    b.ch(',');
    kv_u64(b, "in_seq", r.in_seq);
    b.ch(',');
    kv_u64(b, "in_ack", r.in_ack);
    b.ch(',');
    kv_u64(b, "out_seq", r.out_seq);
    b.ch(',');
    kv_u64(b, "rel_seq", r.rel_seq);
    b.ch(',');
    kv_u64(b, "rel_fill", r.rel_fill);
    b.ch(',');
    kv_u64(b, "rate", r.rate);
    b.ch(',');
    kv_fixed(b, "cleartime", r.cleartime, 3);
    b.ch(',');
    kv_u64(b, "updaterate", r.updaterate);
    b.ch(',');
    kv_u64(b, "cmdrate", r.cmdrate);
    b.ch(',');
    kv_u64(b, "state", r.state);
    b.ch(',');
    kv_u64(b, "fragbuf_total", r.fragbuf_total);
    b.ch(',');
    kv_u64(b, "incomingready_mask", r.incomingready_mask);
    b.ch(',');
    kv_fixed(b, "flow_in_kbps", r.flow_in_kbps, 3);
    b.ch(',');
    kv_fixed(b, "flow_out_kbps", r.flow_out_kbps, 3);
    b.ch(',');
    kv_fixed(b, "flow_in_avg", r.flow_in_avg, 3);
    b.ch(',');
    kv_fixed(b, "flow_out_avg", r.flow_out_avg, 3);
    b.ch(',');
    kv_u64(b, "msg_flags", r.msg_flags);
    b.ch(',');
    kv_u64(b, "idle_ms", r.idle_ms);
    emit_identity(b, r);
    b.ch('}');
}

void emit_client_snapshot(JsonBuf& b, const ClientSnapshot& r) {
    b.ch('{');
    kv_u64(b, "active", r.active);
    b.ch(',');
    kv_u64(b, "spawned", r.spawned);
    b.ch(',');
    kv_u64(b, "fully_connected", r.fully_connected);
    b.ch(',');
    kv_u64(b, "connected", r.connected);
    b.ch(',');
    kv_u64(b, "proxy", r.proxy);
    b.ch(',');
    kv_u64(b, "fake", r.fake);
    b.ch(',');
    kv_u64(b, "userid", r.userid);
    b.ch(',');
    kv_fixed(b, "connecttime", r.connecttime, 3);
    b.ch(',');
    kv_fixed(b, "cmdtime", r.cmdtime, 3);
    b.ch(',');
    kv_fixed(b, "connection_started", r.connection_started, 3);
    b.ch('}');
}

void emit_netchan(JsonBuf& b, const NetchanSnapshot& r) {
    b.ch('{');
    kv_u64(b, "in_seq", r.in_seq);
    b.ch(',');
    kv_u64(b, "in_ack", r.in_ack);
    b.ch(',');
    kv_u64(b, "in_rel_ack", r.in_rel_ack);
    b.ch(',');
    kv_u64(b, "in_rel_seq", r.in_rel_seq);
    b.ch(',');
    kv_u64(b, "out_seq", r.out_seq);
    b.ch(',');
    kv_u64(b, "rel_seq", r.rel_seq);
    b.ch(',');
    kv_u64(b, "last_rel_seq", r.last_rel_seq);
    b.ch(',');
    kv_u64(b, "rel_len", r.rel_len);
    b.ch(',');
    kv_u64(b, "rel_fill", r.rel_fill);
    b.ch(',');
    kv_u64(b, "rate", r.rate);
    b.ch(',');
    kv_fixed(b, "cleartime", r.cleartime, 3);
    b.ch(',');
    kv_u64(b, "msg_flags", r.msg_flags);
    b.ch(',');
    kv_fixed(b, "last_received", r.last_received, 3);
    b.ch(',');
    kv_fixed(b, "connect_time", r.connect_time, 3);
    b.ch(',');
    kv_fixed(b, "now_real", r.now_real, 3);
    b.ch(',');
    kv_u64(b, "idle_ms", r.idle_ms);
    b.ch(',');
    kv_fixed(b, "sv_timeout", r.sv_timeout, 3);
    b.ch('}');
}

void emit_neighbors(JsonBuf& b, const NeighborsInfo& r) {
    b.ch('{');
    kv_u64(b, "count", r.count);
    b.ch(',');
    kv_fixed(b, "avg_loss", r.avg_loss, 3);
    b.ch(',');
    kv_fixed(b, "avg_ping", r.avg_ping, 3);
    b.ch(',');
    kv_fixed(b, "max_loss", r.max_loss, 3);
    b.ch('}');
}

void emit_server(JsonBuf& b, const ServerSample& r) {
    b.ch('{');
    kv_str(b, "type", "server");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_key(b, "frame_ms");
    b.ch('{');
    kv_fixed(b, "min", r.frame_ms_min, 3);
    b.ch(',');
    kv_fixed(b, "avg", r.frame_ms_avg, 3);
    b.ch(',');
    kv_fixed(b, "max", r.frame_ms_max, 3);
    b.ch('}');
    b.ch(',');
    kv_key(b, "hitch");
    b.ch('{');
    kv_u64(b, "gt50", r.hitch_gt50);
    b.ch(',');
    kv_u64(b, "gt100", r.hitch_gt100);
    b.ch('}');
    b.ch(',');
    kv_fixed(b, "cpu_pct", r.cpu_pct, 3);
    b.ch(',');
    kv_u64(b, "rss_kb", r.rss_kb);
    b.ch(',');
    kv_u64(b, "minflt", r.minflt);
    b.ch(',');
    kv_u64(b, "majflt", r.majflt);
    b.ch(',');
    kv_u64(b, "players", r.players);
    b.ch(',');
    kv_u64(b, "real", r.real);
    b.ch(',');
    kv_u64(b, "proxy", r.proxy);
    b.ch(',');
    kv_u64(b, "fake", r.fake);
    b.ch(',');
    kv_fixed(b, "in_kbps", r.in_kbps, 3);
    b.ch(',');
    kv_fixed(b, "out_kbps", r.out_kbps, 3);
    b.ch(',');
    kv_u64(b, "edicts", r.edicts);
    b.ch(',');
    kv_str(b, "map", r.map);
    b.ch(',');
    kv_u64(b, "uptime_s", r.uptime_s);
    b.ch(',');
    kv_fixed(b, "fps", r.fps, 3);
    b.ch(',');
    kv_u64(b, "maxclients", r.maxclients);
    b.ch('}');
}

void emit_drop(JsonBuf& b, const DropRecord& r) {
    b.ch('{');
    kv_str(b, "type", "drop");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "id", r.id);
    b.ch(',');
    kv_u64(b, "gen", r.gen);
    b.ch(',');
    kv_str(b, "reason", r.reason);
    b.ch(',');
    kv_str(b, "category", r.category);
    b.ch(',');
    kv_bool(b, "crash", r.crash != 0);
    b.ch(',');
    kv_str(b, "observed_via", r.observed_via);
    emit_identity(b, r);
    b.ch(',');
    kv_key(b, "client");
    emit_client_snapshot(b, r.client);
    b.ch(',');
    kv_key(b, "netchan");
    emit_netchan(b, r.netchan);
    b.ch(',');
    kv_key(b, "window");
    b.ch('[');
    {
        size_t wc = r.window_count;
        if (wc > HLDSMON_DROP_WINDOW_MAX) wc = HLDSMON_DROP_WINDOW_MAX;
        for (size_t i = 0; i < wc; ++i) {
            if (i != 0) b.ch(',');
            emit_client(b, r.window[i]);
        }
    }
    b.ch(']');
    b.ch(',');
    kv_key(b, "server");
    emit_server(b, r.server);
    b.ch(',');
    kv_key(b, "neighbors");
    emit_neighbors(b, r.neighbors);
    b.ch('}');
}

void emit_reject(JsonBuf& b, const RejectRecord& r) {
    b.ch('{');
    kv_str(b, "type", "reject");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_str(b, "ip", r.ip);
    b.ch(',');
    kv_str(b, "steamid", r.steamid);
    b.ch(',');
    kv_str(b, "stage", r.stage);
    b.ch(',');
    kv_str(b, "detail", r.detail);
    b.ch('}');
}

void emit_connect(JsonBuf& b, const ConnectRecord& r) {
    b.ch('{');
    kv_str(b, "type", "connect");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "id", r.id);
    b.ch(',');
    kv_str(b, "ip", r.ip);
    b.ch(',');
    kv_str(b, "steamid", r.steamid);
    b.ch(',');
    kv_str(b, "name", r.name);
    b.ch('}');
}

void emit_netcmd(JsonBuf& b, const NetCmdRecord& r) {
    b.ch('{');
    kv_str(b, "type", "netcmd");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "id", r.id);
    b.ch(',');
    kv_u64(b, "cmd", r.cmd);
    b.ch(',');
    kv_u64(b, "count", r.count);
    b.ch('}');
}

void emit_massdrop(JsonBuf& b, const MassDropRecord& r) {
    b.ch('{');
    kv_str(b, "type", "massdrop");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "window_s", r.window_s);
    b.ch(',');
    kv_u64(b, "count", r.count);
    b.ch(',');
    kv_str(b, "reason", r.reason);
    b.ch(',');
    kv_str(b, "category", r.category);
    b.ch('}');
}

void emit_packet(JsonBuf& b, const PacketStats& r) {
    b.ch('{');
    kv_str(b, "type", "packet");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "pps", r.pps);
    b.ch(',');
    kv_u64(b, "bytes", r.bytes);
    b.ch(',');
    kv_key(b, "sizes");
    b.ch('{');
    kv_u64(b, "lt128", r.sizes.lt128);
    b.ch(',');
    kv_u64(b, "lt512", r.sizes.lt512);
    b.ch(',');
    kv_u64(b, "lt1024", r.sizes.lt1024);
    b.ch(',');
    kv_u64(b, "ge1024", r.sizes.ge1024);
    b.ch('}');
    b.ch(',');
    kv_u64(b, "unique_ips", r.unique_ips);
    b.ch(',');
    kv_str(b, "top_ip", r.top_ip);
    b.ch('}');
}

void emit_hitch(JsonBuf& b, const HitchRecord& r) {
    b.ch('{');
    kv_str(b, "type", "hitch");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "duration_ms", r.duration_ms);
    b.ch('}');
}

void emit_mapchange(JsonBuf& b, const MapChangeRecord& r) {
    b.ch('{');
    kv_str(b, "type", "mapchange");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_str(b, "map", r.map);
    b.ch('}');
}

void emit_voice(JsonBuf& b, const VoiceRecord& r) {
    b.ch('{');
    kv_str(b, "type", "voice");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_u64(b, "id", r.id);
    b.ch(',');
    kv_fixed(b, "last_voice_time", r.last_voice_time, 3);
    b.ch('}');
}

void emit_shutdown(JsonBuf& b, const ShutdownRecord& r) {
    b.ch('{');
    kv_str(b, "type", "shutdown");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_str(b, "reason", r.reason);
    b.ch('}');
}

void emit_crash(JsonBuf& b, const CrashRecord& r) {
    b.ch('{');
    kv_str(b, "type", "crash");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_i64(b, "signal", r.signal);
    b.ch('}');
}

void emit_modnet(JsonBuf& b, const ModnetRecord& r) {
    b.ch('{');
    kv_str(b, "type", "modnet");
    b.ch(',');
    kv_fixed(b, "t", r.t, 3);
    b.ch(',');
    kv_str(b, "module", r.module);
    b.ch(',');
    kv_str(b, "so", r.so);
    b.ch(',');
    kv_u64(b, "conns", r.conns);
    b.ch(',');
    kv_u64(b, "conn_fail", r.conn_fail);
    b.ch(',');
    kv_u64(b, "connect_attempts", r.connect_attempts);
    b.ch(',');
    kv_u64(b, "dns_calls", r.dns_calls);
    b.ch(',');
    kv_u64(b, "in_bytes", r.in_bytes);
    b.ch(',');
    kv_u64(b, "out_bytes", r.out_bytes);
    b.ch(',');
    kv_fixed(b, "in_kbps", r.in_kbps, 3);
    b.ch(',');
    kv_fixed(b, "out_kbps", r.out_kbps, 3);
    b.ch('}');
}

// Measure pass then write pass; on short capacity nothing is written.
template <class Record>
using EmitFn = void (*)(JsonBuf&, const Record&);

template <class Record>
size_t write_record(char* out, size_t cap, const Record& r, EmitFn<Record> emit) {
    JsonBuf measure{nullptr, SIZE_MAX, 0, false};
    emit(measure, r);
    const size_t need = measure.pos;
    if (need + 1 > cap) return 0;
    JsonBuf writer{out, cap, 0, false};
    emit(writer, r);
    if (writer.overflow || writer.pos != need) return 0;
    out[need] = '\0';
    return need;
}

} // namespace

size_t json_escape(const char* in, char* out, size_t cap) {
    if (!in) in = "";
    size_t need = 0;
    for (const char* p = in; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' ||
            c == '\r' || c == '\t') {
            need += 2;
        } else if (c < 0x20) {
            need += 6;
        } else {
            need += 1;
        }
    }
    if (need + 1 > cap) return 0;

    size_t k = 0;
    for (const char* p = in; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"':  out[k++] = '\\'; out[k++] = '"'; break;
            case '\\': out[k++] = '\\'; out[k++] = '\\'; break;
            case '\b': out[k++] = '\\'; out[k++] = 'b'; break;
            case '\f': out[k++] = '\\'; out[k++] = 'f'; break;
            case '\n': out[k++] = '\\'; out[k++] = 'n'; break;
            case '\r': out[k++] = '\\'; out[k++] = 'r'; break;
            case '\t': out[k++] = '\\'; out[k++] = 't'; break;
            default:
                if (c < 0x20) {
                    out[k++] = '\\';
                    out[k++] = 'u';
                    out[k++] = '0';
                    out[k++] = '0';
                    out[k++] = kHexDigits[(c >> 4) & 0x0Fu];
                    out[k++] = kHexDigits[c & 0x0Fu];
                } else {
                    out[k++] = static_cast<char>(c);
                }
        }
    }
    out[k] = '\0';
    return k;
}

size_t write_client_json(char* out, size_t cap, const ClientSample& r) {
    return write_record(out, cap, r, &emit_client);
}
size_t write_server_json(char* out, size_t cap, const ServerSample& r) {
    return write_record(out, cap, r, &emit_server);
}
size_t write_drop_json(char* out, size_t cap, const DropRecord& r) {
    return write_record(out, cap, r, &emit_drop);
}
size_t write_reject_json(char* out, size_t cap, const RejectRecord& r) {
    return write_record(out, cap, r, &emit_reject);
}
size_t write_connect_json(char* out, size_t cap, const ConnectRecord& r) {
    return write_record(out, cap, r, &emit_connect);
}
size_t write_netcmd_json(char* out, size_t cap, const NetCmdRecord& r) {
    return write_record(out, cap, r, &emit_netcmd);
}
size_t write_massdrop_json(char* out, size_t cap, const MassDropRecord& r) {
    return write_record(out, cap, r, &emit_massdrop);
}
size_t write_packet_json(char* out, size_t cap, const PacketStats& r) {
    return write_record(out, cap, r, &emit_packet);
}
size_t write_hitch_json(char* out, size_t cap, const HitchRecord& r) {
    return write_record(out, cap, r, &emit_hitch);
}
size_t write_mapchange_json(char* out, size_t cap, const MapChangeRecord& r) {
    return write_record(out, cap, r, &emit_mapchange);
}
size_t write_voice_json(char* out, size_t cap, const VoiceRecord& r) {
    return write_record(out, cap, r, &emit_voice);
}
size_t write_shutdown_json(char* out, size_t cap, const ShutdownRecord& r) {
    return write_record(out, cap, r, &emit_shutdown);
}
size_t write_crash_json(char* out, size_t cap, const CrashRecord& r) {
    return write_record(out, cap, r, &emit_crash);
}
size_t write_modnet_json(char* out, size_t cap, const ModnetRecord& r) {
    return write_record(out, cap, r, &emit_modnet);
}

} // namespace hldsmon
