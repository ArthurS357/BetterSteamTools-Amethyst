#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure mirror-chain logic behind RemoteToml::Fetch, split out (header-only) so
// the URL order and the fall-through policy are unit-testable without WinHTTP,
// Config or the filesystem -- same pattern as Hook/ManifestHookLogic.h.
namespace RemoteTomlLogic {

    // Built-in mirror chain, tried in order. Used when [remote] url_template is
    // unset. Mirrors are independent repos/hosts (not just CDN copies of one
    // repo), so their coverage differs.
    //
    // Order is deliberate (v1.3.2): madoiscool/steam-monitor is first because it
    // is the actively maintained tracker -- it publishes every stable/beta build
    // within hours and carries patterns (e.g. YldLoadDepotManifest, since
    // 2026-09-19) the other repo never got. OpenSteam001/steam-monitor's
    // `pattern` branch has had no commit since 2026-09-06; when both mirrors hold
    // the same hash the first one wins, so keeping the stale repo first served
    // its older, pattern-poorer TOML. It stays in the chain as a fallback.
    // Revisit this order if the maintenance situation of either repo changes.
    inline constexpr std::array<std::string_view, 5> kDefaultTemplates = {
        "https://raw.githubusercontent.com/madoiscool/steam-monitor/{channel}/{component}/{sha256}.toml",
        "https://cdn.jsdelivr.net/gh/madoiscool/steam-monitor@{channel}/{component}/{sha256}.toml",
        "https://raw.githubusercontent.com/OpenSteam001/steam-monitor/{channel}/{component}/{sha256}.toml",
        "https://cdn.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml",
        "https://git.lua.tools/luatools/steam-monitor/raw/branch/{channel}/{component}/{sha256}.toml",
    };

    [[nodiscard]] inline bool IsValidTemplate(std::string_view urlTemplate) noexcept
    {
        return urlTemplate.find("{channel}") != std::string_view::npos &&
               urlTemplate.find("{component}") != std::string_view::npos &&
               urlTemplate.find("{sha256}") != std::string_view::npos;
    }

    inline void ReplaceAll(std::string& text, std::string_view from, std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    [[nodiscard]] inline std::string ExpandTemplate(std::string urlTemplate,
                                                    std::string_view channel,
                                                    std::string_view component,
                                                    std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", channel);
        ReplaceAll(urlTemplate, "{component}", component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    // The URLs RemoteToml::Fetch tries, in order.
    [[nodiscard]] inline std::vector<std::string> BuildMirrorUrls(const std::vector<std::string>& templates,
                                                                  std::string_view channel,
                                                                  std::string_view component,
                                                                  std::string_view sha256)
    {
        std::vector<std::string> urls;
        urls.reserve(templates.size());
        for (const std::string& t : templates)
            urls.push_back(ExpandTemplate(t, channel, component, sha256));
        return urls;
    }

    // Only a 200 stops the chain. A 404 (or any other failure) on one mirror
    // never implies the others lack the file -- they are independent repos.
    inline constexpr uint32_t kHttpOk = 200;

    [[nodiscard]] constexpr bool IsMirrorHit(bool transportOk, uint32_t status) noexcept
    {
        return transportOk && status == kHttpOk;
    }

} // namespace RemoteTomlLogic
