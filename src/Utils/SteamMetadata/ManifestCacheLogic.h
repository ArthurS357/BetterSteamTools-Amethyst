#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Pure logic behind ManifestCache::EnsureCached, split out so it compiles
// and runs without WinHTTP or a real Steam install (see
// manifest_cache_logic_test.cpp) -- same split as OnlineFixLogic and
// LicenseListLogic. The real depotcache directory, archive base URL, and
// HTTP client are all resolved by ManifestCache and passed in here.
namespace ManifestCacheLogic {

    // Mirrors OSTPlatform::Http::Result's shape without depending on
    // OSTPlatform/include/Http.h, so this header pulls in no WinHTTP surface.
    struct FetchResult {
        std::string body;
        uint32_t    status = 0;
        bool        ok     = false;
    };

    // A GET against `url`, bounded by `recvTimeoutMs`. ManifestCache binds
    // this to the real OSTPlatform::Http::Execute; tests bind it to a fake
    // returning canned results.
    using Fetcher = std::function<FetchResult(const std::string& url, uint32_t recvTimeoutMs)>;

    // Ensure <cacheDir>\<depot>_<gid>.manifest exists, fetching it from
    // "{archiveBaseUrl}/m/{depot}/{gid}" via `fetch` if missing.
    //
    // Returns true when the file is present afterwards (already cached or
    // freshly written). Returns false when the manifest is not archived, the
    // body fails validation, or any argument is unusable (0 depot/gid, empty
    // cacheDir/archiveBaseUrl) -- never throws.
    //
    // outNotArchived, when provided, is set to true ONLY when the archive
    // gave a definitive 404 -- not for timeouts or other transient failures.
    // Callers use it to tell "queued, try later" apart from a passing blip.
    //
    // bypassNegativeCache forces a fresh fetch even for a depot:gid that
    // recently 404'd. A depot that returns 404 is remembered for a short TTL
    // and skipped (no fetch call) on subsequent calls; pass true only when a
    // download is actually starting, so a just-supplied manifest is picked up
    // immediately.
    bool EnsureCached(AppId_t app, uint32_t depot, uint64_t gid,
                      const std::filesystem::path& cacheDir,
                      const std::string& archiveBaseUrl,
                      const Fetcher& fetch,
                      uint32_t recvTimeoutMs, bool* outNotArchived,
                      bool bypassNegativeCache);

} // namespace ManifestCacheLogic
