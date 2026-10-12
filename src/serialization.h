// HldsMonitoring — no-allocation JSONL serialization API (plan Phase 1, INV-2).
//
// Every writer serializes one record into a caller-provided buffer. No heap,
// no std::string/std::vector, no exceptions, no RTTI, no snprintf. On success
// the return value is the number of bytes written WITHOUT the trailing NUL; on
// insufficient capacity (cap < needed + 1) the functions write nothing at all
// and return 0 (never a partial line). All JSON string values are escaped.

#ifndef HLDSMON_SERIALIZATION_H
#define HLDSMON_SERIALIZATION_H

#include <cstddef>

#include "metrics.h"

namespace hldsmon {

// Escapes `in` into `out` (NUL-terminated), without surrounding quotes.
// Returns the number of bytes written excluding the terminator, or 0 when the
// buffer is too small (nothing is written) — an empty input also yields 0.
// Escaped: backslash, double-quote, \b \f \n \r \t, and control chars < 0x20
// as \u00XX.
size_t json_escape(const char* in, char* out, size_t cap);

// Each returns bytes written (excl. NUL) or 0 if cap is insufficient.
size_t write_client_json(char* out, size_t cap, const ClientSample& r);
size_t write_server_json(char* out, size_t cap, const ServerSample& r);
size_t write_drop_json(char* out, size_t cap, const DropRecord& r);
size_t write_reject_json(char* out, size_t cap, const RejectRecord& r);
size_t write_connect_json(char* out, size_t cap, const ConnectRecord& r);
size_t write_netcmd_json(char* out, size_t cap, const NetCmdRecord& r);
size_t write_massdrop_json(char* out, size_t cap, const MassDropRecord& r);
size_t write_packet_json(char* out, size_t cap, const PacketStats& r);
size_t write_hitch_json(char* out, size_t cap, const HitchRecord& r);
size_t write_mapchange_json(char* out, size_t cap, const MapChangeRecord& r);
size_t write_voice_json(char* out, size_t cap, const VoiceRecord& r);
size_t write_shutdown_json(char* out, size_t cap, const ShutdownRecord& r);
size_t write_crash_json(char* out, size_t cap, const CrashRecord& r);
size_t write_degraded_json(char* out, size_t cap, const DegradedRecord& r);
size_t write_retention_json(char* out, size_t cap, const RetentionRecord& r);
size_t write_modnet_json(char* out, size_t cap, const ModnetRecord& r);
size_t write_config_json(char* out, size_t cap, const ConfigRecord& r);
size_t write_host_json(char* out, size_t cap, const HostRecord& r);

} // namespace hldsmon

#endif // HLDSMON_SERIALIZATION_H
