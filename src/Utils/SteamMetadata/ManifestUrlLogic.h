#pragma once

#include "Steam/Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Pure URL-selection/construction logic behind ManifestClient::FetchActive,
// extracted so it is unit-testable without WinHTTP, Config, or LuaConfig
// (see src/tests/CMakeLists.txt). No network call happens here -- this only
// decides which URL to fetch and builds it.
namespace ManifestUrlLogic {

    enum class Shape : std::uint8_t {
        Override,    // [manifest] url_template took precedence over everything below
        DepotAware,  // provider's app/depot/gid template (post-2026-09-09 MRC)
        GidOnly,     // provider's plain gid template (fallback / carrier-only)
    };

    struct Choice {
        std::string url;
        Shape       shape;
        // True if a provider's printf-style template (DepotAware/GidOnly only --
        // Override is built into an unbounded std::string and never truncates,
        // see Build() below) needed more than kMaxProviderUrlLen bytes; `url`
        // is truncated in that case. Never expected in practice -- the 3
        // built-in provider templates are short fixed literals -- kept as a
        // defensive signal rather than a silent corruption.
        bool        truncated = false;
    };

    // Bound for the printf-style provider templates only; matches the fixed
    // buffer size this logic replaces in ManifestClient.cpp.
    inline constexpr std::size_t kMaxProviderUrlLen = 256;

    // Decides and builds the URL to fetch a manifest request code from.
    //
    //  - overrideTemplate: [manifest] url_template (empty = none set). Wins
    //    UNCONDITIONALLY over the provider tables when non-empty -- this is
    //    the "point requests at your own server" escape hatch and must
    //    short-circuit provider selection entirely, never silently falling
    //    back to a built-in provider. Placeholders substituted: {appid},
    //    {depotid}, {gid}. {appid} and {depotid} are OPTIONAL -- a template
    //    using only {gid} (written before this option existed) is unaffected,
    //    so no existing override needs migrating.
    //
    //  - providerTemplate: the active provider's one-%llu-gid printf template
    //    (every provider has one -- the pre-MRC shape all 3 still support).
    //  - providerTemplateEx: the active provider's optional app/depot/gid
    //    printf template ("%u/%u/%llu" argument order: appid, depotid, gid).
    //    nullptr = this provider has none (wudrm, steamrun expose no
    //    depot-aware route).
    //
    //  - depotId == 0 means "unknown depot" and forces GidOnly even when
    //    providerTemplateEx is set: a depot-aware URL naming depot 0 would be
    //    meaningless, and Valve's 2026-09-09 change only requires depot
    //    awareness for a *specific* depot's CDN authorization.
    [[nodiscard]] Choice Build(std::string_view overrideTemplate,
                               const char* providerTemplate,
                               const char* providerTemplateEx,
                               AppId_t appId, AppId_t depotId, uint64_t gid);

}
