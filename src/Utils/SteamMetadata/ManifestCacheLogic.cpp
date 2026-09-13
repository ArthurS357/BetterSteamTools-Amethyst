#include "ManifestCacheLogic.h"

#include <windows.h> // MoveFileExW, GetCurrentThreadId

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace ManifestCacheLogic {
namespace {

    // A valid manifest is at least the payload header + a metadata section +
    // the EOF marker; anything smaller is certainly not one.
    constexpr size_t kMinBodyBytes = 16;

    constexpr uint32_t kHttpStatusOk       = 200;
    constexpr uint32_t kHttpStatusNotFound = 404;

    // On-disk byte order (little-endian u32 magics).
    constexpr unsigned char kPayloadMagic[4] = {0xD0, 0x17, 0xF6, 0x71}; // 0x71F617D0
    constexpr unsigned char kEofMagic[4]     = {0xAB, 0x15, 0xC4, 0x32}; // 0x32C415AB

    bool LooksLikeManifest(const std::string& body) {
        if (body.size() < kMinBodyBytes) return false;
        const auto* head = reinterpret_cast<const unsigned char*>(body.data());
        const auto* tail = head + body.size() - 4;
        for (int i = 0; i < 4; ++i) {
            if (head[i] != kPayloadMagic[i]) return false;
            if (tail[i] != kEofMagic[i])     return false;
        }
        return true;
    }

    // Writes via a temp file beside the target, then an atomic rename. Uses
    // the native wide path throughout (fs::path's own value_type on Windows)
    // rather than fs::path::string() -- that narrows through the process's
    // current ANSI codepage, not UTF-8, and can silently mangle a Steam
    // install path containing non-ASCII characters before the rename ever
    // reaches Win32. MoveFileExW + native wide c_str() avoids that entirely.
    bool WriteAtomic(const fs::path& dest, const std::string& body) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec); // no-op if it exists

        const fs::path tmp = fs::path(dest).concat(
            std::format(L".{}.tmp", ::GetCurrentThreadId()));

        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            ofs.flush();
            if (!ofs) {
                ofs.close();
                fs::remove(tmp, ec);
                return false;
            }
        }

        if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    // Negative cache: a depot:gid that returns a definitive 404 is remembered
    // here for a short window so repeated calls do not re-fetch (and
    // re-hammer) the archive for a manifest already known missing. A real
    // download bypasses this (see bypassNegativeCache); this is in-process
    // memory only and does not survive a restart.
    constexpr auto kNegativeTtl = std::chrono::minutes(10);
    std::mutex g_negMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_negCache;

    // De-dupe concurrent fetches of the same depot:gid.
    std::mutex g_inFlightMutex;
    std::unordered_set<std::string> g_inFlight;

    struct InFlightGuard {
        std::string key;
        bool acquired = false;
        explicit InFlightGuard(std::string k) : key(std::move(k)) {
            const std::lock_guard<std::mutex> lock(g_inFlightMutex);
            acquired = g_inFlight.insert(key).second;
        }
        ~InFlightGuard() {
            if (!acquired) return;
            const std::lock_guard<std::mutex> lock(g_inFlightMutex);
            g_inFlight.erase(key);
        }

        // A copy would race both instances' destructors to erase the same
        // key; a move would leave the source destructor still trying to
        // erase a key the moved-to instance now owns. Neither is meaningful
        // for a scope guard -- delete both.
        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
        InFlightGuard(InFlightGuard&&) = delete;
        InFlightGuard& operator=(InFlightGuard&&) = delete;
    };

} // namespace

bool EnsureCached(AppId_t app, uint32_t depot, uint64_t gid,
                  const fs::path& cacheDir,
                  const std::string& archiveBaseUrl,
                  const Fetcher& fetch,
                  uint32_t recvTimeoutMs, bool* outNotArchived,
                  bool bypassNegativeCache) {
    (void)app; // identifies the depot in logs only; the wrapper does that
    if (outNotArchived) *outNotArchived = false;
    if (!depot || !gid || cacheDir.empty() || archiveBaseUrl.empty() || !fetch) return false;

    const fs::path dest = cacheDir / std::format("{}_{}.manifest", depot, gid);

    std::error_code ec;
    if (fs::exists(dest, ec)) return true; // already cached, no request needed

    const std::string key = std::format("{}_{}", depot, gid);

    if (!bypassNegativeCache) {
        const std::lock_guard<std::mutex> lock(g_negMutex);
        auto it = g_negCache.find(key);
        if (it != g_negCache.end()) {
            if (std::chrono::steady_clock::now() - it->second < kNegativeTtl)
                return false;
            g_negCache.erase(it); // stale entry: allow a re-check
        }
    }

    const InFlightGuard guard(key);
    if (!guard.acquired) return false; // another caller is already on it

    // Re-check after taking the slot: the other caller may have just finished.
    if (fs::exists(dest, ec)) return true;

    const std::string url = std::format("{}/m/{}/{}", archiveBaseUrl, depot, gid);
    const FetchResult resp = fetch(url, recvTimeoutMs);

    if (!resp.ok || resp.status != kHttpStatusOk) {
        // 404 = not archived (the common case). Any other status or a
        // transport failure (!ok) is transient and must NOT be reported as
        // missing, nor cached negatively -- it should be retried promptly.
        const bool notArchived = (resp.ok && resp.status == kHttpStatusNotFound);
        if (outNotArchived) *outNotArchived = notArchived;
        if (notArchived) {
            const std::lock_guard<std::mutex> lock(g_negMutex);
            g_negCache[key] = std::chrono::steady_clock::now();
        }
        return false;
    }

    if (!LooksLikeManifest(resp.body)) return false;

    return WriteAtomic(dest, resp.body);
}

} // namespace ManifestCacheLogic
