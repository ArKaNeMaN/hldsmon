// HldsMonitoring — engine layout adapter (plan Phase 3, INV-4).
//
// The ONLY translation unit that reaches into the concrete client_t/netchan_t
// definitions. It builds a POD ClientProbeView per client slot and feeds the
// pure predicate declared in engine_layout.h. Access to client_t is strictly
// GetId() -> GetClient_t(id); IGameClient* is never cast to client_t*.

#include "plugin.h"

namespace hldsmon {
namespace {

inline uint32_t pack_ip(const unsigned char ip[4]) noexcept {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

} // namespace

client_t* client_by_id(IRehldsServerStatic* svs, int id) noexcept {
    if (svs == nullptr || id < 0) {
        return nullptr;
    }
    return svs->GetClient_t(id);
}

LayoutProbeResult run_layout_probe(IRehldsServerStatic* svs,
                                   IRehldsApi* api) noexcept {
    LayoutProbeResult res;
    res.ok = true;
    res.checked = 0;
    res.first_bad_id = -1;
    res.first_bad_reason = 0;

    if (svs == nullptr || api == nullptr) {
        res.ok = false;
        res.first_bad_id = -1;
        res.first_bad_reason = 1;
        return res;
    }

    const RehldsFuncs_t* funcs = api->GetFuncs();
    const double now = funcs != nullptr ? funcs->GetRealTime() : 0.0;

    const int maxcl = svs->GetMaxClients();
    if (maxcl <= 0 || maxcl > MAX_CLIENTS) {
        res.ok = false;
        res.first_bad_id = -1;
        res.first_bad_reason = 1;
        return res;
    }

    for (int id = 0; id < maxcl; ++id) {
        IGameClient* gc = svs->GetClient(id);
        if (gc == nullptr) {
            res.ok = false;
            res.first_bad_id = id;
            res.first_bad_reason = 1;
            return res;
        }

        client_t* ct = client_by_id(svs, id);
        if (ct == nullptr) {
            res.ok = false;
            res.first_bad_id = id;
            res.first_bad_reason = 2;
            return res;
        }

        INetChan* nc = gc->GetNetChan();
        if (nc == nullptr) {
            res.ok = false;
            res.first_bad_id = id;
            res.first_bad_reason = 3;
            return res;
        }
        const netadr_t* adr = nc->GetRemoteAdr();
        if (adr == nullptr) {
            res.ok = false;
            res.first_bad_id = id;
            res.first_bad_reason = 3;
            return res;
        }

        ClientProbeView v;
        v.id = static_cast<uint8_t>(id);
        v.expected_id = static_cast<uint8_t>(gc->GetId());
        v.userid = static_cast<uint32_t>(ct->userid);
        v.remote_ip = pack_ip(ct->netchan.remote_address.ip);
        v.expected_ip = pack_ip(adr->ip);
        v.connect_time = static_cast<double>(ct->netchan.connect_time);
        v.last_received = static_cast<double>(ct->netchan.last_received);
        v.now = now;

        ++res.checked;
        if (!probe_ok(v)) {
            res.ok = false;
            res.first_bad_id = id;
            res.first_bad_reason = 4;
            return res;
        }
    }

    return res;
}

} // namespace hldsmon
