// HldsMonitoring — record emission layer (plan Phase 3, design §7, INV-5).
//
// Serializes the frozen POD records into the preallocated line buffer via the
// no-allocation serialization API, then hands the bytes to LogWriter. All
// writes are gated by hldsmon_enable; when disabled no bytes are produced.

#ifndef HLDSMON_EVENTS_H
#define HLDSMON_EVENTS_H

#include "metrics.h"

namespace hldsmon {
namespace events {

void write_client(const ClientSample& r);
void write_server(const ServerSample& r);
void write_drop(const DropRecord& r);
void write_reject(const RejectRecord& r);
void write_connect(const ConnectRecord& r);
void write_netcmd(const NetCmdRecord& r);
void write_massdrop(const MassDropRecord& r);
void write_packet(const PacketStats& r);
void write_hitch(const HitchRecord& r);
void write_mapchange(const MapChangeRecord& r);
void write_voice(const VoiceRecord& r);
void write_shutdown(const ShutdownRecord& r);
void write_crash(const CrashRecord& r);
void write_degraded(const DegradedRecord& r);
void write_retention(const RetentionRecord& r);
void write_modnet(const ModnetRecord& r);
void write_config(const ConfigRecord& r);
void write_host(const HostRecord& r);

} // namespace events
} // namespace hldsmon

#endif // HLDSMON_EVENTS_H
