#pragma once

#include "Utils/Config/Config.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Pure [donate] TOML parsing extracted from Config.cpp, so it compiles here
// (see src/tests/CMakeLists.txt) without pulling in Lua/LuaConfig, WinHTTP,
// or spdlog the way the rest of Utils/Config does. Same split rationale as
// ConfigMigration.cpp next to it.
namespace DonateConfigLogic {

    // One numeric key that was present in the TOML but outside its documented
    // [lo, hi] range -- the existing value in `out` was left unchanged rather
    // than clamped or silently accepted (a mistyped 0 here would otherwise
    // mean an unthrottled loop hammering Steam as the signed-in user).
    // Config.cpp logs these via LOG_WARN; tests can assert on the list
    // directly without a logging dependency.
    struct RangeViolation {
        std::string key;
        int64_t     value = 0;
        uint32_t    lo = 0;
        uint32_t    hi = 0;
        uint32_t    kept = 0;   // the value in `out` after this key was rejected
    };

    // Applies every recognized key in `donateTable` onto `out` in place.
    // - "enabled" (bool) and "url" (string) are copied as-is when present.
    // - The five numeric keys (interval_secs, max_mints_per_cycle,
    //   min_mint_interval_ms, max_mints_per_session, wanted_refresh_secs) are
    //   clamped to the same [lo, hi] ranges as upstream BetterSteamTools;
    //   in-range values overwrite `out`, out-of-range values are reported
    //   in the returned list and `out` keeps whatever it already had.
    // - A key absent from `donateTable` leaves the corresponding field in
    //   `out` untouched -- callers should start from a `Config::DonateSettings{}`
    //   (all safe defaults, enabled=false) rather than a partially-built one.
    [[nodiscard]] std::vector<RangeViolation> Apply(const toml::table& donateTable,
                                                     Config::DonateSettings& out);

}
