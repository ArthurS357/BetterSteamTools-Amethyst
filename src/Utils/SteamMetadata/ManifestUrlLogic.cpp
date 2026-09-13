#include "Utils/SteamMetadata/ManifestUrlLogic.h"

#include <cstdio>

namespace ManifestUrlLogic {
namespace {

    void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

} // namespace

Choice Build(std::string_view overrideTemplate,
             const char* providerTemplate,
             const char* providerTemplateEx,
             AppId_t appId, AppId_t depotId, uint64_t gid)
{
    if (!overrideTemplate.empty()) {
        // Built directly into a std::string -- no fixed-size buffer here. A
        // custom endpoint URL a user typed has no reason to be bounded the
        // way the compile-time provider literals below are; routing it
        // through a 256-byte snprintf would only add a truncation risk with
        // no corresponding benefit.
        std::string url(overrideTemplate);
        ReplaceAll(url, "{appid}", std::to_string(appId));
        ReplaceAll(url, "{depotid}", std::to_string(depotId));
        ReplaceAll(url, "{gid}", std::to_string(gid));
        return Choice{std::move(url), Shape::Override, false};
    }

    const bool depotAware = providerTemplateEx != nullptr && depotId != 0;

    char buf[kMaxProviderUrlLen];
    const int written = depotAware
        ? std::snprintf(buf, sizeof(buf), providerTemplateEx, appId, depotId, gid)
        : std::snprintf(buf, sizeof(buf), providerTemplate, gid);

    const bool truncated = written < 0 || static_cast<std::size_t>(written) >= sizeof(buf);
    const std::size_t len = truncated ? sizeof(buf) - 1
                                       : static_cast<std::size_t>(written < 0 ? 0 : written);
    return Choice{
        std::string(buf, len),
        depotAware ? Shape::DepotAware : Shape::GidOnly,
        truncated,
    };
}

} // namespace ManifestUrlLogic
