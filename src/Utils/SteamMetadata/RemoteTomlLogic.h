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
    // Order is deliberate.
    //
    // Repo priority (v1.3.2): madoiscool/steam-monitor before
    // OpenSteam001/steam-monitor. madoiscool is the actively maintained tracker
    // -- every stable/beta build within hours, patterns (e.g.
    // YldLoadDepotManifest, since 2026-09-19) the other repo never got.
    // OpenSteam001's `pattern` branch has had no commit since 2026-09-06; when
    // both hold the same hash the first one wins, so it stays last, as a fallback.
    //
    // Host priority (v1.5.0, from upstream v1.0.9): within a repo, jsDelivr is
    // tried before raw.githubusercontent.com. Where GitHub raw is unreachable it
    // fails with HTTP=0 only after the connect timeout, and on a cache miss that
    // runs before hooks install -- the window in which Steam can build a depot
    // config unhooked ("invalid content configuration"). git.lua.tools sits
    // between them for madoiscool, as upstream does.
    // Revisit this order if the maintenance situation of either repo changes.
    inline constexpr std::array<std::string_view, 5> kDefaultTemplates = {
        "https://cdn.jsdelivr.net/gh/madoiscool/steam-monitor@{channel}/{component}/{sha256}.toml",
        "https://git.lua.tools/luatools/steam-monitor/raw/branch/{channel}/{component}/{sha256}.toml",
        "https://raw.githubusercontent.com/madoiscool/steam-monitor/{channel}/{component}/{sha256}.toml",
        "https://cdn.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml",
        "https://raw.githubusercontent.com/OpenSteam001/steam-monitor/{channel}/{component}/{sha256}.toml",
    };

    // WinHTTP timeouts for the cache-miss walk (v1.5.0, from upstream v1.0.9).
    // It runs on the init thread before hooks install, so a dead mirror must
    // fail in seconds instead of the OSTPlatform::Http defaults (5 s resolve,
    // 5 s connect, 10 s send/recv). resolve/connect use kMissTimeoutMs,
    // send/recv use kMissRecvTimeoutMs.
    inline constexpr uint32_t kMissTimeoutMs     = 2500;
    inline constexpr uint32_t kMissRecvTimeoutMs = 5000;

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

    // The cache-miss walk behind RemoteToml::FetchOnMiss: tries `urls` in order
    // with the kMiss* timeouts and stops at the first hit. The HTTP call is
    // injected so tests can fake a dead host and check the timeouts it gets:
    //   get(url, resolveMs, connectMs, sendMs, recvMs) -> R, R has .ok and .status
    // Returns the last attempt's result (default R when `urls` is empty) and sets
    // lastUrl to the last URL tried.
    template <class R, class Get>
    [[nodiscard]] R WalkMissMirrors(const std::vector<std::string>& urls, Get&& get, std::string& lastUrl)
    {
        R result{};
        for (const std::string& url : urls) {
            lastUrl = url;
            result = get(url, kMissTimeoutMs, kMissTimeoutMs, kMissRecvTimeoutMs, kMissRecvTimeoutMs);
            if (IsMirrorHit(result.ok, result.status)) break;
        }
        return result;
    }

    // M-4 (v1.3.4 audit): the background revalidation may replace an already
    // trusted cache, so it only talks to https:// mirrors. The scheme compare is
    // ASCII case-insensitive (RFC 3986 3.1). Only that path applies this check;
    // the cache-miss walk keeps accepting whatever [remote] url_template says.
    [[nodiscard]] constexpr bool IsHttpsUrl(std::string_view url) noexcept
    {
        constexpr std::string_view kScheme = "https://";
        if (url.size() < kScheme.size()) return false;
        for (std::size_t i = 0; i < kScheme.size(); ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != kScheme[i]) return false;
        }
        return true;
    }

    // Log label for where a fetched TOML came from (RemoteToml::Result flags):
    //   !fromCache                     -> "remote"          (cache miss, network hit)
    //   fromCache && !cacheFallback    -> "cache hit"       (normal cache-first path)
    //   fromCache && cacheFallback     -> "cache fallback"  (network failed, cache as plan B)
    // cacheFallback without fromCache is inconsistent input; it is still "remote".
    [[nodiscard]] constexpr const char* DescribeLoadSource(bool fromCache, bool cacheFallback) noexcept
    {
        if (!fromCache) return "remote";
        return cacheFallback ? "cache fallback" : "cache hit";
    }

} // namespace RemoteTomlLogic
