// HldsMonitoring — hook registration / unregistration (plan Phase 3).

#include "plugin.h"
#include "hook_manager.h"

namespace hldsmon {
namespace {

// Priority contract (design §5.2): SV_DropClient runs at the HEAD of the chain
// (HC_PRIORITY_UNINTERRUPTABLE == 255) so our drop snapshot is taken before any
// other plugin can short-circuit the chain. All other hooks use the default
// priority (128).
enum HookBit : uint32_t {
    kB_Drop           = 1u << 0,
    kB_SteamDisc      = 1u << 1,
    kB_ClientConnect  = 1u << 2,
    kB_ConnectClient  = 1u << 3,
    kB_CheckProtocol  = 1u << 4,
    kB_CheckIP        = 1u << 5,
    kB_FinishCert     = 1u << 6,
    kB_CheckKeyInfo   = 1u << 7,
    kB_CheckUserInfo  = 1u << 8,
    kB_SteamConnect   = 1u << 9,
    kB_Challenge      = 1u << 10,
    kB_Preprocess     = 1u << 11,
    kB_NetCommand     = 1u << 12,
    kB_Frame          = 1u << 13,
    kB_Spawn          = 1u << 14,
};

uint32_t g_reg_mask = 0;

template <class Registry, class Hook>
bool reg_one(Registry* r, Hook h, int priority, uint32_t bit) {
    if (r == nullptr) {
        return false;
    }
    r->registerHook(h, priority);
    g_reg_mask |= bit;
    return true;
}

template <class Registry, class Hook>
void unreg_one(Registry* r, Hook h, uint32_t bit) {
    if (r != nullptr && (g_reg_mask & bit) != 0u) {
        r->unregisterHook(h);
        g_reg_mask &= ~bit;
    }
}

} // namespace

bool register_hooks() noexcept {
    PluginContext& c = ctx();
    IRehldsHookchains* hc = c.hookchains;
    if (hc == nullptr) {
        c.hooks_degraded = true;
        return false;
    }

    int ok = 0;
    int fail = 0;
    auto note = [&](bool good) { if (good) { ++ok; } else { ++fail; } };

    note(reg_one(hc->SV_DropClient(), &h_SV_DropClient, HC_PRIORITY_UNINTERRUPTABLE, kB_Drop));
    note(reg_one(hc->Steam_NotifyClientDisconnect(), &h_Steam_NotifyClientDisconnect, HC_PRIORITY_DEFAULT, kB_SteamDisc));
    note(reg_one(hc->ClientConnected(), &h_ClientConnected, HC_PRIORITY_DEFAULT, kB_ClientConnect));
    note(reg_one(hc->SV_ConnectClient(), &h_SV_ConnectClient, HC_PRIORITY_DEFAULT, kB_ConnectClient));
    note(reg_one(hc->SV_CheckProtocol(), &h_SV_CheckProtocol, HC_PRIORITY_DEFAULT, kB_CheckProtocol));
    note(reg_one(hc->SV_CheckIPRestrictions(), &h_SV_CheckIPRestrictions, HC_PRIORITY_DEFAULT, kB_CheckIP));
    note(reg_one(hc->SV_FinishCertificateCheck(), &h_SV_FinishCertificateCheck, HC_PRIORITY_DEFAULT, kB_FinishCert));
    note(reg_one(hc->SV_CheckKeyInfo(), &h_SV_CheckKeyInfo, HC_PRIORITY_DEFAULT, kB_CheckKeyInfo));
    note(reg_one(hc->SV_CheckUserInfo(), &h_SV_CheckUserInfo, HC_PRIORITY_DEFAULT, kB_CheckUserInfo));
    note(reg_one(hc->Steam_NotifyClientConnect(), &h_Steam_NotifyClientConnect, HC_PRIORITY_DEFAULT, kB_SteamConnect));
    note(reg_one(hc->SVC_GetChallenge_mod(), &h_SVC_GetChallenge_mod, HC_PRIORITY_DEFAULT, kB_Challenge));
    note(reg_one(hc->PreprocessPacket(), &h_PreprocessPacket, HC_PRIORITY_DEFAULT, kB_Preprocess));
    note(reg_one(hc->HandleNetCommand(), &h_HandleNetCommand, HC_PRIORITY_DEFAULT, kB_NetCommand));
    note(reg_one(hc->SV_Frame(), &h_SV_Frame, HC_PRIORITY_DEFAULT, kB_Frame));
    note(reg_one(hc->SV_Spawn_f(), &h_SV_Spawn_f, HC_PRIORITY_DEFAULT, kB_Spawn));

    if (fail > 0) {
        c.hooks_degraded = true;
    }
    return ok > 0;
}

void unregister_hooks() noexcept {
    PluginContext& c = ctx();
    IRehldsHookchains* hc = c.hookchains;
    if (hc == nullptr) {
        g_reg_mask = 0;
        return;
    }

    unreg_one(hc->SV_DropClient(), &h_SV_DropClient, kB_Drop);
    unreg_one(hc->Steam_NotifyClientDisconnect(), &h_Steam_NotifyClientDisconnect, kB_SteamDisc);
    unreg_one(hc->ClientConnected(), &h_ClientConnected, kB_ClientConnect);
    unreg_one(hc->SV_ConnectClient(), &h_SV_ConnectClient, kB_ConnectClient);
    unreg_one(hc->SV_CheckProtocol(), &h_SV_CheckProtocol, kB_CheckProtocol);
    unreg_one(hc->SV_CheckIPRestrictions(), &h_SV_CheckIPRestrictions, kB_CheckIP);
    unreg_one(hc->SV_FinishCertificateCheck(), &h_SV_FinishCertificateCheck, kB_FinishCert);
    unreg_one(hc->SV_CheckKeyInfo(), &h_SV_CheckKeyInfo, kB_CheckKeyInfo);
    unreg_one(hc->SV_CheckUserInfo(), &h_SV_CheckUserInfo, kB_CheckUserInfo);
    unreg_one(hc->Steam_NotifyClientConnect(), &h_Steam_NotifyClientConnect, kB_SteamConnect);
    unreg_one(hc->SVC_GetChallenge_mod(), &h_SVC_GetChallenge_mod, kB_Challenge);
    unreg_one(hc->PreprocessPacket(), &h_PreprocessPacket, kB_Preprocess);
    unreg_one(hc->HandleNetCommand(), &h_HandleNetCommand, kB_NetCommand);
    unreg_one(hc->SV_Frame(), &h_SV_Frame, kB_Frame);
    unreg_one(hc->SV_Spawn_f(), &h_SV_Spawn_f, kB_Spawn);

    g_reg_mask = 0;
    c.hooks_degraded = false;
}

bool hooks_degraded() noexcept {
    return ctx().hooks_degraded;
}

} // namespace hldsmon
