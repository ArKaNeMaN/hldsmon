// HldsMonitoring — hldsmon_status / hldsmon_flush_log (plan Phase 4).
//
// Runs on the server thread from the console; output is built into the
// preallocated line buffer without snprintf/heap (kept consistent with INV-2).

#include "plugin.h"
#include "control.h"

namespace hldsmon {
namespace {

void ap(char* out, size_t cap, size_t& pos, const char* s) noexcept {
    if (s == nullptr) {
        return;
    }
    for (; *s != '\0' && pos + 1 < cap; ++s) {
        out[pos++] = *s;
    }
}

void ap_u32(char* out, size_t cap, size_t& pos, uint32_t v) noexcept {
    append_u32_dec(out, cap, pos, v);
}

void ap_u64(char* out, size_t cap, size_t& pos, uint64_t v) noexcept {
    if (v == 0) {
        append_u32_dec(out, cap, pos, 0);
        return;
    }
    char tmp[20];
    int n = 0;
    while (v != 0) {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10u));
        v /= 10u;
    }
    while (n > 0 && pos + 1 < cap) {
        out[pos++] = tmp[--n];
    }
}

void ap_yn(char* out, size_t cap, size_t& pos, bool v) noexcept {
    ap(out, cap, pos, v ? "yes" : "no");
}

void cmd_status() {
    PluginContext& c = ctx();
    if (c.log == nullptr) {
        if (g_engfuncs.pfnServerPrint != nullptr) {
            g_engfuncs.pfnServerPrint("hldsmon: log not initialized\n");
        }
        return;
    }
    char* out = c.line_buf;
    const size_t cap = kLineBufSize;
    size_t pos = 0;

    uint32_t cr_size = 0;
    uint32_t cr_cap = 0;
    for (int i = 0; i < c.maxclients; ++i) {
        cr_size += static_cast<uint32_t>(c.clients[i].ring.size());
        cr_cap += static_cast<uint32_t>(c.clients[i].ring.capacity());
    }

    ap(out, cap, pos, "hldsmon status\n");
    ap(out, cap, pos, "  file: ");
    ap(out, cap, pos, c.log->current_path());
    ap(out, cap, pos, "\n  bytes_today: ");
    ap_u64(out, cap, pos, c.log->bytes_today());
    ap(out, cap, pos, "\n  enabled: ");
    ap_yn(out, cap, pos, plugin_enabled());
    ap(out, cap, pos, " log_enabled: ");
    ap_yn(out, cap, pos, c.log->enabled());
    ap(out, cap, pos, " size_capped: ");
    ap_yn(out, cap, pos, c.log->size_capped());
    ap(out, cap, pos, " hooks_degraded: ");
    ap_yn(out, cap, pos, c.hooks_degraded);
    ap(out, cap, pos, "\n  layout_ok: ");
    ap_yn(out, cap, pos, c.layout_ok);
    ap(out, cap, pos, " layout_checked: ");
    ap_u32(out, cap, pos, static_cast<uint32_t>(c.layout_checked));
    ap(out, cap, pos, "\n  rings: server ");
    ap_u32(out, cap, pos, static_cast<uint32_t>(c.server_ring.size()));
    ap(out, cap, pos, "/");
    ap_u32(out, cap, pos, static_cast<uint32_t>(c.server_ring.capacity()));
    ap(out, cap, pos, " client ");
    ap_u32(out, cap, pos, cr_size);
    ap(out, cap, pos, "/");
    ap_u32(out, cap, pos, cr_cap);
    ap(out, cap, pos, "\n  drops: ");
    ap_u32(out, cap, pos, c.drop_count);
    ap(out, cap, pos, " fallback: ");
    ap_u32(out, cap, pos, c.fallback_drop_count);
    ap(out, cap, pos, " rejects: ");
    ap_u32(out, cap, pos, c.reject_count);
    ap(out, cap, pos, " massdrop: ");
    ap_u32(out, cap, pos, c.massdrop_count);
    ap(out, cap, pos, " hitch: ");
    ap_u32(out, cap, pos, c.hitch_count);
    ap(out, cap, pos, "\n  write_errors: ");
    ap_u64(out, cap, pos, c.log->error_count());
    ap(out, cap, pos, " pruned: ");
    ap_u32(out, cap, pos, c.log->last_prune_count());
    ap(out, cap, pos, " serialize_failures: ");
    ap_u32(out, cap, pos, c.serialize_failures);
    ap(out, cap, pos, "\n  pkt_interval: ");
    ap_u32(out, cap, pos, c.pkt_count);
    ap(out, cap, pos, " bytes: ");
    ap_u64(out, cap, pos, c.pkt_bytes);
    ap(out, cap, pos, " unique_ips: ");
    ap_u32(out, cap, pos, c.ip_unique);
    ap(out, cap, pos, " ip_overflow: ");
    ap_u32(out, cap, pos, c.ip_overflow);
    ap(out, cap, pos, "\n  connects: ");
    ap_u32(out, cap, pos, c.connect_attempts);
    ap(out, cap, pos, " challenges: ");
    ap_u32(out, cap, pos, c.challenge_count);
    ap(out, cap, pos, "\n  modnet: enabled ");
    ap_yn(out, cap, pos, c.modnet.enabled);
    ap(out, cap, pos, " probe_ok ");
    ap_yn(out, cap, pos, c.modnet.probe_ok);
    ap(out, cap, pos, " lazy_bound ");
    ap_yn(out, cap, pos, c.modnet.lazy_bound);
    ap(out, cap, pos, " patched_objects ");
    ap_u32(out, cap, pos, c.modnet.patched_objects);
    ap(out, cap, pos, " patched_slots ");
    ap_u32(out, cap, pos, c.modnet.patched_slots);
    ap(out, cap, pos, " restored_slots ");
    ap_u32(out, cap, pos, c.modnet.restored_slots);
    ap(out, cap, pos, " conflicts ");
    ap_u32(out, cap, pos, c.modnet.conflicts);
    ap(out, cap, pos, "\n    modnet_targets: ");
    ap(out, cap, pos, c.modnet.targets[0] != '\0' ? c.modnet.targets : "(none)");
    ap(out, cap, pos, " last_error: ");
    ap(out, cap, pos, c.modnet.last_error[0] != '\0' ? c.modnet.last_error : "-");
    ap(out, cap, pos, "\n");

    out[pos < cap ? pos : cap - 1] = '\0';
    if (g_engfuncs.pfnServerPrint != nullptr) {
        g_engfuncs.pfnServerPrint(out);
    }
}

void cmd_flush_log() {
    if (ctx().log != nullptr) {
        ctx().log->flush();
    }
    if (g_engfuncs.pfnServerPrint != nullptr) {
        g_engfuncs.pfnServerPrint("hldsmon: log flushed\n");
    }
}

char s_cmd_status[] = "hldsmon_status";
char s_cmd_flush[] = "hldsmon_flush_log";

} // namespace

void register_control_commands() noexcept {
    if (g_engfuncs.pfnAddServerCommand == nullptr) {
        return;
    }
    g_engfuncs.pfnAddServerCommand(s_cmd_status, &cmd_status);
    g_engfuncs.pfnAddServerCommand(s_cmd_flush, &cmd_flush_log);
}

} // namespace hldsmon
