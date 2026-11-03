#include "PatternLoader.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/PatternLoaderLogic.h"
#include "Utils/SteamMetadata/PatternRefreshLogic.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/RemoteTomlLogic.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

// ---- compile-time sanity checks for FNV-1a table keys ----
// If the steam-monitor bot uses the same algorithm these must hold.
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u,
              "FNV-1a mismatch for BBuildAndAsyncSendFrame");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu,
              "FNV-1a mismatch for BuildDepotDependency");

namespace {

// ---- per-function pattern record ----
// Shared with the pure replacement policy (PatternRefreshLogic.h), so the H-1
// "working entries must stay identical" check runs on exactly this layout.
using PatternEntry = PatternRefreshLogic::PatternEntry;

// key = Fnv1aHash(funcName)
using PatternMap = PatternRefreshLogic::EntryMap;

// module → its pattern map
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternMap> g_moduleMaps;

// Modules whose Load() call failed (popup already shown). FindPattern
// silently returns nullptr for these — without re-logging or adding the
// function to g_missingFunctions — so we don't follow one "TOML missing"
// popup with a second popup listing every dependent hook.
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_failedModules;

// required functions whose names were not found during FindPattern
static PatternLoaderLogic::MissingFunctionLog g_missingFunctions;

// ---- byte-pattern scanner ----

static bool ParseSig(const std::string& str,
                     std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& mask)
{
    bytes.clear();
    mask.clear();
    for (const char* p = str.c_str(); *p; ) {
        if (*p == ' ' || *p == '\t' || *p == ',') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0); mask.push_back(0); p += 2; continue;
        }
        char hi = p[0], lo = p[1];
        if (!hi || !lo) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

static void* ScanModule(OSTPlatform::DynamicLibrary::ModuleHandle module,
                        const std::vector<uint8_t>& bytes,
                        const std::vector<uint8_t>& mask)
{
    const auto image = OSTPlatform::Memory::GetModuleImage(module);
    if (!image) return nullptr;

    auto* base = image->base;
    size_t size = image->size;
    size_t patLen = bytes.size();
    if (size < patLen) return nullptr;

    for (size_t i = 0; i <= size - patLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && base[i + j] != bytes[j]) { found = false; break; }
        }
        if (found) return base + i;
    }
    return nullptr;
}

// ---- TOML pattern parser ----

// Section keys are hex literals like "0x82428E37"; each section is a table
// with optional `name`, `rva` (hex string), and `sig` (IDA-style bytes).
static PatternMap TableToPatternMap(const toml::table& tbl)
{
    PatternMap map;
    map.reserve(tbl.size());
    for (auto& [rawKey, val] : tbl) {
        if (!val.is_table()) continue;
        auto& sub = *val.as_table();

        const auto parsedKey = OSTPlatform::Numbers::ParseHexUInt32(std::string(rawKey));
        if (!parsedKey) continue;
        const uint32_t hashKey = *parsedKey;

        PatternEntry entry;
        if (auto v = sub["name"].value<std::string>()) entry.name = *v;
        if (auto v = sub["rva"].value<std::string>()) {
            if (const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*v)) {
                entry.rva = static_cast<uintptr_t>(*rva);
            }
        }
        if (auto v = sub["sig"].value<std::string>()) entry.sig = *v;

        map[hashKey] = std::move(entry);
    }
    return map;
}

static PatternMap ParsePatternString(std::string_view body,
                                     std::string* outError = nullptr)
{
    try {
        return TableToPatternMap(toml::parse(body));
    } catch (const toml::parse_error& e) {
        if (outError) *outError = e.description();
        return {};
    }
}

// Resolves funcName against an explicit pattern map: RVA first, then signature
// scan. verbose=false is used when probing a candidate (not yet active) map
// during cache revalidation, so a miss there doesn't read like a live failure.
// requireRvaInImage=true (candidate probing only, H-1): an RVA counts as
// resolved only if it lies inside the loaded module image; otherwise the entry
// is unresolvable -- no fallback to its sig, because the live lookup on the next
// launch would use that RVA. The live path keeps its pre-v1.3.4 behaviour.
static void* LookupInMap(OSTPlatform::DynamicLibrary::ModuleHandle module,
                         const PatternMap& map, const char* funcName, bool verbose,
                         bool requireRvaInImage = false)
{
    const uint32_t key = Fnv1aHash(funcName);
    auto entryIt = map.find(key);
    if (entryIt == map.end()) {
        if (verbose) LOG_WARN("PatternLoader: no entry for '{}' (key=0x{:08X})", funcName, key);
        return nullptr;
    }

    const PatternEntry& entry = entryIt->second;

    // Priority 1: RVA direct offset
    if (entry.rva != 0) {
        if (requireRvaInImage) {
            const auto image = OSTPlatform::Memory::GetModuleImage(module);
            if (!image || !PatternRefreshLogic::IsRvaWithinImage(entry.rva, image->size))
                return nullptr;   // fail-closed: cannot validate -> unresolvable
        }
        void* addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(module) + entry.rva);
        if (verbose) LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X}", funcName, entry.rva);
        return addr;
    }

    // Priority 2: byte-signature scan
    if (!entry.sig.empty()) {
        std::vector<uint8_t> bytes, mask;
        if (ParseSig(entry.sig, bytes, mask)) {
            void* addr = ScanModule(module, bytes, mask);
            if (addr) {
                [[maybe_unused]] uintptr_t rva = reinterpret_cast<uintptr_t>(addr) -
                                reinterpret_cast<uintptr_t>(module);
                if (verbose) LOG_DEBUG("PatternLoader: {} resolved via sig @ RVA 0x{:X}", funcName, rva);
                return addr;
            }
            if (verbose) LOG_WARN("PatternLoader: sig scan miss for '{}' (pattern parsed OK, "
                                  "no match in module image)", funcName);
        } else if (verbose) {
            LOG_WARN("PatternLoader: malformed sig for '{}': '{}'", funcName, entry.sig);
        }
    } else if (verbose) {
        LOG_WARN("PatternLoader: entry for '{}' has neither rva nor sig", funcName);
    }

    return nullptr;
}

// ---- popup helpers (detached threads so we never block Steam) ----

// Surface a missing pattern file to the user, with enough detail to either
// (a) drop a file in manually, (b) check the upstream repo, or (c) file
// an actionable bug report.  We deliberately only disable hooks for the
// failing module — the rest of AmethystTool keeps working.
static void ShowDownloadFailedPopup(const std::string& dllName,
                                    const std::string& sha256,
                                    const std::string& component)
{
    SteamDiagnostics::ShowWarning(
        "AmethystTool - Unsupported Steam Version",
        "AmethystTool: signature file not found for " + dllName + ".\n\n"
        "Hooks that depend on " + dllName + " are disabled for this session; "
        "other modules are unaffected.\n\n"
        "You can:\n"
        "  1. Wait for the next signature update, then restart Steam.\n"
        "  2. Drop a matching TOML at:\n"
        "       <Steam>\\amethysttool\\pattern\\" + component + "\\" + sha256 + ".toml\n"
        "  3. Check upstream:\n"
        "       https://github.com/madoiscool/steam-monitor/tree/pattern/" + component + "\n"
        "  4. Consult the AmethystTool README and include the diagnostics below.");
}

} // namespace

// ---- public API ----

namespace PatternLoader {

    constexpr const char* kPatternChannel = "pattern";

namespace {

// ---- self-healing cache revalidation (v1.3.4) ----
// Policy and rationale: Utils/SteamMetadata/PatternRefreshLogic.h.

using ModuleHandle = OSTPlatform::DynamicLibrary::ModuleHandle;

struct LoadInfo {
    std::string dllPath;
    std::string component;
    std::string sha256;
    bool        fromCache     = false;
    bool        cacheFallback = false;
    std::string cachedBody;   // kept only for a cache hit; revalidation diffs against it
};

// Guards g_loadInfo and g_requiredMisses. FindPattern normally runs on the init
// thread, but some RESOLVE_FUNC lookups happen lazily on Steam threads.
std::mutex g_revalidateMutex;
std::unordered_map<ModuleHandle, LoadInfo> g_loadInfo;
std::unordered_map<ModuleHandle, std::vector<std::string>> g_requiredMisses;
std::atomic<bool> g_revalidationStarted{ false };

struct RevalidationJob {
    ModuleHandle             module = nullptr;
    LoadInfo                 info;
    PatternMap               cachedMap;   // snapshot; the worker never touches g_moduleMaps
    std::vector<std::string> missing;     // required functions that failed to resolve
};

// Writes the throttle stamp. Content is a fixed marker string: no SHA (that is
// already the file NAME, same as the .toml beside it), no path, account or time.
// Returns false if the stamp could not be persisted with a fresh mtime -- the
// caller then skips the network, because without a stamp the 24h throttle would
// degrade to "every launch" (and an unwritable dir couldn't take the new TOML anyway).
[[nodiscard]] bool TouchStamp(const std::filesystem::path& stampPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(stampPath.parent_path(), ec);
    {
        std::ofstream ofs(stampPath, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs << "AmethystTool pattern cache revalidation stamp\n";
        ofs.flush();
        if (!ofs) return false;
    }
    std::error_code timeEc;
    fs::last_write_time(stampPath, fs::file_time_type::clock::now(), timeEc);
    return !timeEc;
}

// Removes "<sha256>.toml.<tid>.tmp" leftovers (ReplaceCacheFile's temp naming)
// from one component's cache directory. Best-effort, never throws.
void RemoveOrphanTempFiles(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::error_code fileEc;
        if (p.extension() == L".tmp" &&
            p.filename().wstring().find(L".toml.") != std::wstring::npos &&
            it->is_regular_file(fileEc))
            fs::remove(p, fileEc);
    }
}

// Runs on a detached worker thread, never on Steam's loader path.
void Revalidate(const RevalidationJob& job, bool enabled)
{
    namespace fs = std::filesystem;
    using namespace PatternRefreshLogic;

    const RemoteToml::Request request{ kPatternChannel, job.info.component, job.info.dllPath };
    const fs::path cachePath = RemoteToml::CachePathFor(request, job.info.sha256);
    const fs::path stampPath = cachePath.parent_path() /
                               OSTPlatform::Encoding::Utf8ToPath(StampFileName(job.info.sha256));

    RevalidateInputs in;
    in.enabled        = enabled;
    in.fromCache      = job.info.fromCache;
    in.cacheFallback  = job.info.cacheFallback;
    in.requiredMisses = job.missing.size();
    std::error_code ec;
    const fs::file_time_type stamped = fs::last_write_time(stampPath, ec);
    if (!ec) {
        in.sinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(
            fs::file_time_type::clock::now() - stamped);
    }

    const Decision decision = ShouldRevalidate(in);
    if (decision != Decision::Revalidate) {
        LOG_INFO("PatternLoader: {} has {} unresolved required function(s); cache revalidation skipped ({})",
                 job.info.component, job.missing.size(), ToString(decision));
        return;
    }

    // Stamp BEFORE the network: a timeout or crash still counts as an attempt,
    // so a mirror that simply lacks the function costs one request per interval.
    if (!TouchStamp(stampPath)) {
        LOG_WARN("PatternLoader: {} revalidation skipped: could not write throttle stamp {}",
                 job.info.component, OSTPlatform::Encoding::PathToUtf8(stampPath));
        return;
    }
    LOG_INFO("PatternLoader: {} cache has {} unresolved required function(s); "
             "checking mirrors for an updated TOML", job.info.component, job.missing.size());

    // requireHttps (M-4): this path may replace a cache that is already trusted,
    // so a cleartext mirror (custom http:// url_template) is never used here.
    const RemoteToml::RemoteBody remote = RemoteToml::FetchRemoteOnly(
        request, job.info.sha256, kRevalidateTimeoutMs, kRevalidateRecvTimeoutMs,
        /*requireHttps=*/true);
    if (!remote.ok) {
        LOG_INFO("PatternLoader: {} revalidation: no mirror answered; keeping cache", job.info.component);
        return;
    }

    std::string parseErr;
    const PatternMap remoteMap = ParsePatternString(remote.body, &parseErr);

    // Probe each missing function against the candidate in the live module.
    // RVAs must land inside the image (fail-closed, H-1). A NEW or CHANGED entry
    // for a missing function that cannot be resolved makes the whole candidate
    // unusable (the next launch would hook it). An entry identical to the cached
    // one is today's state -- it already fails, installs no hook -- so it does not
    // block a candidate that fixes another missing function.
    std::unordered_set<uint32_t> missingKeys;
    size_t resolved = 0;
    size_t unresolvable = 0;
    // H-1b: entries the candidate ADDS outside the missing set (e.g. an optional
    // function) are not probed below, so EntriesAreConsistent checks their RVAs
    // against the image size. 0 = image unreadable -> fail-closed (refuse them).
    const auto image = OSTPlatform::Memory::GetModuleImage(job.module);
    const size_t imageSize = image ? image->size : 0;
    for (const std::string& name : job.missing) {
        const uint32_t key = Fnv1aHash(name.c_str());
        missingKeys.insert(key);
        if (LookupInMap(job.module, remoteMap, name.c_str(), /*verbose=*/false,
                        /*requireRvaInImage=*/true)) {
            ++resolved;
        } else if (const auto cand = remoteMap.find(key); cand != remoteMap.end()) {
            const auto old = job.cachedMap.find(key);
            if (old == job.cachedMap.end() || !SameEntry(old->second, cand->second))
                ++unresolvable;
        }
    }

    if (!EntriesAreConsistent(job.cachedMap, remoteMap, missingKeys, imageSize)) {
        LOG_WARN("PatternLoader: {} revalidation: mirror copy alters or drops entries that "
                 "work today, or adds an entry whose RVA lies outside the module image; "
                 "refused (only missing functions may change)", job.info.component);
        return;
    }

    const bool differs = remote.body != job.info.cachedBody;
    if (!ShouldReplaceCache(job.cachedMap, remoteMap, missingKeys, differs, resolved, unresolvable,
                            imageSize)) {
        LOG_INFO("PatternLoader: {} revalidation: mirror copy is not an improvement "
                 "(differs={} entries={} resolves {}/{} missing, {} unresolvable{}{}); keeping cache",
                 job.info.component, differs, remoteMap.size(), resolved, job.missing.size(),
                 unresolvable, parseErr.empty() ? "" : ", parse error: ", parseErr);
        return;
    }

    if (!RemoteToml::ReplaceCacheFile(cachePath, remote.body)) {
        LOG_WARN("PatternLoader: {} revalidation: could not write the updated cache file",
                 job.info.component);
        return;
    }

    // Hooks were already installed (or skipped) from the old file, so the new one
    // takes effect on the next Steam start -- swapping live hooks is not worth the risk.
    LOG_INFO("PatternLoader: {} cache updated from {} ({}/{} missing function(s) now resolve); "
             "restart Steam to apply", job.info.component, remote.url, resolved, job.missing.size());
    OSTPlatform::Dialog::ShowInfo(
        "AmethystTool - Signatures Updated",
        "AmethystTool downloaded an updated signature file for " + job.info.component + ".\n\n" +
        std::to_string(resolved) + " of " + std::to_string(job.missing.size()) +
        " missing function(s) can now be located.\n\n"
        "Restart Steam to apply it.");
}

} // namespace

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component)
{
    // Delegate fetch + cache + mirror fallback to RemoteToml.
    const RemoteToml::Request request{ kPatternChannel, component, dllPath };
    RemoteToml::Result r = RemoteToml::Fetch(request);

    // L-2: drop temp files a crash left between ReplaceCacheFile's write and its
    // rename. Safe here: Load runs on the init thread before any revalidation
    // worker exists, so no live temp file can be in this directory.
    RemoveOrphanTempFiles(RemoteToml::CachePathFor(request, r.sha256).parent_path());

    if (r.ok) {
        std::string parseErr;
        PatternMap map = ParsePatternString(r.body, &parseErr);

        // Self-heal a corrupt cache entry: a cached file that parses to nothing
        // would otherwise fail every launch until the user deleted it by hand.
        // Treat it exactly like a cache miss (same synchronous mirror walk with
        // the normal timeouts, same request) and overwrite it only with a body
        // that parses. Gated by [remote] revalidate like the background check,
        // so revalidate = false restores the pre-v1.3.4 network behaviour exactly.
        if (map.empty() && r.fromCache && !r.cacheFallback && Config::GetRemoteRevalidate()) {
            LOG_WARN("PatternLoader: cached TOML for {} is unusable ({}); re-downloading",
                     component, parseErr.empty() ? "no entries" : parseErr);
            RemoteToml::RemoteBody fresh = RemoteToml::FetchRemoteOnly(
                request, r.sha256,
                PatternRefreshLogic::kCorruptCacheRefetchTimeoutMs,
                PatternRefreshLogic::kCorruptCacheRefetchRecvTimeoutMs);
            if (fresh.ok) {
                std::string freshErr;
                PatternMap freshMap = ParsePatternString(fresh.body, &freshErr);
                if (!freshMap.empty()) {
                    if (!RemoteToml::ReplaceCacheFile(RemoteToml::CachePathFor(request, r.sha256), fresh.body))
                        LOG_WARN("PatternLoader: could not overwrite the corrupt cache file for {}", component);
                    map = std::move(freshMap);
                    r.body = std::move(fresh.body);
                    r.fromCache = false;
                } else {
                    LOG_WARN("PatternLoader: re-downloaded TOML for {} is unusable too ({})",
                             component, freshErr.empty() ? "no entries" : freshErr);
                }
            }
        }

        if (!map.empty()) {
            LOG_INFO("PatternLoader: loaded {} patterns for {} ({})",
                     map.size(), component,
                     RemoteTomlLogic::DescribeLoadSource(r.fromCache, r.cacheFallback));
            {
                std::lock_guard<std::mutex> lock(g_revalidateMutex);
                LoadInfo info;
                info.dllPath       = dllPath;
                info.component     = component;
                info.sha256        = r.sha256;
                info.fromCache     = r.fromCache;
                info.cacheFallback = r.cacheFallback;
                if (r.fromCache && !r.cacheFallback) info.cachedBody = r.body;
                g_loadInfo[module] = std::move(info);
            }
            g_moduleMaps[module] = std::move(map);
            return true;
        }
        LOG_WARN("PatternLoader: TOML for {} parsed empty ({})",
                 component, parseErr.empty() ? "no entries" : parseErr);
    }

    // Total failure — popup + disable module's hooks. dllPath is UTF-8 (SteamUIPath/
    // SteamclientPath); decode via Utf8ToPath rather than the ANSI-codepage
    // path(std::string) constructor. .filename() is always the hardcoded ASCII
    // "steamclient64.dll"/"steamui.dll" suffix today, so this has no visible effect
    // yet, but is correct if that ever changes. The final .string() (not PathToUtf8)
    // is deliberate: dllName reaches SteamDiagnostics::ShowWarning -> MessageBoxA,
    // which needs an ANSI-codepage string, not UTF-8.
    std::string dllName = OSTPlatform::Encoding::Utf8ToPath(dllPath).filename().string();
    std::string sha     = r.sha256.empty() ? "(hash failed)" : r.sha256;
    ShowDownloadFailedPopup(dllName, sha, component);
    g_failedModules.insert(module);
    return false;
}

namespace {

// Resolves funcName in module's pattern map: RVA first, then signature scan.
// Logs why a lookup failed; recording it as "missing" is the caller's call.
void* Lookup(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    auto mapIt = g_moduleMaps.find(module);
    if (mapIt == g_moduleMaps.end()) {
        // Load() was never called for this module.
        LOG_WARN("PatternLoader: FindPattern called for module that was never loaded "
                 "('{}')", funcName);
        return nullptr;
    }

    return LookupInMap(module, mapIt->second, funcName, /*verbose=*/true);
}

void* Find(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName,
           PatternLoaderLogic::Requirement req)
{
    // If the whole module's pattern file failed to load, stay quiet — the
    // user already saw one popup and the main.log already has the warning.
    // No point amplifying that into one log line per hook plus a second
    // "missing functions" popup later.
    if (g_failedModules.count(module)) {
        return nullptr;
    }

    void* addr = PatternLoaderLogic::ResolveOrRecord(funcName, req, g_missingFunctions,
                                                     [module, funcName] { return Lookup(module, funcName); });
    // Remember which module a REQUIRED miss belongs to: that is the only signal
    // that makes StartCacheRevalidation() look at the network (optional misses
    // never do -- many builds legitimately lack optional functions).
    if (!addr && req == PatternLoaderLogic::Requirement::Required) {
        std::lock_guard<std::mutex> lock(g_revalidateMutex);
        g_requiredMisses[module].emplace_back(funcName);
    }
    return addr;
}

} // namespace

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    return Find(module, funcName, PatternLoaderLogic::Requirement::Required);
}

void* FindPatternOptional(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    void* addr = Find(module, funcName, PatternLoaderLogic::Requirement::Optional);
    if (!addr)
        LOG_INFO("PatternLoader: optional '{}' unavailable; its hook is skipped (no popup)", funcName);
    return addr;
}

void ReportMissingFunctions()
{
    const std::vector<std::string> missing = g_missingFunctions.TakeAll();
    if (missing.empty()) return;

    // Build the list
    std::string list;
    for (const auto& name : missing)
        list += "  - " + name + "\n";

    SteamDiagnostics::ShowWarning(
        "AmethystTool - Missing Signatures",
        "AmethystTool: some functions could not be located.\n\n"
        "The following functions were not found in the signature file:\n" +
        list +
        "\nHooks for these functions are disabled for this session.\n\n"
        "See the AmethystTool README for how to report this.");
}

void StartCacheRevalidation()
{
    if (g_revalidationStarted.exchange(true)) return;   // once per process

    const bool enabled = Config::GetRemoteRevalidate();
    std::vector<RevalidationJob> jobs;
    {
        std::lock_guard<std::mutex> lock(g_revalidateMutex);
        for (const auto& [module, info] : g_loadInfo) {
            auto missIt = g_requiredMisses.find(module);
            if (missIt == g_requiredMisses.end() || missIt->second.empty())
                continue;   // healthy module: no request, no log noise
            // g_moduleMaps is written only by Load() on the init thread, which is
            // also the caller here, so this read cannot race a write.
            auto mapIt = g_moduleMaps.find(module);
            if (mapIt == g_moduleMaps.end()) continue;

            RevalidationJob job;
            job.module    = module;
            job.info      = info;
            job.cachedMap = mapIt->second;
            job.missing   = missIt->second;
            std::sort(job.missing.begin(), job.missing.end());
            job.missing.erase(std::unique(job.missing.begin(), job.missing.end()), job.missing.end());
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) return;

    // M-1: an exception escaping a std::thread body calls std::terminate, which
    // would take Steam down with it. Catch everything in the worker, and a
    // failure to create the thread (std::system_error) on the init thread.
    try {
        std::thread([jobs = std::move(jobs), enabled]() {
            try {
                for (const RevalidationJob& job : jobs) Revalidate(job, enabled);
            } catch (...) {
                LOG_ERROR("PatternLoader: cache revalidation worker aborted by an exception; "
                          "keeping the current cache");
            }
        }).detach();
    } catch (...) {
        LOG_ERROR("PatternLoader: could not start the cache revalidation worker; skipped");
    }
}

} // namespace PatternLoader
