#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "Hook/ManifestHookLogic.h"
#include "dllmain.h"
#include "Utils/Config/Config.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
//    YldLoadDepotManifest — (optional pattern; opt-in, [cache] url)
//      pre-seeds depotcache from the archive right before Steam's own
//      per-manifest disk check.
// ═══════════════════════════════════════════════════════════════════
namespace {

    // Per-fetch receive cap for the on-demand pre-seed in YldLoadDepotManifest.
    // A cache HIT (already on disk, or a fast edge hit) resolves well under
    // this; on timeout the original takes its normal request-code path, so the
    // worst case is the pre-v1.3.2 behavior plus this wait.
    constexpr uint32_t kPreseedFetchTimeoutMs = 5000;

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

        // An empty depot list for a lua app is appinfo caught mid-PICS-refresh;
        // storing it makes the install "complete" with nothing downloaded. Fail
        // the call so Steam keeps its existing config (see
        // ManifestHookLogic::ShouldRejectEmptyDepotList).
        if (ManifestHookLogic::ShouldRejectEmptyDepotList(
                AppId, pDepotInfo != nullptr, pDepotInfo ? pDepotInfo->m_Size : 0,
                LuaConfig::HasDepot(AppId, false)))
        {
            LOG_MANIFEST_WARN("BuildDepotDependency: app {} returned 0 depots "
                "(appinfo mid-refresh) - failing the call so Steam keeps its "
                "existing depot config instead of storing an empty one", AppId);
            return false;
        }

        // Before the override pass, so what is cached is Steam's GID.
        RecordDepots(pDepotInfo);
        RecordDepots(pSharedDepotInfo);

        const auto& overrides = LuaConfig::GetManifestOverrides();

        // Apply manifest overrides in place (only depots a lua explicitly
        // pins). The patched gid is what Steam goes on to request, so
        // YldLoadDepotManifest pre-seeds the pinned manifest later.
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

        // No pre-seeding here (v1.3.2, upstream dec29b7 + 0b776c5). The old
        // synchronous PreseedDepots sweep fetched manifests on Steam's thread for
        // every lua depot while Steam was still building depot configs at
        // startup -- speculative work for games that may never be downloaded.
        // YldLoadDepotManifest does it at the per-manifest acquire point instead.
        return result;
    }

    // ── On-demand manifest pre-seed ─────────────────────────────────
    // CDepotDownloadMgr's per-manifest acquire. It builds
    // <steam>\depotcache\<depot>_<gid>.manifest, checks disk, and only falls
    // through to the request-code path on a miss. Fetching the archived
    // manifest here -- before the original's own disk check -- makes that
    // check succeed, so the download starts on the first attempt. Unlike
    // BuildDepotDependency this fires for every manifest acquisition,
    // including workshop items (depot == appid), with the gid Steam is
    // actually requesting.
    //
    // Signature from upstream dec29b7: appId=a3, depotId=a4, manifestGid=a5,
    // branch=a6. Installed as OPTIONAL: TOMLs cached before 2026-09-19 (and
    // the stale OpenSteam001 mirror) lack this pattern; then the hook is
    // simply absent -- no popup, no fetch -- and Steam's request-code path
    // (plus HandleSend's on-demand fetch) behaves as before.
    HOOK_FUNC(YldLoadDepotManifest, int64_t,
              void* a1, void* a2, int appId, uint32_t depotId,
              uint64_t manifestGid, const char* branch)
    {
        const auto app = static_cast<AppId_t>(appId);
        if (ManifestHookLogic::ShouldPreseedOnManifestLoad(
                !Config::GetCacheSettings().url.empty(), depotId, manifestGid,
                LuaConfig::GetManifestOverrides().count(depotId) != 0,
                LuaConfig::HasDepot(depotId, false),
                LuaConfig::IsOwned(app) || LuaConfig::IsOwned(depotId)))
        {
            LOG_MANIFEST_DEBUG("YldLoadDepotManifest: pre-seed app={} depot={} gid={} branch={}",
                               app, depotId, manifestGid, branch ? branch : "");
            // Bounded blocking fetch: an archive hit turns the original's disk
            // check into a first-attempt success; a miss/timeout just falls
            // through to the normal path.
            ManifestCache::EnsureCached(app, depotId, manifestGid, kPreseedFetchTimeoutMs);
        }
        return oYldLoadDepotManifest(a1, a2, appId, depotId, manifestGid, branch);
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        INSTALL_HOOK_OPTIONAL_C(YldLoadDepotManifest);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNINSTALL_HOOK(YldLoadDepotManifest);   // no-op when the optional hook was skipped
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
