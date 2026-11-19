#pragma once

#include <atomic>
#include <cstdint>

// Pure part of the reactive license refresh (Hooks_Package.cpp), v1.5.2.
//
// Measured on a live Steam (2026-11-19, Debug trace): the MarkLicenseAsChanged +
// ProcessPendingLicenseUpdates that follows the initial injection did not reach Steam's
// ownership index. Steam's raw CheckAppOwnership answer for every lua app stayed
// "in no package" (existInPkgs 0) until a SECOND refresh, run from a watcher thread 83 s
// later, turned it into "package 0, existInPkgs 1" in under a second. While the index was
// stale, BuildDepotDependency saw 0 depots for lua apps, so a download Steam resumed by
// itself at startup was cancelled ("Invalid content configuration").
//
// The fix repeats the refresh from a thread of our own until Steam's raw answers show the
// index is current. The hook only records what it sees (lock-free, never blocks); a worker
// started once after the initial refresh decides when to refresh again.
//
// The index is one per package, not one per app, so the state is global: a per-app retry
// would multiply the same refresh by the number of lua apps.
namespace ReactiveRefreshLogic {

    inline constexpr int      kMaxAttempts = 5;     // refreshes after the initial one, at most
    inline constexpr uint32_t kDebounceMs  = 250;   // before the first look
    inline constexpr uint32_t kIntervalMs  = 1000;  // between looks (so between refreshes)

    enum class Observation { Irrelevant, Stale, Fresh };

    // Steam's raw answer (before our override) to CheckAppOwnership for an id.
    //  - Irrelevant: the injection is not published yet, no .lua declares the id, or the
    //    answer comes from a real package (an owned app says nothing about package 0).
    //  - Stale: the id is in no package at all, although the injection is published.
    //  - Fresh: Steam reports the id in the injected package: its index has the injection.
    // ownsLicense is not an input on purpose: a real but lapsed license reports
    // ownsLicense=false with existInPkgs>=1, and refreshing package 0 would not change it.
    [[nodiscard]] constexpr Observation Classify(bool injectionPublished, bool luaDeclared,
                                                 bool haveAnswer, uint32_t rawPackageId,
                                                 uint32_t rawExistInPkgs,
                                                 uint32_t injectedPackageId) noexcept {
        if (!injectionPublished || !luaDeclared || !haveAnswer) return Observation::Irrelevant;
        if (rawExistInPkgs == 0) return Observation::Stale;
        if (rawPackageId == injectedPackageId) return Observation::Fresh;
        return Observation::Irrelevant;
    }

    // What the hook writes and the worker reads. Lock-free: the hook never waits.
    // Sticky slots: once stale or fresh has been seen it stays seen for this process.
    // Each slot packs (kSetBit | appId) in one atomic uint64_t so the flag and the first
    // appId are published atomically together (no window where flag=true and appId=0).
    struct Signals {
        static constexpr uint64_t kSetBit = uint64_t{1} << 32;

        std::atomic<uint64_t> staleSlot{0};
        std::atomic<uint64_t> freshSlot{0};
        std::atomic<bool>     stop{false};

        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "Signals slots must be lock-free for CheckAppOwnership hook");
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "Signals::stop must be lock-free for Uninstall/DllMain");

        [[nodiscard]] bool     HasStale()      const noexcept { return staleSlot.load(std::memory_order_acquire) != 0; }
        [[nodiscard]] bool     HasFresh()      const noexcept { return freshSlot.load(std::memory_order_acquire) != 0; }
        [[nodiscard]] uint32_t FirstStaleApp() const noexcept { return static_cast<uint32_t>(staleSlot.load(std::memory_order_acquire)); }
        [[nodiscard]] uint32_t FirstFreshApp() const noexcept { return static_cast<uint32_t>(freshSlot.load(std::memory_order_acquire)); }
        [[nodiscard]] bool     StopRequested() const noexcept { return stop.load(std::memory_order_acquire); }

        void RequestStop() noexcept {
            // Release pairs with StopRequested()'s acquire load in the worker.
            stop.store(true, std::memory_order_release);
        }

        // True only the first time this observation latches its slot (for one-shot logs).
        // Called on every CheckAppOwnership for a lua app: a relaxed load first so the hot
        // path is a read once latched, not a write on a shared cache line. Slow path uses
        // acq_rel CAS so any reader observing HasStale()/HasFresh() via acquire sees appId.
        bool Record(Observation o, uint32_t appId) noexcept {
            switch (o) {
                case Observation::Stale: return Latch(staleSlot, appId);
                case Observation::Fresh: return Latch(freshSlot, appId);
                case Observation::Irrelevant: return false;
            }
            return false;
        }

    private:
        static bool Latch(std::atomic<uint64_t>& slot, uint32_t appId) noexcept {
            if (slot.load(std::memory_order_relaxed) != 0) return false;
            uint64_t expected = 0;
            return slot.compare_exchange_strong(expected, kSetBit | appId,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed);
        }
    };

    enum class Action { Wait, Refresh, Stop };

    // Decision at each look. Fresh ends it (success); the attempt budget ends it (give up);
    // otherwise refresh only once a stale answer has been seen (no evidence, no refresh).
    [[nodiscard]] constexpr Action NextAction(int looksDone, bool freshSeen, bool staleSeen) noexcept {
        if (freshSeen) return Action::Stop;
        if (looksDone >= kMaxAttempts) return Action::Stop;
        return staleSeen ? Action::Refresh : Action::Wait;
    }

    struct Summary {
        int  looks     = 0;   // looks that did not stop the loop
        int  refreshes = 0;
        bool fresh     = false;
        bool stale     = false;
        bool stopped   = false;
    };

    // The worker loop, with the sleep and the refresh injected so the timing and the
    // budget are testable without Steam. At most kMaxAttempts looks/refreshes, at least
    // kIntervalMs apart, the first after kDebounceMs; one final look after the last refresh
    // so the summary reports whether it took. Bounded: kMaxAttempts + 1 sleeps. Exits early
    // without refreshing if s.RequestStop() is called (shutdown / DLL unload).
    template <class SleepMs, class Refresh>
    Summary RunWorker(const Signals& s, SleepMs sleepMs, Refresh refresh) {
        Summary out;
        for (;;) {
            if (s.StopRequested()) {
                out.stopped = true;
                break;
            }
            sleepMs(out.looks == 0 ? kDebounceMs : kIntervalMs);
            if (s.StopRequested()) {
                out.stopped = true;
                break;
            }
            const Action a = NextAction(out.looks, s.HasFresh(), s.HasStale());
            if (a == Action::Stop) break;
            ++out.looks;
            if (a == Action::Refresh) {
                ++out.refreshes;
                refresh(out.refreshes);
            }
        }
        out.fresh = s.HasFresh();
        out.stale = s.HasStale();
        return out;
    }

}  // namespace ReactiveRefreshLogic
