// HldsMonitoring — engine layout access + runtime ABI probe (plan Phase 3, INV-4).
//
// This header is deliberately ENGINE-FREE so the probe predicate can be
// unit-tested on the host without any vendored engine headers. The engine
// adapter that fills ClientProbeView lives in engine_layout.cpp, the only
// translation unit that includes the concrete client_t/netchan_t definitions.
//
// ABI rules (design Appendix B):
//  - client_t is reached ONLY via IRehldsServerStatic::GetClient_t(id).
//  - IGameClient* is NEVER reinterpret_cast to client_t*.
//  - At Meta_Attach the adapter validates the observed layout against the
//    public getters; on mismatch the plugin fails closed.

#ifndef HLDSMON_ENGINE_LAYOUT_H
#define HLDSMON_ENGINE_LAYOUT_H

#include <cstdint>

class IRehldsApi;
class IRehldsServerStatic;
struct client_t;

namespace hldsmon {

// POD view assembled from one engine client slice. Pure data, no pointers, so
// the predicate below is trivially host-testable.
struct ClientProbeView {
    uint8_t  id;             // slot index the engine returned the client for
    uint8_t  expected_id;    // IGameClient::GetId()
    uint32_t userid;         // client_t::userid
    uint32_t remote_ip;      // client_t::netchan.remote_address.ip (big-endian)
    uint32_t expected_ip;    // IGameClient::GetNetChan()->GetRemoteAdr()->ip
    double   connect_time;   // client_t::netchan.connect_time
    double   last_received;  // client_t::netchan.last_received
    double   now;            // engine real time (RehldsFuncs_t::GetRealTime)
};

// Pure inline predicate: true when the observed slice is self-consistent with
// the public ReHLDS getters. No engine state, no allocation.
inline bool probe_ok(const ClientProbeView& v) noexcept {
    if (v.id != v.expected_id) {
        return false; // slot mapping disagrees with IGameClient::GetId()
    }
    if (v.remote_ip != v.expected_ip) {
        return false; // netchan address disagrees with GetRemoteAdr()
    }
    if (v.userid > 0xFFFFu) {
        return false; // implausible userid => struct offset drift
    }
    if (v.connect_time < 0.0 || v.connect_time > v.now + 1.0) {
        return false; // connect time outside [0, now]
    }
    if (v.last_received < 0.0 || v.last_received > v.now + 1.0) {
        return false; // receive time outside [0, now]
    }
    return true;
}

// Outcome of scanning all client slots at attach time.
struct LayoutProbeResult {
    bool    ok;
    int     checked;         // slots that produced a view
    int     first_bad_id;    // slot id of first failure (-1 when ok)
    uint8_t first_bad_reason; // 1=client null, 2=client_t null, 3=netchan null,
                              // 4=predicate rejected the view
};

// Engine adapter. Defined in engine_layout.cpp.
LayoutProbeResult run_layout_probe(IRehldsServerStatic* svs,
                                   IRehldsApi* api) noexcept;

// The single supported path from a client id to client_t: GetClient_t(id).
// Defined in engine_layout.cpp; never cast IGameClient* to client_t*.
client_t* client_by_id(IRehldsServerStatic* svs, int id) noexcept;

} // namespace hldsmon

#endif // HLDSMON_ENGINE_LAYOUT_H
