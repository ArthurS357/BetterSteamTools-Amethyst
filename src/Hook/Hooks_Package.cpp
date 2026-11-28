#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "PackageReconcileLogic.h"
#include "Hook/AppIdVecOrderLogic.h"
#include "Hook/GateSkipLogic.h"
#include "Hook/IndexRebuildLogic.h"
#include "Hook/InjectedAppIdLogic.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    // Steam's CUser handler for "appinfo update finished" (see IndexRebuildLogic.h).
    // The upstream pattern TOML does not list it, so its signature lives here. The
    // CUser field offsets are wildcarded; the handler stays correct if they move.
    RESOLVE_FUNC(OnAppInfoUpdated, void, void*, IndexRebuildLogic::AppInfoUpdateMsg*);
    constexpr const char* kOnAppInfoUpdatedSig =
        "83 7A 04 00 77 29 83 7A 08 00 77 23 80 B9 ?? ?? ?? ?? 00 75 12 80 B9 ?? ?? ?? ?? 00 "
        "75 09 83 B9 ?? ?? ?? ?? 00 74 11 C6 81 ?? ?? ?? ?? 01 C3 66 C7 81 ?? ?? ?? ?? 01 01 C3";

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    std::atomic<void*> g_pCUser{nullptr};
    // Published only after the initial fill, so non-null also means "initialized".
    // CheckAppOwnership's fast path, which runs on every call, reads just this.
    // RevalidateInjectedPackage (M3) sets it back to null, under g_appIdVecMutex, when
    // Steam replaced the object; the next CheckAppOwnership fills the new one.
    std::atomic<PackageInfo*> g_pInjectedPackageInfo{nullptr};

    // Serialises every access THIS DLL makes to the injected package's AppIdVec and to
    // g_pInjectedPackageInfo's value as seen by the diagnostics: the initial fill, the
    // reconcile, the owned-depot dump, the pointer revalidation. A plain mutex because
    // almost all of them write. Order: g_appIdVecMutex -> LuaConfig's g_stateMutex (see
    // LuaConfig.cpp). Never hold it across MarkLicenseAsChanged or GetPackageInfo: those
    // run Steam code that can re-enter CheckAppOwnership.
    //
    // M1 (v1.6.0): every write to the vector now runs inside CheckAppOwnership, on a Steam
    // thread, in Steam's own ownership-query path; the .lua and config watchers only raise
    // g_licenseChangePending. ponytail: CheckAppOwnership is called from more than one Steam
    // thread, and Steam's readers take no lock we can take, so a reader on ANOTHER Steam
    // thread can still see the vector mid-edit. What is gone is the writer on a thread
    // Steam never schedules against. Upgrade path, if a trace ever shows that race: run the
    // drain only on the thread that runs ProcessPendingLicenseUpdates (CUser's RunFrame).
    std::mutex g_appIdVecMutex;

    // Raised by NotifyLicenseChanged (watcher threads), consumed by the next CheckAppOwnership.
    std::atomic<bool> g_licenseChangePending{false};

    // M3 (v1.6.0): how often CheckAppOwnership asks Steam again for package 0, to notice
    // that Steam replaced the PackageInfo the injection lives in. ponytail: a fixed interval;
    // between a replacement and the next check, readers still see the old pointer. A hook on
    // Steam's package-cache update would close that window; no such signature is known.
    constexpr int64_t kPackageRecheckIntervalMs = 2000;
    std::atomic<int64_t> g_nextPackageRecheckMs{0};

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    // Marks package 0 changed and raises CUser's flags; Steam's RunFrame then runs
    // ProcessPendingLicenseUpdates and rebuilds the ownership index on its own thread.
    // Neither of those is ever called from here: the index has no lock. Both callers run
    // inside CheckAppOwnership on a Steam thread (since v1.6.0 also the .lua reload, M4).
    // ponytail: the trigger is one unlocked aligned 16-bit store into CUser. A race with
    // RunFrame clearing the run flag is harmless: PPLU clears the rebuild flag before it
    // rebuilds (0x9FA7DC, then the call at 0x9FA7E4), so a flag raised meanwhile is never
    // lost.
    bool MarkLicenseAsChangedAndRequestRebuild() {
        void* user = g_pCUser;
        if (!user || !oMarkLicenseAsChanged) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndRequestRebuild: dependencies not ready, skipping");
            return false;
        }
        // A missing trigger (signature not found, warned once at Install) must not take
        // the mark, or NotifyLicenseChanged's UI removals, down with it.
        IndexRebuildLogic::MarkAndRequestRebuild(user,
            [](void* u) { oMarkLicenseAsChanged(u, kInjectedPackageId, true); },
            [](void* u, IndexRebuildLogic::AppInfoUpdateMsg* m) { if (oOnAppInfoUpdated) oOnAppInfoUpdated(u, m); });
        if (oOnAppInfoUpdated)
            LOG_PACKAGE_INFO("Ownership index rebuild requested: package {} marked changed, CUser rebuild flag set",
                             kInjectedPackageId);
        else
            LOG_PACKAGE_WARN("Package {} marked changed; rebuild flag NOT set (OnAppInfoUpdated unresolved)",
                             kInjectedPackageId);
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

    // Set while Steam's license pass (ProcessPendingLicenseUpdates) runs on this thread; the
    // gate section below says what it holds. Declared here because the helpers that edit
    // package 0 (fill, revalidate, drain) stay out of the pass: a CheckAppOwnership nested
    // in it must not grow or swap the vector the pass may be walking further up the stack.
    // Their work is simply left for the next call outside the pass.
    struct GateWindow {
        GateSkipLogic::Marks<CAppData> marks;
        void* cache = nullptr;   // the appinfo cache GetOrAddAppData was called on
    };
    thread_local GateWindow* t_gate = nullptr;

    bool TryInitFakeLicenseOnce() {
        if (g_pInjectedPackageInfo) return true;
        if (t_gate || !CAPTURE_READY(GetPackageInfo)) return false;

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

        MarkLicenseAsChangedAndRequestRebuild();
        return true;
    }

    // M3: Steam can drop the PackageInfo of package 0 and build a new one (a package-cache
    // update); the injected ids live only in the old one. Asks Steam for package 0 at most
    // every kPackageRecheckIntervalMs and, when it hands back another object, re-arms the
    // initial fill: TryInitFakeLicenseOnce then fills the new object and marks it changed.
    // A null answer is "don't know" and keeps the current pointer (re-arming on it would
    // make every CheckAppOwnership retry GetPackageInfo and warn).
    void RevalidateInjectedPackage() {
        PackageInfo* const cached = g_pInjectedPackageInfo;
        if (!cached || t_gate || !CAPTURE_READY(GetPackageInfo)) return;

        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t due = g_nextPackageRecheckMs.load();
        if (now < due) return;
        // One thread per interval does the check; the others go on.
        if (!g_nextPackageRecheckMs.compare_exchange_strong(due, now + kPackageRecheckIntervalMs)) return;

        PackageInfo* const live = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
        if (!live || live == cached) return;

        std::scoped_lock lock(g_appIdVecMutex);
        if (g_pInjectedPackageInfo != cached) return;   // another thread already re-armed
        g_pInjectedPackageInfo = nullptr;
        LOG_PACKAGE_WARN("Package {} was rebuilt by Steam ({} -> {}); re-injecting the configured ids",
                         kInjectedPackageId, static_cast<void*>(cached), static_cast<void*>(live));
    }

    void ApplyLicenseChange();

    // M1/M4: the watchers only flag the change; it is applied here, on the Steam thread that
    // runs CheckAppOwnership. Applied only once the injection exists: before that the flag
    // stays up, and the initial fill reads the full .lua set anyway.
    // Nothing may unwind out of the detour into Steam's frames: on any exception the change
    // goes back to pending and is retried on the next call.
    void DrainLicenseChange() {
        if (t_gate || !g_licenseChangePending.load() || !g_pInjectedPackageInfo) return;
        if (!g_licenseChangePending.exchange(false)) return;   // another thread took it
        try {
            ApplyLicenseChange();
        } catch (...) {
            g_licenseChangePending = true;
            LOG_PACKAGE_ERROR("NotifyLicenseChanged: applying the .lua change threw; retried on the next ownership query");
        }
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


    // ── The license pass's appinfo gate ──────────────────────────────────────
    // Steam's RunFrame runs ProcessPendingLicenseUpdates when our trigger raises the flags.
    // That pass rebuilds the ownership index only if every app of every licensed package has
    // appinfo or is flagged "known unknown" (CAppData::bSkipFlag): it asks
    // GetOrAddAppData(app) for each one and gives up at an empty-SHA, unflagged entry. The
    // ids of the injected package that are really depot ids never get appinfo, so the pass
    // would give up every time. While it runs, those entries are flagged; GateSkipLogic.h says
    // why only then. Neither hook runs on any thread but the one inside the pass, for any
    // longer than a flag read.
    //
    // The marks live on the pass's own stack and a trivial thread_local pointer reaches them:
    // nothing is shared between threads, and a nested pass joins the outer window instead of
    // closing it. The CAppData pointers are kept only as identities: Restore looks each id
    // up again (look-up only, never creates) and clears the flag only on the same entry, so an
    // AppData Steam freed or replaced during the pass is never written (M2, v1.6.0).
    // GateWindow / t_gate are declared above TryInitFakeLicenseOnce.

    // The third argument is Steam's "look up only": nonzero returns null for an unknown app
    // instead of adding it (4DF790: `test bpl,bpl / jne` straight to the epilogue). The gate
    // passes 0, so it also creates the blank entry we then flag; Restore passes 1.
    HOOK_FUNC(GetOrAddAppData, CAppData*, void* pCache, AppId_t appId, bool bLookupOnly) {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bLookupOnly);
        // Restore looks ids up in one cache; an entry of any other cache is never raised,
        // since it could not be put back.
        if (t_gate && (!t_gate->cache || t_gate->cache == pCache) &&
            t_gate->marks.Raise(appId, pData, [appId] { return LuaConfig::HasDepot(appId, false); })) {
            t_gate->cache = pCache;
            LOG_PACKAGE_TRACE("GetOrAddAppData: skip_flag raised for injected id {} (no appinfo)", appId);
        }
        return pData;
    }

    HOOK_FUNC(ProcessPendingLicenseUpdates, bool, void* pUser) {
        struct Window {
            GateWindow gate;
            bool outermost;
            Window() : outermost(t_gate == nullptr) { if (outermost) t_gate = &gate; }
            ~Window() {
                if (!outermost) return;
                t_gate = nullptr;   // before the look-ups below: they must not raise anything
                [[maybe_unused]] const size_t raised = gate.marks.Count();
                void* const cache = gate.cache;
                [[maybe_unused]] const size_t restored = gate.marks.Restore([cache](uint32_t id) -> CAppData* {
                    return cache ? oGetOrAddAppData(cache, id, /*bLookupOnly=*/true) : nullptr;
                });
                LOG_PACKAGE_TRACE("License pass: skip_flag was raised on {} injected ids with no appinfo; "
                                  "{} restored, {} gone or replaced by Steam (left alone)",
                                  raised, restored, raised - restored);
            }
        } window;
        const bool done = oProcessPendingLicenseUpdates(pUser);
        LOG_PACKAGE_DEBUG("License pass finished: {}", done ? "ownership index rebuilt or nothing pending"
                                                            : "stopped at the appinfo gate");
        return done;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        if (!g_pCUser) {
            g_pCUser = pObj;
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", pObj);
        }

        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        // Order: a rebuilt package re-arms the fill, the fill runs, then a pending .lua
        // change is applied to whichever object is current.
        try {
            RevalidateInjectedPackage();
        } catch (...) {   // logging only can throw here; never into Steam's frames
        }
        TryInitFakeLicenseOnce();
        DrainLicenseChange();
        Hooks_Package::TryDumpOwnedDepots();

        if (pOwn && LuaConfig::HasDepot(appId,false)) {
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

    // The pointer is read under the lock: the M3 re-arm clears it under the same lock.
    InjectedState GetInjectedState(AppId_t id) {
        std::scoped_lock lock(g_appIdVecMutex);
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) return InjectedState::NotReady;
        return ContainsAppId(pPkg->AppIdVec, id) ? InjectedState::Present : InjectedState::Absent;
    }

    InjectedLayout GetInjectedLayout(AppId_t id) {
        InjectedLayout out;
        std::scoped_lock lock(g_appIdVecMutex);
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) return out;
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
        oOnAppInfoUpdated = reinterpret_cast<OnAppInfoUpdated_t>(
            PatternLoader::FindUniqueSignature(client_hModule, kOnAppInfoUpdatedSig));
        if (!oOnAppInfoUpdated)
            LOG_PACKAGE_WARN("OnAppInfoUpdated signature not found (or not unique): "
                             "injected apps stay unowned until Steam's next appinfo update");

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();

        // Optional, and in a transaction of their own: a failed attach here must not drop
        // CheckAppOwnership with it. Without them the trigger still fires, and the index
        // rebuilds as before whenever Steam's own appinfo update flags those ids.
        HOOK_BEGIN();
        INSTALL_HOOK_OPTIONAL_C(ProcessPendingLicenseUpdates);
        INSTALL_HOOK_OPTIONAL_C(GetOrAddAppData);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNINSTALL_HOOK_C(ProcessPendingLicenseUpdates);
        UNINSTALL_HOOK_C(GetOrAddAppData);
        UNHOOK_END();
    }

    void NotifyLicenseChanged() {
        // Watcher thread: nothing of Steam's is touched here (M1/M4). The next
        // CheckAppOwnership applies the queued change on its own thread.
        g_licenseChangePending = true;
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: change queued for the next CheckAppOwnership{}",
                          g_pInjectedPackageInfo ? "" : " (applied after the initial fill)");
    }
}

namespace {
    // Runs inside CheckAppOwnership (DrainLicenseChange), never on a watcher thread.
    void ApplyLicenseChange() {
        // The drain is rare, so it does not wait for the M3 interval: Steam is asked for
        // package 0 right now (no lock held: Steam code), and a replaced object is re-armed
        // instead of written into.
        PackageInfo* const live = CAPTURE_READY(GetPackageInfo)
            ? oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken) : nullptr;

        PackageReconcileLogic::Plan plan;
        {
            // Take and apply under one lock: two Steam threads can drain back to back, and
            // applying each other's queues out of order would lose a change. The pointer is
            // read under it too, so a concurrent re-arm (M3) is seen.
            std::scoped_lock lock(g_appIdVecMutex);
            PackageInfo* pPkg = g_pInjectedPackageInfo;
            if (!pPkg) {   // re-armed meanwhile: keep the change for after the refill
                g_licenseChangePending = true;
                return;
            }
            if (live && live != pPkg) {   // Steam rebuilt it: refill first, then apply
                g_pInjectedPackageInfo = nullptr;
                g_licenseChangePending = true;
                LOG_PACKAGE_WARN("Package {} was rebuilt by Steam ({} -> {}); re-injecting before the .lua change",
                                 kInjectedPackageId, static_cast<void*>(pPkg), static_cast<void*>(live));
                return;
            }

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

        // Mark package 0 as changed and request the index rebuild + library refresh. This
        // also fires when a reload left the id set unchanged, as it always has: the file may
        // have changed a manifest pin or a token, and nothing shows Steam does not need it.
        // M4: on the CheckAppOwnership thread now, the same place the initial fill has
        // always called it from.
        if (!MarkLicenseAsChangedAndRequestRebuild()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
            return;
        }
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", plan.additions.size(), plan.removals.size());

        // Queue UI removals for the steamui RunFrame hook to drain: MarkAppChange belongs
        // to the UI thread, not to this one either.
        for (AppId_t id : plan.removals)
            Hooks_SteamUI::QueueRemoval(id);
    }
}
