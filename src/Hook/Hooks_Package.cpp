#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "PackageReconcileLogic.h"
#include "Hook/AppIdVecOrderLogic.h"
#include "Hook/InjectedAppIdLogic.h"
#include "Hook/ReactiveRefreshLogic.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
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
        void* const pUser = g_pCUser.load(std::memory_order_acquire);
        if (!pUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(pUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(pUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    // ── Reactive license refresh (v1.5.2, see ReactiveRefreshLogic.h) ─────────
    // The hook writes g_refreshSignals (lock-free); one worker thread, started after the
    // initial refresh, reads it and repeats the refresh while lua apps still read as "in
    // no package".
    ReactiveRefreshLogic::Signals g_refreshSignals;

    // Set with release AFTER TryInitFakeLicenseOnce's initial refresh returns. Snapshotted
    // with acquire BEFORE oCheckAppOwnership so answers computed pre-injection are never
    // misclassified as Stale.
    std::atomic<bool> g_initialRefreshDone{false};

    // Serialises the refreshes made from OUR threads (the reactive worker and the .lua /
    // config watchers via NotifyLicenseChanged), so two of them never run Steam's license
    // update at the same time. Never taken on a Steam thread: the initial refresh in
    // TryInitFakeLicenseOnce runs inside the CheckAppOwnership hook and must not wait on a
    // thread that may itself be inside Steam's license code.
    std::mutex g_licenseRefreshMutex;

    bool RefreshLicensesFromOwnThread() {
        std::scoped_lock lock(g_licenseRefreshMutex);
        return MarkLicenseAsChangedAndProcessUpdates();
    }

    void SleepInterruptibleMs(uint32_t totalMs) {
        constexpr uint32_t kSliceMs = 50;
        uint32_t remaining = totalMs;
        while (remaining > 0 && !g_refreshSignals.StopRequested()) {
            const uint32_t slice = remaining < kSliceMs ? remaining : kSliceMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    }

    void ReactiveRefreshAttempt([[maybe_unused]] int attempt) {
        if (g_refreshSignals.StopRequested()) return;
        LOG_PACKAGE_TRACE("Reactive refresh triggered for app {} (attempt {}/{})",
                          g_refreshSignals.FirstStaleApp(), attempt,
                          ReactiveRefreshLogic::kMaxAttempts);
        if (!RefreshLicensesFromOwnThread())
            LOG_PACKAGE_WARN("Reactive refresh: attempt {} could not run (license functions not ready)", attempt);
    }

    void ReactiveRefreshWorker() {
        const auto start = std::chrono::steady_clock::now();
        const ReactiveRefreshLogic::Summary r = ReactiveRefreshLogic::RunWorker(
            g_refreshSignals,
            &SleepInterruptibleMs,
            &ReactiveRefreshAttempt);
        if (r.stopped) return;   // DLL unloading / Steam shutting down: touch nothing more.

        [[maybe_unused]] const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (r.fresh && r.refreshes == 0) {
            LOG_PACKAGE_DEBUG("Reactive refresh: not needed, Steam already lists lua apps in package {} "
                              "(first seen on app {})", kInjectedPackageId, g_refreshSignals.FirstFreshApp());
        } else if (r.fresh) {
            LOG_PACKAGE_INFO("Reactive refresh: Steam lists lua apps in package {} after {} extra refresh(es), "
                             "{} ms after the initial one (stale first seen on app {}, fresh first seen on app {})",
                             kInjectedPackageId, r.refreshes, ms, g_refreshSignals.FirstStaleApp(),
                             g_refreshSignals.FirstFreshApp());
        } else if (r.refreshes > 0) {
            LOG_PACKAGE_WARN("Reactive refresh: gave up after {} refresh(es) in {} ms; lua apps still read as "
                             "in no package (first: app {})", r.refreshes, ms, g_refreshSignals.FirstStaleApp());
        } else if (r.stale) {
            LOG_PACKAGE_WARN("Reactive refresh: stale answer seen only at the end of the {} ms window "
                             "(first: app {}), no refreshes left", ms, g_refreshSignals.FirstStaleApp());
        } else {
            LOG_PACKAGE_DEBUG("Reactive refresh: no lua app was looked up in {} ms, nothing to do", ms);
        }
    }

    // Thread entry. Nothing may escape a thread of ours: it would terminate Steam.
    void ReactiveRefreshThreadMain() noexcept {
        try {
            ReactiveRefreshWorker();
        } catch (...) {
            try {
                LOG_PACKAGE_WARN("Reactive refresh: worker stopped by an exception");
            } catch (...) {}
        }
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

#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
    // Diagnostics (trace): is the injected package's AppIdVec in ascending order? Steam may look
    // ids up in it with a binary search; nothing here reorders anything. One line per mutation,
    // never the ids themselves (the vector has hundreds). `preExisting` is the size before this
    // mutation's appends (0 = unknown, skips the per-part split). Caller holds g_appIdVecMutex.
    void LogAppIdVecOrder(const char* when, const CUtlVector<AppId_t>& vec, uint32 preExisting) {
        const AppId_t* p = vec.m_Memory.m_pMemory;
        const auto all = AppIdVecOrderLogic::AnalyzeOrder(p, vec.m_Size);
        if (all.Sorted()) {
            LOG_PACKAGE_TRACE("AppIdVec after {} is sorted. Size: {}.", when, vec.m_Size);
            return;
        }
        if (preExisting > 0 && preExisting < vec.m_Size) {
            const auto before   = AppIdVecOrderLogic::AnalyzeOrder(p, preExisting);
            const auto appended = AppIdVecOrderLogic::AnalyzeOrder(p + preExisting, vec.m_Size - preExisting);
            LOG_PACKAGE_TRACE("AppIdVec is NOT sorted (after {}). Size: {}, inversions: {}, first at [{}]: {} > {}. "
                              "Steam's own part [0,{}): {} inversion(s); appended part: {} inversion(s).",
                              when, vec.m_Size, all.inversions, all.firstAt, all.firstPrev, all.firstNext,
                              preExisting, before.inversions, appended.inversions);
        } else {
            LOG_PACKAGE_TRACE("AppIdVec is NOT sorted (after {}). Size: {}, inversions: {}, first at [{}]: {} > {}.",
                              when, vec.m_Size, all.inversions, all.firstAt, all.firstPrev, all.firstNext);
        }
    }
#endif

    // Caller holds g_appIdVecMutex.
    bool InjectConfiguredDepots(PackageInfo* pPkg) {
        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license.
        [[maybe_unused]] const uint32 preExisting = pPkg->AppIdVec.m_Size;
        // Only ids the vector does not hold yet (no duplicates), ascending. The whole vector is
        // sorted in place below: Steam's own part is ascending and the vector must stay so.
        std::vector<AppId_t> appIds = InjectedAppIdLogic::MissingIds(
            pPkg->AppIdVec.m_Memory.m_pMemory, pPkg->AppIdVec.m_Size, LuaConfig::GetAllDepotIds());
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
#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
        LogAppIdVecOrder("initial injection, before sort", pPkg->AppIdVec, preExisting);
#endif
        InjectedAppIdLogic::SortInPlace(pPkg->AppIdVec.m_Memory.m_pMemory, pPkg->AppIdVec.m_Size);
#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
        LogAppIdVecOrder("initial injection, after sort", pPkg->AppIdVec, 0);
#endif
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
        // Release pairs with CheckAppOwnership's acquire load before oCheckAppOwnership,
        // so pre-injection answers are never misclassified as Stale.
        g_initialRefreshDone.store(true, std::memory_order_release);

        // v1.5.2: this first refresh, made inside the CheckAppOwnership hook right after the
        // license list arrives, was measured not to reach Steam's ownership index. A worker
        // of our own repeats it while the hook keeps seeing lua apps in no package. Using
        // std::thread (_beginthreadex under MSVC) initializes per-thread CRT state cleanly
        // and never blocks this Steam thread.
        if (!g_refreshSignals.StopRequested()) {
            try {
                std::thread(&ReactiveRefreshThreadMain).detach();
            } catch (...) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: could not start the reactive refresh worker");
            }
        }
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

        // Snapshot before oCheckAppOwnership: only answers computed after the initial
        // injection and refresh can be evidence that Steam's ownership index is stale.
        const bool initialRefreshDone = g_initialRefreshDone.load(std::memory_order_acquire);
        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        TryInitFakeLicenseOnce();
        Hooks_Package::TryDumpOwnedDepots();

        if (LuaConfig::HasDepot(appId,false)) {
            // v1.5.2: feed Steam's raw answer (before the override below) to the reactive
            // refresh. Lock-free and non-blocking; the worker acts on it, not this thread.
            const auto observation = ReactiveRefreshLogic::Classify(
                initialRefreshDone, true, pOwn != nullptr,
                pOwn ? pOwn->PackageId : 0, pOwn ? pOwn->ExistInPackageNums : 0, kInjectedPackageId);
            if (g_refreshSignals.Record(observation, appId)) {
                if (observation == ReactiveRefreshLogic::Observation::Stale) {
                    LOG_PACKAGE_TRACE("CheckAppOwnership: Steam's ownership index is stale, lua apps read as in no package (first seen on app {})",
                                      appId);
                } else {
                    LOG_PACKAGE_TRACE("CheckAppOwnership: Steam's ownership index lists package {} (first seen on app {})",
                                      kInjectedPackageId, appId);
                }
            }

            // Diagnostics (Debug + log.level = "trace" only): Steam's own answer
            // before the override below, and which branch it takes.
            LOG_PACKAGE_TRACE("CheckAppOwnership: appId={} result={} pkg={} existInPkgs={} ownsLicense={} "
                              "releaseState={} -> {}",
                              appId, result, pOwn ? pOwn->PackageId : 0,
                              pOwn ? pOwn->ExistInPackageNums : 0,
                              pOwn ? pOwn->bOwnsLicense : false,
                              pOwn ? static_cast<int>(pOwn->ReleaseState) : -1,
                              (result && pOwn && pOwn->ExistInPackageNums > 1)
                                  ? "actually owned (MarkOwned, real result)" : "injected (pkg 0, true)");
            if (result && pOwn && pOwn->ExistInPackageNums > 1) {
                // Actually owned — record so HasDepot excludes it going forward
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
            } else {
                if (pOwn) {
                    pOwn->PackageId    = kInjectedPackageId;
                    pOwn->ReleaseState = EAppReleaseState::Released;
                    pOwn->bOwnsLicense = true; //This forces DLCs on steam family shared games that u dont own when adding their appid via .lua
                    // Setting this free flag to false will hide it from the library UI.
                    pOwn->bFreeLicense = false;
                }
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

    InjectedState GetInjectedState(AppId_t id) {
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) return InjectedState::NotReady;
        std::scoped_lock lock(g_appIdVecMutex);
        return ContainsAppId(pPkg->AppIdVec, id) ? InjectedState::Present : InjectedState::Absent;
    }

    InjectedLayout GetInjectedLayout(AppId_t id) {
        InjectedLayout out;
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) return out;
        std::scoped_lock lock(g_appIdVecMutex);
        const AppId_t* p = pPkg->AppIdVec.m_Memory.m_pMemory;
        const size_t   n = pPkg->AppIdVec.m_Size;
        out.ready  = true;
        out.size   = static_cast<uint32_t>(n);
        out.sorted = AppIdVecOrderLogic::AnalyzeOrder(p, n).Sorted();
        if (const auto pos = AppIdVecOrderLogic::PositionOf(p, n, id)) {
            out.present = true;
            out.pos     = static_cast<uint32_t>(*pos);
        }
        return out;
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
        g_refreshSignals.RequestStop();
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
                // Order-preserving: FindAndFastRemove swaps the last element into the hole and
                // unsorts the vector.
                size_t size = pPkg->AppIdVec.m_Size;
                InjectedAppIdLogic::RemoveKeepingOrder(pPkg->AppIdVec.m_Memory.m_pMemory, size, id);
                pPkg->AppIdVec.m_Size = static_cast<uint32>(size);
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
            if (!plan.additions.empty())   // additions were appended: put the vector back in order
                InjectedAppIdLogic::SortInPlace(pPkg->AppIdVec.m_Memory.m_pMemory, pPkg->AppIdVec.m_Size);
#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
            LogAppIdVecOrder("reconcile", pPkg->AppIdVec, 0);
#endif
        }

        // An applied addition invalidates any UI removal that has not
        // reached the UI thread yet.
        for (AppId_t id : plan.additions)
            Hooks_SteamUI::CancelRemoval(id);

        // Mark package 0 as changed and trigger library refresh. This also fires when
        // a reload left the id set unchanged, as it always has: the file may have
        // changed a manifest pin or a token, and nothing shows Steam does not need it.
        if (!RefreshLicensesFromOwnThread()) {   // watcher thread: serialised with the reactive worker
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
