#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

// Pure policy behind PatternLoader's self-healing cache refresh (v1.3.4),
// split out (header-only) so it is unit-testable without WinHTTP, the
// filesystem, the TOML parser or a live module -- same pattern as
// RemoteTomlLogic.h / PatternLoaderLogic.h.
//
// Problem: a pattern TOML is cached under the DLL's SHA-256 and the cache is
// authoritative (cache-first, no per-launch request -- see RemoteToml::Fetch).
// If a mirror later FIXES the TOML for the same SHA (adds a missing function,
// corrects a signature), the client never sees it; the user had to delete
// <Steam>\amethysttool\pattern by hand.
//
// Policy: revalidate a cached TOML against the mirror chain ONLY when it is
// demonstrably broken -- a *required* function from that module failed to
// resolve during hook install (the user is already looking at the "Missing
// Signatures" popup) -- and at most once per kMinRevalidateInterval. A healthy
// cache never causes a request, so the cache-first privacy/offline property is
// preserved for the normal case. The download only replaces the cache when it
// is strictly better (see ShouldReplaceCache).
namespace PatternRefreshLogic {

    // At most one revalidation attempt per cached file per interval, tracked by
    // a sidecar stamp file's mtime (see StampFileName). Survives restarts.
    inline constexpr std::chrono::hours kMinRevalidateInterval{ 24 };

    // Short WinHTTP timeouts for the background revalidation: it is a
    // best-effort improvement, never worth a long wait. resolve/connect/send
    // use kRevalidateTimeoutMs, recv uses kRevalidateRecvTimeoutMs.
    inline constexpr uint32_t kRevalidateTimeoutMs     = 2000;
    inline constexpr uint32_t kRevalidateRecvTimeoutMs = 3000;

    inline constexpr std::string_view kStampSuffix = ".checked";

    enum class Decision : uint8_t {
        Disabled,          // [remote] revalidate = false
        NotFromCache,      // body came from the mirror this launch: already current
        CacheFallback,     // the network already failed this launch
        NoRequiredMisses,  // cache is healthy: never touch the network
        Throttled,         // attempted less than minInterval ago
        Revalidate,
    };

    struct RevalidateInputs {
        bool   enabled        = true;
        bool   fromCache      = false;
        bool   cacheFallback  = false;
        size_t requiredMisses = 0;
        // Time since the last attempt (stamp file mtime). nullopt = never
        // attempted / stamp unreadable.
        std::optional<std::chrono::seconds> sinceLastAttempt;
    };

    [[nodiscard]] constexpr Decision ShouldRevalidate(
        const RevalidateInputs& in,
        std::chrono::seconds minInterval = kMinRevalidateInterval) noexcept
    {
        if (!in.enabled)            return Decision::Disabled;
        if (!in.fromCache)          return Decision::NotFromCache;
        if (in.cacheFallback)       return Decision::CacheFallback;
        if (in.requiredMisses == 0) return Decision::NoRequiredMisses;
        // A negative age means the stamp is in the future (clock moved back):
        // treat it as stale rather than throttling forever.
        if (in.sinceLastAttempt &&
            *in.sinceLastAttempt >= std::chrono::seconds::zero() &&
            *in.sinceLastAttempt < minInterval)
            return Decision::Throttled;
        return Decision::Revalidate;
    }

    [[nodiscard]] constexpr const char* ToString(Decision d) noexcept
    {
        switch (d) {
        case Decision::Disabled:         return "disabled";
        case Decision::NotFromCache:     return "not from cache";
        case Decision::CacheFallback:    return "cache fallback";
        case Decision::NoRequiredMisses: return "no required misses";
        case Decision::Throttled:        return "throttled";
        case Decision::Revalidate:       return "revalidate";
        }
        return "?";
    }

    // Whether a freshly downloaded TOML should replace the cached one. All of:
    //   * the body actually differs (an identical body changes nothing);
    //   * it parsed to at least one entry (an empty/garbage reply never wins);
    //   * it keeps every function key the cached file had (never trade a
    //     working hook for a fixed one -- a truncated or regressed mirror
    //     reply is refused);
    //   * it resolves at least one function that is missing right now
    //     (measured against the live module by the caller).
    [[nodiscard]] inline bool ShouldReplaceCache(const std::unordered_set<uint32_t>& cachedKeys,
                                                 const std::unordered_set<uint32_t>& remoteKeys,
                                                 bool bodyDiffers,
                                                 size_t missesResolvedByRemote)
    {
        if (!bodyDiffers || remoteKeys.empty() || missesResolvedByRemote == 0)
            return false;
        for (uint32_t key : cachedKeys)
            if (remoteKeys.count(key) == 0)
                return false;
        return true;
    }

    // Sidecar stamp next to "<sha256>.toml": "<sha256>.toml.checked".
    [[nodiscard]] inline std::string StampFileName(std::string_view sha256)
    {
        std::string name(sha256);
        name += ".toml";
        name += kStampSuffix;
        return name;
    }

} // namespace PatternRefreshLogic
