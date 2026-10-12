// HldsMonitoring — host test for the config snapshot POD and diff helper
// (config-context stage, CFG-P0: sizes, presence bitmask, equality).

#include "metrics.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

static constexpr uint16_t kAllBits = 0x1FFu;

static ConfigSnapshot make_full(float base) {
    ConfigSnapshot s;
    std::memset(&s, 0, sizeof s);
    s.present = kAllBits;
    s.sys_ticrate = base + 1.0f;
    s.host_framerate = base + 2.0f;
    s.sv_timeout = base + 3.0f;
    s.sv_maxrate = base + 4.0f;
    s.sv_minrate = base + 5.0f;
    s.sv_maxupdaterate = base + 6.0f;
    s.sv_minupdaterate = base + 7.0f;
    s.sv_maxcmdrate = base + 8.0f;
    s.sv_mincmdrate = base + 9.0f;
    return s;
}

int main() {
    using Field = float ConfigSnapshot::*;
    static constexpr Field kFields[] = {
        &ConfigSnapshot::sys_ticrate,
        &ConfigSnapshot::host_framerate,
        &ConfigSnapshot::sv_timeout,
        &ConfigSnapshot::sv_maxrate,
        &ConfigSnapshot::sv_minrate,
        &ConfigSnapshot::sv_maxupdaterate,
        &ConfigSnapshot::sv_minupdaterate,
        &ConfigSnapshot::sv_maxcmdrate,
        &ConfigSnapshot::sv_mincmdrate,
    };

    // ---- sizes (same numbers pinned by metrics.h static_assert) ----
    CHECK(sizeof(ConfigSnapshot) == 38);
    CHECK(sizeof(ConfigRecord) == 119);
    CHECK(sizeof(DropRecord) == 16872);

    // ---- present bitmask ----
    {
        uint16_t acc = 0;
        const uint16_t bits[] = {
            kCfgSysTicrate, kCfgHostFramerate, kCfgSvTimeout,
            kCfgSvMaxrate, kCfgSvMinrate, kCfgSvMaxupdaterate,
            kCfgSvMinupdaterate, kCfgSvMaxcmdrate, kCfgSvMincmdrate,
        };
        for (uint16_t bit : bits) {
            CHECK(bit != 0);
            CHECK((acc & bit) == 0);   // each bit unique
            acc |= bit;
        }
        CHECK(acc == kAllBits);
        CHECK(sizeof bits / sizeof bits[0] == 9);   // one bit per tracked cvar
    }

    // ---- equality / inequality ----
    {
        ConfigSnapshot a;
        ConfigSnapshot b;
        std::memset(&a, 0, sizeof a);
        std::memset(&b, 0, sizeof b);
        CHECK(!config_changed(a, b));   // two memset-zeroed snapshots are equal

        const ConfigSnapshot full = make_full(10.0f);
        CHECK(!config_changed(full, make_full(10.0f)));
        CHECK(config_changed(full, make_full(20.0f)));

        // Presence mask alone is a change, even when the value slots agree.
        a.present = kCfgSvTimeout;
        a.sv_timeout = 0.0f;
        CHECK(config_changed(a, b));
        b.present = kCfgSvTimeout;
        CHECK(!config_changed(a, b));

        // Every tracked value participates in the comparison.
        for (Field field : kFields) {
            ConfigSnapshot mutated = full;
            mutated.*field += 1.0f;
            CHECK(config_changed(full, mutated));
        }

        // A stale value behind a cleared presence bit still counts as a change.
        ConfigSnapshot stale = full;
        stale.present &= static_cast<uint16_t>(~kCfgSvTimeout);
        CHECK(config_changed(full, stale));
    }

    // ---- record layout sanity ----
    {
        ConfigRecord r;
        std::memset(&r, 0, sizeof r);
        r.rehlds_major = 3;
        r.rehlds_minor = 14;
        r.maxclients = 32;
        std::strcpy(r.map, "de_dust2");
        r.config = make_full(0.0f);
        CHECK(r.config.present == kAllBits);
        CHECK(r.maxclients == 32);
        CHECK(std::strcmp(r.map, "de_dust2") == 0);
    }

    if (g_fail == 0) std::printf("test_config: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
