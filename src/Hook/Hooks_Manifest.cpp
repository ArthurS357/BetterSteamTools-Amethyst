#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include <chrono>
#include <format>
#include <mutex>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach), and
//      (opt-in, [cache] url) pre-seeds depotcache from the archive.
// ═══════════════════════════════════════════════════════════════════
namespace {

    // Synchronous pre-seed budget. A cache HIT (already on disk, or a fast
    // edge hit) resolves well under this; once the total budget is spent the
    // remaining depots fall back to a detached fetch so a huge or slow
    // install never freezes Steam.
    constexpr uint32_t kPreseedFetchTimeoutMs = 5000;
    constexpr int64_t  kPreseedBudgetMs       = 15000;

    // depotId -> (appId, Steam's own manifest GID). Recorded before the
    // override pass below, so this is what Steam believes rather than what we
    // told it. Not consumed by anything yet in this build — LookupDepot exists
    // so a future depot-aware caller (manifest probing) has a bridge to a real
    // app id without needing its own capture of BuildDepotDependency.
    struct DepotSeen { AppId_t appId; uint64 gid; };
    std::unordered_map<uint32, DepotSeen> g_depotsSeen;
    std::mutex g_depotsSeenMutex;

    void RecordDepots(const CUtlVector<DepotEntry>* vec) {
        if (!vec) return;
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        for (uint32 i = 0; i < vec->m_Size; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;
            g_depotsSeen[e.DepotId] = {e.AppId, e.ManifestGid};
        }
    }

    // Opt-in ([cache] url) pre-seed of <steam>\depotcache for every depot OST
    // is responsible for, using the gid Steam just reported (or the pinned
    // gid patched by the override pass right after this runs). A depot is
    // OST-served when it is pinned by a lua (an override) or lua-unlocked but
    // not genuinely owned -- genuinely-owned depots get working codes from
    // Steam directly and are left alone. Fetches run within a shared
    // wall-clock deadline; past it, remaining depots are fetched detached so
    // Steam is never hung. Already-cached depots cost only a stat.
    void PreseedDepots(AppId_t appId, const CUtlVector<DepotEntry>* vec,
                       const std::unordered_map<uint64_t, LuaConfig::ManifestOverride>& overrides,
                       std::chrono::steady_clock::time_point deadline)
    {
        if (!vec) return;
        if (Config::GetCacheSettings().url.empty()) return; // feature is off

        for (uint32 i = 0; i < vec->m_Size; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;

            const bool pinned   = overrides.count(e.DepotId) != 0;
            const bool unlocked = LuaConfig::HasDepot(e.DepotId, false) &&
                                  !LuaConfig::IsOwned(e.AppId);
            if (!pinned && !unlocked) continue;   // owned/normal depot: Steam handles it

            const AppId_t app   = appId;
            const uint32  depot = e.DepotId;
            const uint64  gid   = e.ManifestGid;   // pinned gid already patched in

            if (std::chrono::steady_clock::now() < deadline) {
                ManifestCache::EnsureCached(app, depot, gid, kPreseedFetchTimeoutMs);
            } else {
                OSTPlatform::Thread::StartDetached([app, depot, gid]() -> uint32_t {
                    ManifestCache::EnsureCached(app, depot, gid);
                    return 0;
                });
            }
        }
    }

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        if (!result) return result;

        // Before the override pass, so what is cached is Steam's GID.
        RecordDepots(pDepotInfo);
        RecordDepots(pSharedDepotInfo);

        const auto& overrides = LuaConfig::GetManifestOverrides();

        // Apply manifest overrides in place (only depots a lua explicitly
        // pins). Deliberately NOT an early return when overrides is empty:
        // PreseedDepots below must still run for lua-unlocked-but-not-owned
        // depots, which have no override at all.
        if (!overrides.empty() && pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }

        // Opt-in ([cache] url) pre-seed, after the override pass so a pinned
        // depot's patched gid is what gets pre-seeded.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kPreseedBudgetMs);
        PreseedDepots(AppId, pDepotInfo, overrides, deadline);
        PreseedDepots(AppId, pSharedDepotInfo, overrides, deadline);

        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }

    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid) {
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        auto it = g_depotsSeen.find(depotId);
        if (it == g_depotsSeen.end()) return false;
        outAppId = it->second.appId;
        outGid   = it->second.gid;
        return true;
    }
}
