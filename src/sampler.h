// HldsMonitoring — per-frame server sampling + 1 Hz aggregation (plan Phase 3).

#ifndef HLDSMON_SAMPLER_H
#define HLDSMON_SAMPLER_H

namespace hldsmon {

// Called from the SV_Frame hook. `frame_ms` is the frame duration in ms and
// `now_real` is the engine real time (RehldsFuncs_t::GetRealTime). Does nothing
// when the plugin is disabled or not attached.
void sampler_on_frame(double frame_ms, double now_real) noexcept;

// Current map name from the engine globals (never null; may be empty).
const char* current_map() noexcept;

} // namespace hldsmon

#endif // HLDSMON_SAMPLER_H
