// HldsMonitoring — server console commands (plan Phase 4).

#ifndef HLDSMON_CONTROL_H
#define HLDSMON_CONTROL_H

namespace hldsmon {

// Registers hldsmon_status and hldsmon_flush_log with the engine.
void register_control_commands() noexcept;

} // namespace hldsmon

#endif // HLDSMON_CONTROL_H
