#include "Hook/LicenseListLogic.h"

#include "steam_messages.pb.h"

#include <cstdint>
#include <limits>

namespace LicenseListLogic {

bool ParseOwnedLicenses(const uint8_t* body, uint32_t size, std::vector<LicenseEntry>& out) {
    // protobuf's ParseFromArray takes a signed int length; reject anything that
    // would not round-trip rather than let it truncate or go negative.
    if (!body || size > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return false;

    CMsgClientLicenseList msg;
    if (!msg.ParseFromArray(body, static_cast<int>(size)))
        return false;

    std::vector<LicenseEntry> licenses;
    licenses.reserve(static_cast<size_t>(msg.licenses_size()));
    for (int i = 0; i < msg.licenses_size(); ++i) {
        const auto& lic = msg.licenses(i);
        if (!lic.has_package_id()) continue;
        licenses.push_back({lic.package_id(), lic.has_access_token() ? lic.access_token() : 0});
    }

    out = std::move(licenses);
    return true;
}

} // namespace LicenseListLogic
