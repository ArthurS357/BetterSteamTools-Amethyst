#include "Utils/SteamMetadata/ManifestDonorLogic.h"

#include <charconv>

namespace ManifestDonorLogic {

    bool ParseWantedLine(std::string_view line, WantedEntry& out) {
        // Deliberate hardening over the upstream original this was ported
        // from: from_chars alone only reports whether a valid number
        // appeared at the START of the field, not whether it consumed the
        // whole thing -- "123:456" would parse as 123 and silently drop the
        // rest. Checking ptr == end closes that gap for a value coming
        // straight off the network.
        auto field = [](std::string_view s, uint64_t& v) {
            if (s.empty()) return false;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            return ec == std::errc{} && ptr == s.data() + s.size();
        };

        const size_t c1 = line.find(':');
        if (c1 == std::string_view::npos) return false;
        const size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string_view::npos) return false;

        uint64_t app = 0, depot = 0, gid = 0;
        if (!field(line.substr(0, c1), app))                return false;
        if (!field(line.substr(c1 + 1, c2 - c1 - 1), depot)) return false;
        if (!field(line.substr(c2 + 1), gid))                return false;
        if (!depot || !gid) return false;    // app_id 0 is valid; these are not

        out.appId   = static_cast<AppId_t>(app);
        out.depotId = static_cast<uint32_t>(depot);
        out.gid     = gid;
        return true;
    }

}
