// HldsMonitoring — hook registration and forwarding policy (plan Phase 3, INV-1).
//
// HARD RULE: every handler calls chain->callNext(...) and returns its result.
// No hook short-circuits the chain; PreprocessPacket never returns false.

#ifndef HLDSMON_HOOK_MANAGER_H
#define HLDSMON_HOOK_MANAGER_H

#include "plugin.h"

namespace hldsmon {

// ---- handler declarations (defined in hooks.cpp) ----
void h_SV_DropClient(IRehldsHook_SV_DropClient* chain, IGameClient* cl, bool crash, const char* reason);
void h_Steam_NotifyClientDisconnect(IRehldsHook_Steam_NotifyClientDisconnect* chain, IGameClient* cl);
void h_ClientConnected(IRehldsHook_ClientConnected* chain, IGameClient* cl);
void h_SV_ConnectClient(IRehldsHook_SV_ConnectClient* chain);
int h_SV_CheckProtocol(IRehldsHook_SV_CheckProtocol* chain, netadr_t* adr, int protocol);
int h_SV_CheckIPRestrictions(IRehldsHook_SV_CheckIPRestrictions* chain, netadr_t* adr, int type);
int h_SV_FinishCertificateCheck(IRehldsHook_SV_FinishCertificateCheck* chain, netadr_t* adr, int type, char* key, char* val);
int h_SV_CheckKeyInfo(IRehldsHook_SV_CheckKeyInfo* chain, netadr_t* adr, char* key, uint16* pkc, int* pkg, char* steamid, char* val);
int h_SV_CheckUserInfo(IRehldsHook_SV_CheckUserInfo* chain, netadr_t* adr, char* userinfo, qboolean reconnect, int slot, char* name);
qboolean h_Steam_NotifyClientConnect(IRehldsHook_Steam_NotifyClientConnect* chain, IGameClient* cl, const void* key, unsigned int len);
void h_SVC_GetChallenge_mod(IRehldsHook_SVC_GetChallenge_mod* chain, char* buffer, int len);
bool h_PreprocessPacket(IRehldsHook_PreprocessPacket* chain, uint8* data, unsigned int len, const netadr_t& adr);
void h_HandleNetCommand(IRehldsHook_HandleNetCommand* chain, IGameClient* cl, uint8 cmd);
void h_SV_Frame(IRehldsHook_SV_Frame* chain);
void h_SV_Spawn_f(IRehldsHook_SV_Spawn_f* chain);

// Registers all hooks. Returns false when no hook could be registered.
// Partial failures set ctx().hooks_degraded() and the remaining hooks proceed.
bool register_hooks() noexcept;
void unregister_hooks() noexcept;
bool hooks_degraded() noexcept;

} // namespace hldsmon

#endif // HLDSMON_HOOK_MANAGER_H
