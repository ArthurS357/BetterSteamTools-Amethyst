#include "Utils/Config/DonateConfigLogic.h"

namespace DonateConfigLogic {
namespace {

    // Clamp bounds for [donate]'s numeric settings -- named so a bound and
    // its meaning travel together, and a future change to one has exactly
    // one place to edit. Values match upstream BetterSteamTools' own ranges.
    constexpr uint32_t kIntervalSecsMin = 30,        kIntervalSecsMax = 86400;    // 24h
    constexpr uint32_t kMaxMintsPerCycleMin = 1,     kMaxMintsPerCycleMax = 500;
    constexpr uint32_t kMinMintIntervalMsMin = 0,    kMinMintIntervalMsMax = 60000; // 60s
    constexpr uint32_t kMaxMintsPerSessionMin = 0,   kMaxMintsPerSessionMax = 100000;
    constexpr uint32_t kWantedRefreshSecsMin = 30,   kWantedRefreshSecsMax = 86400; // 24h

}

    std::vector<RangeViolation> Apply(const toml::table& donateTable, Config::DonateSettings& out) {
        std::vector<RangeViolation> violations;

        if (auto v = donateTable["enabled"].value<bool>())        out.enabled = *v;
        if (auto v = donateTable["url"].value<std::string>())     out.url = *v;

        // Clamped rather than trusted: a mistyped 0 here would mean an
        // unthrottled loop hammering Steam as the signed-in user.
        auto readClamped = [&](const char* key, uint32_t lo, uint32_t hi, uint32_t& field) {
            auto v = donateTable[key].value<int64_t>();
            if (!v) return;
            if (*v < lo || *v > hi) {
                violations.push_back(RangeViolation{key, *v, lo, hi, field});
                return;
            }
            field = static_cast<uint32_t>(*v);
        };
        readClamped("interval_secs",        kIntervalSecsMin,        kIntervalSecsMax,        out.intervalSecs);
        readClamped("max_mints_per_cycle",   kMaxMintsPerCycleMin,   kMaxMintsPerCycleMax,     out.maxMintsPerCycle);
        readClamped("min_mint_interval_ms",  kMinMintIntervalMsMin,  kMinMintIntervalMsMax,    out.minMintIntervalMs);
        readClamped("max_mints_per_session", kMaxMintsPerSessionMin, kMaxMintsPerSessionMax,   out.maxMintsPerSession);
        readClamped("wanted_refresh_secs",   kWantedRefreshSecsMin,  kWantedRefreshSecsMax,    out.wantedRefreshSecs);

        return violations;
    }

}
