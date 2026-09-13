#include "Utils/Config/DonateConfigLogic.h"

namespace DonateConfigLogic {

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
        readClamped("interval_secs",        30,  86400, out.intervalSecs);
        readClamped("max_mints_per_cycle",   1,    500, out.maxMintsPerCycle);
        readClamped("min_mint_interval_ms",  0,  60000, out.minMintIntervalMs);
        readClamped("max_mints_per_session", 0, 100000, out.maxMintsPerSession);
        readClamped("wanted_refresh_secs",  30,  86400, out.wantedRefreshSecs);

        return violations;
    }

}
