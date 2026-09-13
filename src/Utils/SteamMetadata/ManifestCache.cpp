#include "ManifestCache.h"
#include "ManifestCacheLogic.h"

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <filesystem>
#include <string>

namespace ManifestCache {
namespace {

    // Real manifests reach ~19 MB; 64 MB leaves generous headroom while still
    // bounding what a hostile origin can make us buffer.
    constexpr uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;
    // A cache HIT is fast, but a cold/evicted edge miss pulls the manifest
    // from the slow origin -- give this fetch its own generous recv budget;
    // resolve/connect/send stay at the shared [manifest] timeouts so a dead
    // host still fails fast.
    constexpr uint32_t kRecvTimeoutMs = 60000;

    std::filesystem::path DepotCacheDir() {
        const std::filesystem::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
        if (steamExe.empty()) return {};
        return steamExe.parent_path() / "depotcache";
    }

    std::string ArchiveBaseUrl() {
        std::string base = Config::GetCacheSettings().url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base;
    }

    ManifestCacheLogic::FetchResult RealFetch(const std::string& url, uint32_t recvTimeoutMs) {
        const auto to = Config::GetManifestTimeouts();
        const auto r = OSTPlatform::Http::Execute(
            L"GET", url.c_str(), nullptr, 0, nullptr,
            to.resolve, to.connect, to.send, recvTimeoutMs, kMaxBodyBytes);
        return {r.body, r.status, r.ok};
    }

} // namespace

bool EnsureCached(AppId_t app, uint32_t depot, uint64_t gid,
                  uint32_t recvTimeoutMs, bool* outNotArchived,
                  bool bypassNegativeCache) {
    if (outNotArchived) *outNotArchived = false;

    const std::string base = ArchiveBaseUrl();
    if (base.empty()) return false; // [cache] url not configured -- feature is off

    const std::filesystem::path dir = DepotCacheDir();
    if (dir.empty()) {
        LOG_MANIFEST_WARN("ManifestCache: could not resolve depotcache dir");
        return false;
    }

    const bool cached = ManifestCacheLogic::EnsureCached(
        app, depot, gid, dir, base, RealFetch,
        recvTimeoutMs ? recvTimeoutMs : kRecvTimeoutMs,
        outNotArchived, bypassNegativeCache);

    if (cached)
        LOG_MANIFEST_DEBUG("ManifestCache: app={} depot={} gid={} ready", app, depot, gid);
    else
        LOG_MANIFEST_TRACE("ManifestCache: app={} depot={} gid={} not available", app, depot, gid);

    return cached;
}

} // namespace ManifestCache
