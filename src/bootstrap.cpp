// HldsMonitoring — Metamod entry points and plugin lifecycle (plan Phase 3/4).
//
// Meta_Attach resolves the ReHLDS API, gates on the version, registers cvars,
// allocates all state, runs the runtime layout probe (fail-closed) and installs
// hooks. Meta_Detach removes hooks and closes the LogWriter.

#include "plugin.h"
#include "control.h"
#include "hook_manager.h"
#include "events.h"

#include <new>

// ---- engine/metamod globals ----
enginefuncs_t      g_engfuncs;
globalvars_t*      gpGlobals       = nullptr;
meta_globals_t*    gpMetaGlobals   = nullptr;
gamedll_funcs_t*   gpGamedllFuncs  = nullptr;
mutil_funcs_t*     gpMetaUtilFuncs = nullptr;
edict_t*           g_pEdicts       = nullptr;
IRehldsServerStatic* g_RehldsSvs   = nullptr;

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,   // ifvers
    "HldsMonitoring",         // name
    "0.3.0",                  // version
    __DATE__,                 // date
    "Sisyphus",               // author
    "",                       // url
    "HLDSMON",                // logtag
    PT_ANYTIME,               // loadable
    PT_ANYTIME,               // unloadable
};

static META_FUNCTIONS gMetaFunctionTable;

namespace hldsmon {

PluginContext& ctx() noexcept {
    static PluginContext s;
    return s;
}

namespace {

// ---- cvar storage (mutable strings; engine owns value/string after register) ----
char s_val_enable[]      = "1";
char s_val_client_hz[]   = "1";
char s_val_disk_hz[]     = "1";
char s_val_flush_sec[]   = "1";
char s_val_client_ring[] = "300";
char s_val_server_ring[] = "3600";
char s_val_drop_window[] = "60";
char s_val_dir[]         = "addons/hldsmon/logs";
char s_val_max_mb[]      = "512";
char s_val_ip_table[]    = "4096";
char s_val_retention_days[] = "3";
char s_val_modnet[]           = "0";
char s_val_modnet_targets[]   = "";
char s_val_modnet_rescan_sec[] = "5";

cvar_t s_cvar_enable;
cvar_t s_cvar_client_hz;
cvar_t s_cvar_disk_hz;
cvar_t s_cvar_flush_sec;
cvar_t s_cvar_client_ring;
cvar_t s_cvar_server_ring;
cvar_t s_cvar_drop_window;
cvar_t s_cvar_dir;
cvar_t s_cvar_max_mb;
cvar_t s_cvar_ip_table;
cvar_t s_cvar_retention_days;
cvar_t s_cvar_modnet;
cvar_t s_cvar_modnet_targets;
cvar_t s_cvar_modnet_rescan_sec;

void util_log(const char* msg) noexcept {
    if (gpMetaUtilFuncs != nullptr && gpMetaUtilFuncs->pfnLogConsole != nullptr) {
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[%s] %s\n", Plugin_info.logtag, msg);
    }
}

void reg_cvar(cvar_t& cv, const char* name, char* value, int flags) noexcept {
    std::memset(&cv, 0, sizeof(cv));
    cv.name = name;
    cv.string = value;
    cv.flags = flags;
    if (g_engfuncs.pfnCVarRegister != nullptr) {
        g_engfuncs.pfnCVarRegister(&cv);
    }
}

cvar_t* engine_cvar(const char* name, cvar_t* fallback) noexcept {
    if (g_engfuncs.pfnCVarGetPointer != nullptr) {
        cvar_t* p = g_engfuncs.pfnCVarGetPointer(name);
        if (p != nullptr) {
            return p;
        }
    }
    return fallback;
}

void register_cvars() noexcept {
    PluginContext& c = ctx();
    reg_cvar(s_cvar_enable,      "hldsmon_enable",      s_val_enable,      FCVAR_ARCHIVE);
    reg_cvar(s_cvar_client_hz,   "hldsmon_client_hz",   s_val_client_hz,   FCVAR_ARCHIVE);
    reg_cvar(s_cvar_disk_hz,     "hldsmon_disk_hz",     s_val_disk_hz,     FCVAR_ARCHIVE);
    reg_cvar(s_cvar_flush_sec,   "hldsmon_flush_sec",   s_val_flush_sec,   FCVAR_ARCHIVE);
    reg_cvar(s_cvar_client_ring, "hldsmon_client_ring", s_val_client_ring, FCVAR_ARCHIVE);
    reg_cvar(s_cvar_server_ring, "hldsmon_server_ring", s_val_server_ring, FCVAR_ARCHIVE);
    reg_cvar(s_cvar_drop_window, "hldsmon_drop_window", s_val_drop_window, FCVAR_ARCHIVE);
    reg_cvar(s_cvar_dir,         "hldsmon_dir",         s_val_dir,         FCVAR_ARCHIVE);
    reg_cvar(s_cvar_max_mb,      "hldsmon_max_mb",      s_val_max_mb,      FCVAR_ARCHIVE);
    reg_cvar(s_cvar_ip_table,    "hldsmon_ip_table",    s_val_ip_table,    FCVAR_ARCHIVE);
    reg_cvar(s_cvar_retention_days, "hldsmon_retention_days", s_val_retention_days, FCVAR_ARCHIVE);
    reg_cvar(s_cvar_modnet,           "hldsmon_modnet",           s_val_modnet,           FCVAR_ARCHIVE);
    reg_cvar(s_cvar_modnet_targets,   "hldsmon_modnet_targets",   s_val_modnet_targets,   FCVAR_ARCHIVE);
    reg_cvar(s_cvar_modnet_rescan_sec, "hldsmon_modnet_rescan_sec", s_val_modnet_rescan_sec, FCVAR_ARCHIVE);

    c.cvars.enable      = engine_cvar("hldsmon_enable",      &s_cvar_enable);
    c.cvars.client_hz   = engine_cvar("hldsmon_client_hz",   &s_cvar_client_hz);
    c.cvars.disk_hz     = engine_cvar("hldsmon_disk_hz",     &s_cvar_disk_hz);
    c.cvars.flush_sec   = engine_cvar("hldsmon_flush_sec",   &s_cvar_flush_sec);
    c.cvars.client_ring = engine_cvar("hldsmon_client_ring", &s_cvar_client_ring);
    c.cvars.server_ring = engine_cvar("hldsmon_server_ring", &s_cvar_server_ring);
    c.cvars.drop_window = engine_cvar("hldsmon_drop_window", &s_cvar_drop_window);
    c.cvars.dir         = engine_cvar("hldsmon_dir",         &s_cvar_dir);
    c.cvars.max_mb      = engine_cvar("hldsmon_max_mb",      &s_cvar_max_mb);
    c.cvars.ip_table    = engine_cvar("hldsmon_ip_table",    &s_cvar_ip_table);
    c.cvars.retention_days = engine_cvar("hldsmon_retention_days", &s_cvar_retention_days);
    c.cvars.modnet           = engine_cvar("hldsmon_modnet",           &s_cvar_modnet);
    c.cvars.modnet_targets   = engine_cvar("hldsmon_modnet_targets",   &s_cvar_modnet_targets);
    c.cvars.modnet_rescan_sec = engine_cvar("hldsmon_modnet_rescan_sec", &s_cvar_modnet_rescan_sec);
}

size_t clamp_size(double v, size_t lo, size_t hi) noexcept {
    if (v < static_cast<double>(lo)) {
        return lo;
    }
    if (v > static_cast<double>(hi)) {
        return hi;
    }
    return static_cast<size_t>(v);
}

uint32_t next_pow2(uint32_t v) noexcept {
    if (v == 0) {
        return 1;
    }
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

bool allocate_state() noexcept {
    PluginContext& c = ctx();
    const size_t client_cap = clamp_size(cvar_f(c.cvars.client_ring, 300.0f), 1, 65536);
    const size_t server_cap = clamp_size(cvar_f(c.cvars.server_ring, 3600.0f), 1, 262144);
    if (!c.server_ring.init(server_cap)) {
        return false;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!c.clients[i].ring.init(client_cap)) {
            return false;
        }
    }
    uint32_t ipn = static_cast<uint32_t>(clamp_size(cvar_f(c.cvars.ip_table, 4096.0f), 16u, 1u << 20));
    ipn = next_pow2(ipn);
    c.ip_table = new (std::nothrow) IpEntry[ipn];
    if (c.ip_table == nullptr) {
        return false;
    }
    c.ip_table_size = ipn;
    std::memset(c.ip_table, 0, static_cast<size_t>(ipn) * sizeof(IpEntry));
    return true;
}

void release_state() noexcept {
    PluginContext& c = ctx();
    if (c.attached) {
        unregister_hooks();
        c.attached = false;
    }
    c.server_ring.init(0);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        c.clients[i].ring.init(0);
    }
    delete[] c.ip_table;
    c.ip_table = nullptr;
    c.ip_table_size = 0;
    delete c.log;
    c.log = nullptr;
    c.layout_ok = false;
    c.layout_checked = 0;
}

bool resolve_api() noexcept {
    PluginContext& c = ctx();
    CSysModule* engine = Sys_GetModuleHandle("engine_i486.so");
    if (engine == nullptr) {
        util_log("engine_i486.so not found; ReHLDS is required");
        return false;
    }
    CreateInterfaceFn factory = Sys_GetFactory(engine);
    if (factory == nullptr) {
        util_log("engine CreateInterface export not found");
        return false;
    }
    int rc = 0;
    IRehldsApi* api = reinterpret_cast<IRehldsApi*>(factory(VREHLDS_HLDS_API_VERSION, &rc));
    if (api == nullptr) {
        util_log("IRehldsApi unavailable; ReHLDS version too old");
        return false;
    }
    // API 3.14 and 3.15 differ only by the trailing SetServerPause func we do
    // not use; the hookchain vtable is identical, so 3.14 is compatible.
    constexpr int kMinApiMinor = 14;
    if (api->GetMajorVersion() != REHLDS_API_VERSION_MAJOR ||
        api->GetMinorVersion() < kMinApiMinor) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "ReHLDS API mismatch: server %d.%d, need %d.%d+",
                      api->GetMajorVersion(), api->GetMinorVersion(),
                      REHLDS_API_VERSION_MAJOR, kMinApiMinor);
        util_log(msg);
        return false;
    }
    c.api = api;
    c.funcs = api->GetFuncs();
    c.hookchains = api->GetHookchains();
    c.svs = api->GetServerStatic();
    c.data = api->GetServerData();
    if (c.funcs == nullptr || c.hookchains == nullptr || c.svs == nullptr) {
        util_log("ReHLDS API returned null tables");
        return false;
    }
    g_RehldsSvs = c.svs;
    c.maxclients = c.svs->GetMaxClients();
    if (c.maxclients <= 0 || c.maxclients > MAX_CLIENTS) {
        c.maxclients = MAX_CLIENTS;
    }
    return true;
}

void init_modnet() noexcept {
    PluginContext& c = ctx();
    const char* targets = "";
    if (c.cvars.modnet_targets != nullptr && c.cvars.modnet_targets->string != nullptr) {
        targets = c.cvars.modnet_targets->string;
    }
    const bool using_builtin = (targets[0] == '\0');
    modnet_got_configure_targets(targets);
    if (using_builtin) {
        util_log("modnet: hldsmon_modnet_targets is empty; using built-in target list");
    }
    // Enable only when the master-switch is on; when OFF neither the probe nor
    // any GOT slot is touched, even transiently (QAS-M6, MN-5).
    if (cvar_f(c.cvars.modnet, 0.0f) > 0.5f) {
        modnet_enable();
    }
    c.modnet = modnet_got_status();
    c.modnet.next_rescan_t = 0.0;
}

bool open_log() noexcept {
    PluginContext& c = ctx();
    c.log = new (std::nothrow) LogWriter();
    if (c.log == nullptr) {
        util_log("LogWriter allocation failed");
        return false;
    }
    // Route the log file's raw fd to the crash handler before the first open.
    set_log_fd_observer(&platform_set_crash_log_fd);
    c.log->set_max_mb(static_cast<uint64_t>(cvar_f(c.cvars.max_mb, 512.0f)));
    c.log->set_retention_days(static_cast<int>(cvar_f(c.cvars.retention_days, 3.0f)));

    const char* dir = "addons/hldsmon/logs";
    if (c.cvars.dir != nullptr && c.cvars.dir->string != nullptr &&
        c.cvars.dir->string[0] != '\0') {
        dir = c.cvars.dir->string;
    }

    char game_dir[512];
    game_dir[0] = '\0';
    if (g_engfuncs.pfnGetGameDir != nullptr) {
        g_engfuncs.pfnGetGameDir(game_dir);
    }
    char resolved[1024];
    const char* use = dir;
    if (dir[0] != '/' && game_dir[0] != '\0') {
        const int rn = std::snprintf(resolved, sizeof(resolved), "%s/%s", game_dir, dir);
        if (rn > 0 && static_cast<size_t>(rn) < sizeof(resolved)) {
            use = resolved;
        }
    }

    const double now = platform_wall_seconds();
    const char* effective = use;
    if (!c.log->open(use, now)) {
        util_log("log directory unusable; falling back to /tmp/hldsmon");
        effective = "/tmp/hldsmon";
        if (!c.log->open("/tmp/hldsmon", now)) {
            util_log("fallback log directory unusable; writing disabled");
        }
    }
    platform_install_crash_handler(effective);
    return true;
}

} // namespace

bool plugin_init() noexcept {
    PluginContext& c = ctx();
    if (c.attached) {
        return true;
    }
    if (!resolve_api()) {
        return false;
    }
    register_cvars();
    if (!allocate_state()) {
        util_log("state allocation failed");
        release_state();
        return false;
    }

    const LayoutProbeResult pr = run_layout_probe(c.svs, c.api);
    c.layout_checked = pr.checked;
    if (!pr.ok) {
        util_log("engine layout probe failed; refusing to load (fail-closed)");
        release_state();
        return false;
    }
    c.layout_ok = true;

    open_log();
    register_control_commands();
    init_modnet();

    c.attached = true;
    if (!register_hooks()) {
        util_log("hook registration failed; collector degraded");
    }
    return true;
}

void plugin_shutdown() noexcept {
    PluginContext& c = ctx();
    modnet_disable();
    if (c.log != nullptr) {
        c.log->flush();
    }
    release_state();
}

} // namespace hldsmon

// ---- Metamod entry points ----

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine,
                                        globalvars_t* pGlobals) {
    std::memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    ::gpGlobals = pGlobals;
}

C_DLLEXPORT int Meta_Query(char* /*interfaceVersion*/,
                           plugin_info_t** plinfo,
                           mutil_funcs_t* pMetaUtilFuncs) {
    *plinfo = &Plugin_info;
    gpMetaUtilFuncs = pMetaUtilFuncs;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME /*now*/,
                            META_FUNCTIONS* pFunctionTable,
                            meta_globals_t* pMGlobals,
                            gamedll_funcs_t* pGamedllFuncs) {
    gpMetaGlobals = pMGlobals;
    gpGamedllFuncs = pGamedllFuncs;

    if (!hldsmon::plugin_init()) {
        return FALSE;
    }

    std::memset(&gMetaFunctionTable, 0, sizeof(gMetaFunctionTable));
    gMetaFunctionTable.pfnGetEntityAPI2 = GetEntityAPI2;
    gMetaFunctionTable.pfnGetEntityAPI2_Post = GetEntityAPI2_Post;
    gMetaFunctionTable.pfnGetNewDLLFunctions = GetNewDLLFunctions;

    std::memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME /*now*/, PL_UNLOAD_REASON /*reason*/) {
    hldsmon::plugin_shutdown();
    return TRUE;
}

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS* /*pFunctionTable*/, int* interfaceVersion) {
    if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
    return FALSE;
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS* /*pFunctionTable*/, int* interfaceVersion) {
    if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
    return FALSE;
}

// Reliable graceful-shutdown hook (NEW_DLL_FUNCTIONS::pfnGameShutdown). Metamod
// composes and calls it on `quit`/`exit` and on SIGTERM/SIGINT, before the game
// DLL's shutdown, while the log is still open. Safe if log == nullptr.
void hldsmon_game_shutdown() {
    using namespace hldsmon;
    PluginContext& c = ctx();
    platform_mark_graceful_shutdown();
    if (c.attached && plugin_enabled()) {
        ShutdownRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = platform_wall_seconds();
        copy_str(r.reason, sizeof(r.reason), "graceful");
        events::write_shutdown(r);
    }
    if (c.log != nullptr) {
        c.log->flush();
    }
}

C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS* pNewFunctionTable, int* interfaceVersion) {
    if (pNewFunctionTable == nullptr) {
        return FALSE;
    }
    std::memset(pNewFunctionTable, 0, sizeof(NEW_DLL_FUNCTIONS));
    pNewFunctionTable->pfnGameShutdown = &hldsmon_game_shutdown;
    if (interfaceVersion != nullptr) {
        *interfaceVersion = NEW_DLL_FUNCTIONS_VERSION;
    }
    return TRUE;
}

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t* /*pengfuncsFromEngine*/, int* interfaceVersion) {
    if (interfaceVersion) *interfaceVersion = ENGINE_INTERFACE_VERSION;
    return FALSE;
}

C_DLLEXPORT int GetEngineFunctions_Post(enginefuncs_t* /*pengfuncsFromEngine*/, int* interfaceVersion) {
    if (interfaceVersion) *interfaceVersion = ENGINE_INTERFACE_VERSION;
    return FALSE;
}
