#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include <format>
#include <mutex>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

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

        // Before the override pass, so what is cached is Steam's GID.
        RecordDepots(pDepotInfo);
        RecordDepots(pSharedDepotInfo);

        const auto& overrides = LuaConfig::GetManifestOverrides();
        if (overrides.empty()) return result;

        if (pDepotInfo && pDepotInfo->m_Size) {
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
