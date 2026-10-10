// HldsMonitoring — record emission layer (plan Phase 3).

#include "plugin.h"
#include "events.h"

namespace hldsmon {
namespace events {
namespace {

template <class T>
using Writer = size_t (*)(char*, size_t, const T&);

template <class T>
void emit(const T& r, Writer<T> fn) {
    if (!plugin_enabled()) {
        return;
    }
    PluginContext& c = ctx();
    if (c.log == nullptr) {
        return;
    }
    const size_t n = fn(c.line_buf, kLineBufSize, r);
    if (n == 0) {
        ++c.serialize_failures;
        return;
    }
    c.log->write(c.line_buf, n);
}

} // namespace

void write_client(const ClientSample& r)    { emit(r, &write_client_json); }
void write_server(const ServerSample& r)    { emit(r, &write_server_json); }
void write_drop(const DropRecord& r)        { emit(r, &write_drop_json); }
void write_reject(const RejectRecord& r)    { emit(r, &write_reject_json); }
void write_connect(const ConnectRecord& r)  { emit(r, &write_connect_json); }
void write_netcmd(const NetCmdRecord& r)    { emit(r, &write_netcmd_json); }
void write_massdrop(const MassDropRecord& r){ emit(r, &write_massdrop_json); }
void write_packet(const PacketStats& r)     { emit(r, &write_packet_json); }
void write_hitch(const HitchRecord& r)      { emit(r, &write_hitch_json); }
void write_mapchange(const MapChangeRecord& r){ emit(r, &write_mapchange_json); }
void write_voice(const VoiceRecord& r)      { emit(r, &write_voice_json); }
void write_shutdown(const ShutdownRecord& r){ emit(r, &write_shutdown_json); }
void write_crash(const CrashRecord& r)      { emit(r, &write_crash_json); }
void write_modnet(const ModnetRecord& r)    { emit(r, &write_modnet_json); }

} // namespace events
} // namespace hldsmon
