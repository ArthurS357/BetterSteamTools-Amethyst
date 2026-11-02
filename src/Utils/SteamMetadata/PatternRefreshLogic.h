#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

    // Timeouts for re-downloading a cached TOML that no longer parses. That path
    // is a cache-miss equivalent on the init thread, so it uses the same values
    // as RemoteToml::Fetch's cache-miss walk (OSTPlatform::Http defaults).
    inline constexpr uint32_t kCorruptCacheRefetchTimeoutMs     = 5000;
    inline constexpr uint32_t kCorruptCacheRefetchRecvTimeoutMs = 10000;

    // One pattern-file entry, keyed in EntryMap by Fnv1aHash(function name) --
    // the same layout PatternLoader parses the TOML into.
    struct PatternEntry {
        std::string    name;
        std::uintptr_t rva = 0;   // 0 = not present in file
        std::string    sig;       // empty = not present in file
    };
    using EntryMap = std::unordered_map<uint32_t, PatternEntry>;

    [[nodiscard]] inline bool SameEntry(const PatternEntry& a, const PatternEntry& b) noexcept
    {
        return a.rva == b.rva && a.name == b.name && a.sig == b.sig;
    }

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
    [[nodiscard]] inline bool ShouldReplaceCache(const std::unordered_set<uint32_t>& existingKeys,
                                                 const std::unordered_set<uint32_t>& candidateKeys,
                                                 bool bodyDiffers,
                                                 size_t missesResolvedByCandidate)
    {
        if (!bodyDiffers || candidateKeys.empty() || missesResolvedByCandidate == 0)
            return false;
        for (uint32_t key : existingKeys)
            if (candidateKeys.count(key) == 0)
                return false;
        return true;
    }

    // H-1 (v1.3.4 audit): a candidate TOML may only ADD or FIX the functions
    // that are missing right now; every other entry the cache already had must
    // be byte-identical (name + rva + sig) in the candidate. Otherwise a mirror
    // could silently move hooks that work today. Fail-closed: an existing entry
    // that is absent from the candidate is inconsistent too.
    [[nodiscard]] inline bool EntriesAreConsistent(const EntryMap& existing,
                                                   const EntryMap& candidate,
                                                   const std::unordered_set<uint32_t>& missingKeys) noexcept
    {
        for (const auto& [key, entry] : existing) {
            if (missingKeys.count(key) != 0) continue;   // missing: may be fixed
            const auto it = candidate.find(key);
            if (it == candidate.end() || !SameEntry(entry, it->second)) return false;
        }
        return true;
    }

    // A candidate RVA can only "resolve" a missing function if it points inside
    // the loaded module image; anything else is treated as unresolvable
    // (fail-closed) instead of being counted as a fix.
    [[nodiscard]] constexpr bool IsRvaWithinImage(std::uintptr_t rva, std::size_t imageSize) noexcept
    {
        return rva != 0 && rva < imageSize;
    }

    // Full replacement decision used by PatternLoader. On top of the key-set
    // rules above:
    //   * EntriesAreConsistent (working entries untouched -- H-1);
    //   * no NEW or CHANGED entry for a missing function may be unresolvable
    //     (missesUnresolvableInCandidate == 0, counted by the caller): a
    //     candidate that ships a bad entry for one missing function is refused
    //     even if it fixes another, because the next launch would hook that bad
    //     entry. An entry identical to the cached one is not counted -- it is
    //     today's state and already installs no hook.
    [[nodiscard]] inline bool ShouldReplaceCache(const EntryMap& existing,
                                                 const EntryMap& candidate,
                                                 const std::unordered_set<uint32_t>& missingKeys,
                                                 bool bodyDiffers,
                                                 size_t missesResolvedByCandidate,
                                                 size_t missesUnresolvableInCandidate)
    {
        if (missesUnresolvableInCandidate != 0) return false;
        if (!EntriesAreConsistent(existing, candidate, missingKeys)) return false;

        std::unordered_set<uint32_t> existingKeys;
        std::unordered_set<uint32_t> candidateKeys;
        existingKeys.reserve(existing.size());
        candidateKeys.reserve(candidate.size());
        for (const auto& kv : existing)  existingKeys.insert(kv.first);
        for (const auto& kv : candidate) candidateKeys.insert(kv.first);
        return ShouldReplaceCache(existingKeys, candidateKeys, bodyDiffers, missesResolvedByCandidate);
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
