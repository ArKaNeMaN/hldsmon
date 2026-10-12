// HldsMonitoring — no-allocation JSONL serialization API (plan Phase 1, INV-2).
//
// Every writer serializes one record into a caller-provided buffer. No heap,
// no std::string/std::vector, no exceptions, no RTTI, no snprintf. On success
// the return value is the number of bytes written WITHOUT the trailing NUL; on
// insufficient capacity (out.size() < needed + 1) the functions write nothing
// at all and return 0 (never a partial line). All JSON string values are
// escaped.

#ifndef HLDSMON_SERIALIZATION_H
#define HLDSMON_SERIALIZATION_H

#include <cstddef>
#include <span>

#include "metrics.h"

namespace hldsmon {

// Escapes `in` into `out` (NUL-terminated), without surrounding quotes.
// Returns the number of bytes written excluding the terminator, or 0 when the
// buffer is too small (nothing is written) — an empty input also yields 0.
// Escaped: backslash, double-quote, \b \f \n \r \t, and control chars < 0x20
// as \u00XX.
size_t json_escape(const char* in, std::span<char> out);

// Each returns bytes written (excl. NUL) or 0 if out.size() is insufficient.
size_t write_client_json(std::span<char> out, const ClientSample& r);
size_t write_server_json(std::span<char> out, const ServerSample& r);
size_t write_drop_json(std::span<char> out, const DropRecord& r);
size_t write_reject_json(std::span<char> out, const RejectRecord& r);
size_t write_connect_json(std::span<char> out, const ConnectRecord& r);
size_t write_netcmd_json(std::span<char> out, const NetCmdRecord& r);
size_t write_massdrop_json(std::span<char> out, const MassDropRecord& r);
size_t write_packet_json(std::span<char> out, const PacketStats& r);
size_t write_hitch_json(std::span<char> out, const HitchRecord& r);
size_t write_mapchange_json(std::span<char> out, const MapChangeRecord& r);
size_t write_voice_json(std::span<char> out, const VoiceRecord& r);
size_t write_shutdown_json(std::span<char> out, const ShutdownRecord& r);
size_t write_crash_json(std::span<char> out, const CrashRecord& r);
size_t write_degraded_json(std::span<char> out, const DegradedRecord& r);
size_t write_retention_json(std::span<char> out, const RetentionRecord& r);
size_t write_modnet_json(std::span<char> out, const ModnetRecord& r);
size_t write_config_json(std::span<char> out, const ConfigRecord& r);
size_t write_host_json(std::span<char> out, const HostRecord& r);

} // namespace hldsmon

#endif // HLDSMON_SERIALIZATION_H
