#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "PackageReconcileLogic.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    std::atomic<void*> g_pCUser{nullptr};
    // Published only after the initial fill, so non-null also means "initialized".
    // CheckAppOwnership's fast path, which runs on every call, reads just this.
    std::atomic<PackageInfo*> g_pInjectedPackageInfo{nullptr};

    // Serialises every access THIS DLL makes to the injected package's AppIdVec:
    // the initial fill on a Steam thread, NotifyLicenseChanged on a watcher thread,
    // the owned-depot dump. A plain mutex because almost all of them write, including
    // the fill. Order: g_appIdVecMutex -> LuaConfig's g_stateMutex (see LuaConfig.cpp).
    // Never hold it across MarkLicenseAsChanged/ProcessPendingLicenseUpdates or
    // GetPackageInfo: those run Steam code that can re-enter CheckAppOwnership.
    // ponytail: Steam's own threads read this vector with no lock we can take, and
    // CUtlMemoryGrow can reallocate it under them. That race is inherent to mutating
    // it off-thread; the upgrade path is marshalling the mutation onto the thread
    // that reads it, the way UI removals are drained from RunFrame.
    std::mutex g_appIdVecMutex;

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        if (!g_pCUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(g_pCUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(g_pCUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    bool CUtlMemoryGrowWrap(CUtlVector<AppId_t>* pVec, int grow_size) {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }
        return oCUtlMemoryGrow(pVec, grow_size);
    }

    bool ContainsAppId(const CUtlVector<AppId_t>& vec, AppId_t id) {
        const AppId_t* first = vec.m_Memory.m_pMemory;
        return std::find(first, first + vec.m_Size, id) != first + vec.m_Size;
    }

    // Caller holds g_appIdVecMutex.
    bool InjectConfiguredDepots(PackageInfo* pPkg) {
        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license. 
        std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
        if (!appIds.empty()) {
            uint32 oldSize = pPkg->AppIdVec.m_Size;
            uint32 numToAdd = static_cast<uint32>(appIds.size());
            LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): adding {} apps, oldSize={}", kInjectedPackageId, numToAdd, oldSize);
            if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, numToAdd)) {
                LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                return false;
            }
            for (uint32 i = 0; i < numToAdd; i++)
                pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = appIds[i];
        }
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_pInjectedPackageInfo) return true;
        if (!CAPTURE_READY(GetPackageInfo)) return false;

        PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
        if(!pPkg) {
            LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
            return false;
        }

        {
            std::scoped_lock lock(g_appIdVecMutex);
            // CheckAppOwnership runs on several Steam threads: only one may fill.
            if (g_pInjectedPackageInfo) return true;
            if (!InjectConfiguredDepots(pPkg)) return false;
            g_pInjectedPackageInfo = pPkg;
        }

        MarkLicenseAsChangedAndProcessUpdates();
        return true;
    }


    // ── Owned depot enumeration ──────────────────────────────────────────────
    // The license list arrives once at logon and is the only enumeration of
    // owned packages available; CheckAppOwnership answers one app at a time and
    // only for apps something asks about. Resolving a package to its depots
    // needs GetPackageInfo, whose `this` is captured from Steam's own first
    // call — which normally has not happened by the time the list lands. So the
    // list is parked here and drained on the next CheckAppOwnership.
    //
    // OnLicenseList runs on the network thread and the drain on a Steam thread, and a
    // reconnect can deliver a second list mid-walk, so the list is handed over under
    // this mutex and walked as a private copy: the walk calls GetPackageInfo, which
    // no lock may be held across.
    std::mutex g_pendingLicensesMutex;
    std::vector<Hooks_Package::License> g_pendingLicenses;
    // TryDumpOwnedDepots runs on every CheckAppOwnership: this keeps it lock-free.
    std::atomic<bool> g_licensesPending{false};
    std::atomic<bool> g_dumpRunning{false};

    // Filled by the same walk that logs. Read from other threads eventually
    // (once a network-facing consumer of GetOwnedDepots exists), written from
    // CheckAppOwnership on a Steam thread, hence the mutex.
    std::vector<uint32_t> g_ownedDepots;
    bool                  g_ownedDepotsReady = false;
    std::mutex            g_ownedDepotsMutex;

    // Formats a CUtlVector<AppId_t> as "a, b, c", capped so one enormous package
    // cannot produce an unreadable log line.
    std::string JoinIds(const CUtlVector<AppId_t>& vec, uint32 maxShown = 64) {
        std::string out;
        const uint32 shown = vec.m_Size < maxShown ? vec.m_Size : maxShown;
        for (uint32 i = 0; i < shown; ++i) {
            if (i) out += ", ";
            out += std::to_string(vec.m_Memory.m_pMemory[i]);
        }
        if (vec.m_Size > shown)
            out += " … (+" + std::to_string(vec.m_Size - shown) + " more)";
        return out;
    }

    void DumpOwnedDepots(const std::vector<Hooks_Package::License>& licenses) {
        size_t totalDepots = 0, resolved = 0, unresolved = 0;
        std::vector<uint32_t> owned;

        for (const auto& lic : licenses) {
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, lic.packageId, lic.accessToken);
            if (!pPkg) {
                // Usually a package whose info has not been cached yet rather
                // than a real failure; logged at debug so it does not drown the
                // useful lines.
                ++unresolved;
                LOG_PACKAGE_DEBUG("OwnedDepots: package {} did not resolve (token={})",
                                  lic.packageId, lic.accessToken);
                continue;
            }

            ++resolved;
            totalDepots += pPkg->DepotIdVec.m_Size;

            for (uint32 i = 0; i < pPkg->DepotIdVec.m_Size; ++i) {
                const uint32_t depotId = pPkg->DepotIdVec.m_Memory.m_pMemory[i];
                if (depotId) owned.push_back(depotId);
            }

            // Package 0 is usually on the list too, and its AppIdVec is the injected one.
            [[maybe_unused]] std::string appIds;
            [[maybe_unused]] uint32 appCount = 0;
            {
                std::scoped_lock lock(g_appIdVecMutex);
                appIds = JoinIds(pPkg->AppIdVec);
                appCount = pPkg->AppIdVec.m_Size;
            }

            LOG_PACKAGE_INFO("OwnedDepots: package {} apps={} depots={} | appids=[{}] | depotids=[{}]",
                             lic.packageId,
                             appCount, pPkg->DepotIdVec.m_Size,
                             appIds, JoinIds(pPkg->DepotIdVec));
        }

        // Packages overlap heavily — the same depot arrives via several
        // licenses — so dedupe before anyone intersects against this.
        std::sort(owned.begin(), owned.end());
        owned.erase(std::unique(owned.begin(), owned.end()), owned.end());

        {
            std::lock_guard<std::mutex> lock(g_ownedDepotsMutex);
            g_ownedDepots      = std::move(owned);
            g_ownedDepotsReady = true;
        }

        LOG_PACKAGE_INFO("OwnedDepots: done — {} package(s) resolved, {} unresolved, "
                         "{} depot entr(ies), {} unique",
                         resolved, unresolved, totalDepots, g_ownedDepots.size());

        // The per-package lines above are capped by JoinIds, so for a package
        // with hundreds of depots they show a subset. This set — deduped, after
        // the walk — is the authoritative owned-depot set any future consumer
        // would intersect against, and deciding what gets disclosed off a list
        // nobody can read back is not something to leave to inference. Chunked
        // so no single line is unusable, and debug-only: it is thousands of ids.
        {
            std::lock_guard<std::mutex> lock(g_ownedDepotsMutex);
            constexpr size_t kPerLine = 64;
            for (size_t i = 0; i < g_ownedDepots.size(); i += kPerLine) {
                const size_t end = (i + kPerLine < g_ownedDepots.size())
                                 ? i + kPerLine : g_ownedDepots.size();
                std::string chunk;
                for (size_t j = i; j < end; ++j) {
                    if (j > i) chunk += ", ";
                    chunk += std::to_string(g_ownedDepots[j]);
                }
                LOG_PACKAGE_DEBUG("OwnedDepots[{}-{}]: {}", i, end - 1, chunk);
            }
        }
    }


    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        if (!g_pCUser) {
            g_pCUser = pObj;
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", pObj);
        }

        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        TryInitFakeLicenseOnce();
        Hooks_Package::TryDumpOwnedDepots();

        if (LuaConfig::HasDepot(appId,false)) {
            if (result && pOwn->ExistInPackageNums > 1) {
                // Actually owned — record so HasDepot excludes it going forward
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
            } else {
                pOwn->PackageId    = kInjectedPackageId;
                pOwn->ReleaseState = EAppReleaseState::Released;
                pOwn->bOwnsLicense = true; //This forces DLCs on steam family shared games that u dont own when adding their appid via .lua
                // Setting this free flag to false will hide it from the library UI.
                pOwn->bFreeLicense = false;
                return true;
            }
        }
        return result;
    }
}

namespace Hooks_Package {

    void OnLicenseList(std::vector<License> licenses) {
        std::scoped_lock lock(g_pendingLicensesMutex);
        g_pendingLicenses = std::move(licenses);
        g_licensesPending = true;
    }

    void TryDumpOwnedDepots() {
        if (!g_licensesPending) return;
        if (!CAPTURE_READY(GetPackageInfo)) return;

        // Claimed before dumping, not after: DumpOwnedDepots calls back into
        // steamclient, and CheckAppOwnership is what drives us — without this
        // a re-entrant call would start a second dump. It also stops a slower walk
        // of an old list overwriting a newer one's result. A list that lands
        // meanwhile stays pending for the next call.
        if (g_dumpRunning.exchange(true)) return;

        std::vector<License> licenses;
        {
            std::scoped_lock lock(g_pendingLicensesMutex);
            licenses.swap(g_pendingLicenses);
            g_licensesPending = false;
        }
        if (!licenses.empty()) DumpOwnedDepots(licenses);

        g_dumpRunning = false;
    }

    OwnedDepots GetOwnedDepots() {
        std::lock_guard<std::mutex> lock(g_ownedDepotsMutex);
        return {g_ownedDepotsReady, g_ownedDepots};
    }

    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }

    void NotifyLicenseChanged() {
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        PackageReconcileLogic::Plan plan;
        {
            // Take and apply under one lock: two watcher threads can get here, and
            // applying each other's queues out of order would lose a change.
            std::scoped_lock lock(g_appIdVecMutex);

            const std::vector<AppId_t> queuedRemovals = LuaConfig::TakePendingRemovals();
            const std::vector<AppId_t> queuedAdditions = LuaConfig::TakePendingAdditions();
            if (queuedRemovals.empty() && queuedAdditions.empty()) {
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
                return;
            }

            plan = PackageReconcileLogic::Reconcile(
                queuedRemovals, queuedAdditions,
                [](AppId_t id) { return LuaConfig::HasDepot(id, false); },
                [&](AppId_t id) { return ContainsAppId(pPkg->AppIdVec, id); });
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: {} queued removals and {} queued additions -> "
                              "{} to remove, {} to add",
                              queuedRemovals.size(), queuedAdditions.size(),
                              plan.removals.size(), plan.additions.size());

            for (AppId_t id : plan.removals) {
                pPkg->AppIdVec.FindAndFastRemove(id);
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
            }

            if (!plan.additions.empty()) {
                uint32_t oldSize = pPkg->AppIdVec.m_Size;
                if (CUtlMemoryGrowWrap(&pPkg->AppIdVec, static_cast<int>(plan.additions.size()))) {
                    for (size_t i = 0; i < plan.additions.size(); ++i) {
                        pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = plan.additions[i];
                        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: inserted AppId {} at [{}]", plan.additions[i], oldSize + i);
                    }
                }else {
                    LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
                    plan.additions.clear();
                }
            }
        }

        // An applied addition invalidates any UI removal that has not
        // reached the UI thread yet.
        for (AppId_t id : plan.additions)
            Hooks_SteamUI::CancelRemoval(id);

        // Mark package 0 as changed and trigger library refresh. This also fires when
        // a reload left the id set unchanged, as it always has: the file may have
        // changed a manifest pin or a token, and nothing shows Steam does not need it.
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
            return;
        }
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", plan.additions.size(), plan.removals.size());

        // Queue UI removals for the main-thread RunFrame hook to drain.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        for (AppId_t id : plan.removals)
            Hooks_SteamUI::QueueRemoval(id);
    }
}
