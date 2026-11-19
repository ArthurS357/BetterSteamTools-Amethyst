#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
// The index is one per package, not one per app, so a refresh is global (never one per
// app). But "did it take" is judged per app: an app already in package 0 natively (e.g. the
// Steamworks Common Redistributables) answers "package 0" whatever we inject, so such an
// answer proves nothing. Measured 2026-11-19 (second test): such an app answered "package 0"
// 1.2 s after the first refresh and ended the worker, while South Park (213670) and its
// siblings stayed "in no package" for the rest of the session. A refresh therefore only
// counts as having taken for an app we first saw STALE; the worker stops when every app seen
// stale has since answered "package 0", or when the budget runs out.
namespace ReactiveRefreshLogic {

    inline constexpr int      kMaxAttempts = 10;    // looks (and refreshes) after the initial one, at most
    inline constexpr uint32_t kDebounceMs  = 250;   // before the first look
    inline constexpr uint32_t kIntervalMs  = 1500;  // between looks (so between refreshes): ~15 s in all

    enum class Observation { Irrelevant, Stale, Fresh };

    // Steam's raw answer (before our override) to CheckAppOwnership for an id.
    //  - Irrelevant: the injection is not published yet, no .lua declares the id, or the
    //    answer comes from a real package (an owned app says nothing about package 0).
    //  - Stale: the id is in no package at all, although the injection is published.
    //  - Fresh: Steam reports the id in the injected package. Alone this proves nothing (the
    //    id may be native to the package); Signals only credits it to an app seen Stale.
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
    //
    // Per-app state lives in a fixed open-addressing table. A slot packs
    // (kOccupied | kStaleBit | appId) in one atomic uint64_t: absent = never seen stale (or
    // native, or not asked about), occupied+stale = waiting for the index, occupied only =
    // healed (it answered "package 0" after being stale). Slots are never freed, so a
    // lookup stops at the first empty slot. Fresh answers never insert: a native app costs
    // a few reads of the table and leaves no trace.
    //   ponytail: 4096 slots, apps beyond that are not tracked (the worker still runs for the
    //   ones that are); one slot per distinct lua app ever seen stale, ~660 in the field.
    //
    // Two one-shot latches (first app seen stale, first app that healed) pack
    // (kSetBit | appId) the same way, for logs, so the flag and the appId are published
    // together (no window where flag=true and appId=0).
    struct Signals {
        static constexpr uint64_t kSetBit    = uint64_t{1} << 32;
        static constexpr uint64_t kOccupied  = uint64_t{1} << 32;
        static constexpr uint64_t kStaleBit  = uint64_t{1} << 33;
        static constexpr uint32_t kCapacity  = 4096;   // power of two
        static constexpr uint32_t kHomeShift = 32 - 12;

        std::atomic<uint64_t> firstStaleSlot{0};
        std::atomic<uint64_t> firstHealedSlot{0};
        std::atomic<bool>     stop{false};
        std::atomic<uint64_t> slots[kCapacity]{};

        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "Signals slots must be lock-free for CheckAppOwnership hook");
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "Signals::stop must be lock-free for Uninstall/DllMain");
        static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

        struct Counts {
            size_t everStale = 0;   // apps seen stale at least once
            size_t pending   = 0;   // of those, still waiting for "package 0"
        };

        [[nodiscard]] bool     HasStale()       const noexcept { return firstStaleSlot.load(std::memory_order_acquire) != 0; }
        [[nodiscard]] bool     HasHealed()      const noexcept { return firstHealedSlot.load(std::memory_order_acquire) != 0; }
        [[nodiscard]] uint32_t FirstStaleApp()  const noexcept { return static_cast<uint32_t>(firstStaleSlot.load(std::memory_order_acquire)); }
        [[nodiscard]] uint32_t FirstHealedApp() const noexcept { return static_cast<uint32_t>(firstHealedSlot.load(std::memory_order_acquire)); }
        [[nodiscard]] bool     StopRequested()  const noexcept { return stop.load(std::memory_order_acquire); }

        void RequestStop() noexcept {
            // Release pairs with StopRequested()'s acquire load in the worker.
            stop.store(true, std::memory_order_release);
        }

        // Worker side (not the hot path): one pass over the table.
        [[nodiscard]] Counts Count() const noexcept {
            Counts c;
            for (const auto& slot : slots) {
                const uint64_t v = slot.load(std::memory_order_acquire);
                if (v == 0) continue;
                ++c.everStale;
                if (v & kStaleBit) ++c.pending;
            }
            return c;
        }

        // The apps still waiting for "package 0", ascending.
        [[nodiscard]] std::vector<uint32_t> StillStale() const {
            std::vector<uint32_t> out;
            for (const auto& slot : slots) {
                const uint64_t v = slot.load(std::memory_order_acquire);
                if (v & kStaleBit) out.push_back(static_cast<uint32_t>(v));
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // True only the first time this kind of observation is credited (for one-shot logs).
        // Stale: marks the app as waiting (again, if it had healed). Fresh: credited only if
        // the app was waiting; a native app, or one never seen stale, returns false.
        bool Record(Observation o, uint32_t appId) noexcept {
            switch (o) {
                case Observation::Stale: MarkStale(appId); return Latch(firstStaleSlot, appId);
                case Observation::Fresh: return MarkHealed(appId) && Latch(firstHealedSlot, appId);
                case Observation::Irrelevant: return false;
            }
            return false;
        }

    private:
        static uint32_t Home(uint32_t appId) noexcept { return (appId * 2654435761u) >> kHomeShift; }
        static uint32_t AppOf(uint64_t v) noexcept { return static_cast<uint32_t>(v); }

        void MarkStale(uint32_t appId) noexcept {
            uint32_t i = Home(appId);
            for (uint32_t n = 0; n < kCapacity; ++n, i = (i + 1) & (kCapacity - 1)) {
                uint64_t v = slots[i].load(std::memory_order_acquire);
                for (;;) {
                    if (v == 0) {
                        if (slots[i].compare_exchange_strong(v, kOccupied | kStaleBit | appId,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire))
                            return;
                        continue;   // lost the slot; v now holds what won it
                    }
                    if (AppOf(v) != appId) break;   // someone else's slot: probe on
                    if (v & kStaleBit) return;      // already waiting: a read, no write
                    if (slots[i].compare_exchange_strong(v, v | kStaleBit,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
                        return;
                }
            }
        }

        // True if the app was waiting and is now healed.
        bool MarkHealed(uint32_t appId) noexcept {
            uint32_t i = Home(appId);
            for (uint32_t n = 0; n < kCapacity; ++n, i = (i + 1) & (kCapacity - 1)) {
                uint64_t v = slots[i].load(std::memory_order_acquire);
                if (v == 0) return false;   // never seen stale
                if (AppOf(v) != appId) continue;
                while (v & kStaleBit) {
                    if (slots[i].compare_exchange_strong(v, v & ~kStaleBit,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
                        return true;
                }
                return false;
            }
            return false;
        }

        static bool Latch(std::atomic<uint64_t>& slot, uint32_t appId) noexcept {
            if (slot.load(std::memory_order_relaxed) != 0) return false;
            uint64_t expected = 0;
            return slot.compare_exchange_strong(expected, kSetBit | appId,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed);
        }
    };

    enum class Action { Wait, Refresh, Stop };

    // Decision at each look. Every app seen stale has healed: done. The attempt budget is
    // spent: give up. Otherwise refresh only once some app has been seen stale (no evidence,
    // no refresh); an app that went stale and is still stale keeps the refreshes coming.
    [[nodiscard]] constexpr Action NextAction(int looksDone, size_t everStale, size_t pending) noexcept {
        if (everStale > 0 && pending == 0) return Action::Stop;
        if (looksDone >= kMaxAttempts) return Action::Stop;
        return everStale > 0 ? Action::Refresh : Action::Wait;
    }

    // "[213670, 213671, ... +40 more]": the list for the give-up log, bounded.
    [[nodiscard]] inline std::string FormatAppList(const std::vector<uint32_t>& apps,
                                                   size_t maxShown = 16) {
        std::string out = "[";
        const size_t shown = std::min(apps.size(), maxShown);
        for (size_t i = 0; i < shown; ++i) {
            if (i) out += ", ";
            out += std::to_string(apps[i]);
        }
        if (apps.size() > shown) out += ", ... +" + std::to_string(apps.size() - shown) + " more";
        out += "]";
        return out;
    }

    struct Summary {
        int  looks     = 0;       // looks that did not stop the loop
        int  refreshes = 0;
        bool stale     = false;   // some app was seen stale
        bool healed    = false;   // ...and every such app has since answered "package 0"
        bool stopped   = false;
        std::vector<uint32_t> stillStale;   // apps still waiting at the end (empty if healed)
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
            const Signals::Counts c = s.Count();
            const Action a = NextAction(out.looks, c.everStale, c.pending);
            if (a == Action::Stop) break;
            ++out.looks;
            if (a == Action::Refresh) {
                ++out.refreshes;
                refresh(out.refreshes);
            }
        }
        const Signals::Counts c = s.Count();
        out.stale  = c.everStale > 0;
        out.healed = out.stale && c.pending == 0;
        if (c.pending > 0) out.stillStale = s.StillStale();
        return out;
    }

}  // namespace ReactiveRefreshLogic
