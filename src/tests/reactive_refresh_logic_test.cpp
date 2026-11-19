// Unit tests for ReactiveRefreshLogic (Hook/ReactiveRefreshLogic.h): the v1.5.2 reactive
// license refresh. Classify turns Steam's raw CheckAppOwnership answer into an observation;
// RunWorker is the worker loop with the clock and the refresh injected.

#include "Hook/ReactiveRefreshLogic.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

using namespace ReactiveRefreshLogic;

namespace {

    constexpr uint32_t kPkg0  = 0;
    constexpr uint32_t kNoPkg = 4294967295u;   // what Steam reports for "in no package"

    // ── Classify ─────────────────────────────────────────────────────────

    // The values seen on 2026-11-19: stale before the second refresh, fresh after it.
    TEST(ReactiveRefreshLogicTest, ClassifiesTheMeasuredAnswers) {
        EXPECT_EQ(Classify(true, true, true, kNoPkg, 0, kPkg0), Observation::Stale);
        EXPECT_EQ(Classify(true, true, true, kPkg0, 1, kPkg0), Observation::Fresh);
    }

    TEST(ReactiveRefreshLogicTest, NothingCountsBeforeTheInjectionIsPublished) {
        EXPECT_EQ(Classify(false, true, true, kNoPkg, 0, kPkg0), Observation::Irrelevant);
        EXPECT_EQ(Classify(false, true, true, kPkg0, 1, kPkg0), Observation::Irrelevant);
    }

    TEST(ReactiveRefreshLogicTest, IdsNoLuaDeclaresAreIgnored) {
        EXPECT_EQ(Classify(true, false, true, kNoPkg, 0, kPkg0), Observation::Irrelevant);
    }

    TEST(ReactiveRefreshLogicTest, NoAnswerStructIsIgnored) {
        EXPECT_EQ(Classify(true, true, false, kNoPkg, 0, kPkg0), Observation::Irrelevant);
    }

    // An app owned through a real package says nothing about package 0's index.
    TEST(ReactiveRefreshLogicTest, RealPackageAnswersAreNotFresh) {
        EXPECT_EQ(Classify(true, true, true, 54029, 2, kPkg0), Observation::Irrelevant);
        EXPECT_EQ(Classify(true, true, true, 54029, 1, kPkg0), Observation::Irrelevant);
    }

    // Package 0 reported but with no package count is still "in no package".
    TEST(ReactiveRefreshLogicTest, ZeroPackagesIsStaleWhateverThePackageIdSays) {
        EXPECT_EQ(Classify(true, true, true, kPkg0, 0, kPkg0), Observation::Stale);
    }

    // Non-zero injected package id and multi-package fresh count.
    TEST(ReactiveRefreshLogicTest, ClassifiesNonZeroInjectedPackageId) {
        EXPECT_EQ(Classify(true, true, true, 42, 1, 42), Observation::Fresh);
        EXPECT_EQ(Classify(true, true, true, 42, 2, 42), Observation::Fresh);
        EXPECT_EQ(Classify(true, true, true, kPkg0, 1, 42), Observation::Irrelevant);
    }

    // ── Signals ──────────────────────────────────────────────────────────

    TEST(ReactiveRefreshLogicTest, RecordIsOneShotAndKeepsTheFirstApp) {
        Signals s;
        EXPECT_FALSE(s.Record(Observation::Irrelevant, 5));
        EXPECT_FALSE(s.HasStale());
        EXPECT_FALSE(s.HasHealed());

        EXPECT_TRUE(s.Record(Observation::Stale, 391540));
        EXPECT_FALSE(s.Record(Observation::Stale, 1868140));
        EXPECT_EQ(s.FirstStaleApp(), 391540u);

        EXPECT_FALSE(s.Record(Observation::Fresh, 213670));   // never stale: not credited
        EXPECT_FALSE(s.HasHealed());

        EXPECT_TRUE(s.Record(Observation::Fresh, 391540));
        EXPECT_FALSE(s.Record(Observation::Fresh, 391540));
        EXPECT_EQ(s.FirstHealedApp(), 391540u);

        EXPECT_FALSE(s.Record(Observation::Irrelevant, 5));
        EXPECT_TRUE(s.HasStale());
        EXPECT_TRUE(s.HasHealed());
    }

    // Regression for the 2026-11-19 second test: an app native to package 0 (Steamworks
    // Common Redistributables, 228983...) answers "package 0" whatever we inject. It must
    // leave no trace, so it can never close the latch for the injected apps.
    TEST(ReactiveRefreshLogicTest, ANativeAppAnsweringFreshLeavesNoTrace) {
        Signals s;
        EXPECT_FALSE(s.Record(Observation::Fresh, 228983));
        EXPECT_FALSE(s.Record(Observation::Fresh, 228986));
        EXPECT_FALSE(s.HasHealed());
        EXPECT_EQ(s.Count().everStale, 0u);
        EXPECT_EQ(s.Count().pending, 0u);

        s.Record(Observation::Stale, 213670);
        EXPECT_FALSE(s.Record(Observation::Fresh, 228983));   // still not credited
        EXPECT_EQ(s.Count().everStale, 1u);
        EXPECT_EQ(s.Count().pending, 1u);
        EXPECT_FALSE(s.HasHealed());
    }

    TEST(ReactiveRefreshLogicTest, AnAppThatHealedAndGoesStaleAgainIsPendingAgain) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        s.Record(Observation::Fresh, 213670);
        EXPECT_EQ(s.Count().pending, 0u);
        EXPECT_EQ(s.Count().everStale, 1u);

        s.Record(Observation::Stale, 213670);
        EXPECT_EQ(s.Count().pending, 1u);
        EXPECT_EQ(s.Count().everStale, 1u);   // same slot, not a second app
        EXPECT_EQ(s.StillStale(), (std::vector<uint32_t>{213670}));
    }

    // A real load: far more apps than one probe chain, ids that share home slots.
    TEST(ReactiveRefreshLogicTest, ManyAppsAreTrackedIndependently) {
        Signals s;
        std::vector<uint32_t> ids;
        for (uint32_t i = 0; i < 1500; ++i) ids.push_back(213670u + i * 7u);
        for (uint32_t id : ids) s.Record(Observation::Stale, id);
        for (uint32_t id : ids) s.Record(Observation::Stale, id);   // twice: no second slot
        EXPECT_EQ(s.Count().everStale, ids.size());
        EXPECT_EQ(s.Count().pending, ids.size());

        // Heal every other app; the rest stay pending, in ascending order.
        std::vector<uint32_t> expectedPending;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i % 2 == 0) s.Record(Observation::Fresh, ids[i]);
            else expectedPending.push_back(ids[i]);
        }
        EXPECT_EQ(s.Count().everStale, ids.size());
        EXPECT_EQ(s.Count().pending, expectedPending.size());
        EXPECT_EQ(s.StillStale(), expectedPending);
    }

    // The table has a ceiling: past it new apps are dropped, nothing hangs or corrupts.
    TEST(ReactiveRefreshLogicTest, AFullTableDropsNewAppsWithoutHanging) {
        Signals s;
        for (uint32_t i = 1; i <= Signals::kCapacity + 500; ++i) s.Record(Observation::Stale, i);
        EXPECT_EQ(s.Count().everStale, static_cast<size_t>(Signals::kCapacity));
        EXPECT_FALSE(s.Record(Observation::Fresh, Signals::kCapacity + 400));   // dropped: unknown
        EXPECT_TRUE(s.Record(Observation::Fresh, 1));
    }

    // Regression for M-1: concurrent Record() + reader must never observe HasStale()
    // with FirstStaleApp() == 0, and exactly one writer may win the one-shot latch.
    TEST(ReactiveRefreshLogicTest, SignalsLatchIsAtomicUnderContention) {
        constexpr int kRounds = 200;
        constexpr int kWriters = 4;
        for (int round = 0; round < kRounds; ++round) {
            Signals s;
            std::atomic<bool> go{false};
            std::atomic<int> winners{0};
            std::atomic<bool> sawZeroApp{false};

            std::vector<std::thread> threads;
            threads.reserve(kWriters + 1);
            for (int w = 0; w < kWriters; ++w) {
                threads.emplace_back([&, appId = static_cast<uint32_t>(1000 + w)]() {
                    while (!go.load(std::memory_order_acquire)) {}
                    if (s.Record(Observation::Stale, appId))
                        winners.fetch_add(1, std::memory_order_relaxed);
                });
            }
            threads.emplace_back([&]() {
                while (!go.load(std::memory_order_acquire)) {}
                for (int i = 0; i < 64; ++i) {
                    if (s.HasStale() && s.FirstStaleApp() == 0) {
                        sawZeroApp.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            });

            go.store(true, std::memory_order_release);
            for (auto& t : threads) t.join();

            EXPECT_EQ(winners.load(), 1);
            EXPECT_FALSE(sawZeroApp.load());
            EXPECT_TRUE(s.HasStale());
            EXPECT_NE(s.FirstStaleApp(), 0u);
        }
    }

    // Steam threads race on the same apps: each app must end up in exactly one slot (no
    // duplicate from a lost CAS), and healing from other threads must settle at zero pending.
    TEST(ReactiveRefreshLogicTest, ConcurrentMarkingNeverDuplicatesAnApp) {
        constexpr int kRounds = 50;
        constexpr int kThreads = 4;
        constexpr uint32_t kApps = 300;
        for (int round = 0; round < kRounds; ++round) {
            Signals s;
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    while (!go.load(std::memory_order_acquire)) {}
                    for (uint32_t i = 0; i < kApps; ++i) {
                        const uint32_t app = 213670u + ((i + static_cast<uint32_t>(t) * 37u) % kApps);
                        s.Record(Observation::Stale, app);
                    }
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& t : threads) t.join();
            ASSERT_EQ(s.Count().everStale, static_cast<size_t>(kApps));
            ASSERT_EQ(s.Count().pending, static_cast<size_t>(kApps));

            threads.clear();
            go.store(false, std::memory_order_release);
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    while (!go.load(std::memory_order_acquire)) {}
                    for (uint32_t i = 0; i < kApps; ++i)
                        s.Record(Observation::Fresh, 213670u + ((i + static_cast<uint32_t>(t) * 37u) % kApps));
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& t : threads) t.join();
            EXPECT_EQ(s.Count().everStale, static_cast<size_t>(kApps));
            EXPECT_EQ(s.Count().pending, 0u);
        }
    }

    // ── NextAction ───────────────────────────────────────────────────────

    TEST(ReactiveRefreshLogicTest, EveryStaleAppHealedStopsAtOnce) {
        EXPECT_EQ(NextAction(0, 2, 0), Action::Stop);
        EXPECT_EQ(NextAction(3, 1, 0), Action::Stop);
    }

    TEST(ReactiveRefreshLogicTest, PendingRefreshesUntilTheBudgetRunsOut) {
        for (int i = 0; i < kMaxAttempts; ++i)
            EXPECT_EQ(NextAction(i, 1, 1), Action::Refresh) << i;
        EXPECT_EQ(NextAction(kMaxAttempts, 1, 1), Action::Stop);
    }

    TEST(ReactiveRefreshLogicTest, NoEvidenceMeansNoRefresh) {
        EXPECT_EQ(NextAction(0, 0, 0), Action::Wait);
        EXPECT_EQ(NextAction(kMaxAttempts, 0, 0), Action::Stop);
    }

    TEST(ReactiveRefreshLogicTest, TheBudgetCoversAboutFifteenSeconds) {
        EXPECT_EQ(kMaxAttempts, 10);
        EXPECT_EQ(kIntervalMs, 1500u);
        EXPECT_EQ(kDebounceMs + static_cast<uint32_t>(kMaxAttempts) * kIntervalMs, 15250u);
    }

    // ── FormatAppList ────────────────────────────────────────────────────

    TEST(ReactiveRefreshLogicTest, FormatsTheStaleListForTheLog) {
        EXPECT_EQ(FormatAppList({}), "[]");
        EXPECT_EQ(FormatAppList({213670}), "[213670]");
        EXPECT_EQ(FormatAppList({213670, 213671, 3357650}), "[213670, 213671, 3357650]");
        EXPECT_EQ(FormatAppList({1, 2, 3, 4, 5}, 3), "[1, 2, 3, ... +2 more]");
    }

    // ── RunWorker (the worker loop) ────────────────────────────────────────────

    struct FakeClock {
        std::vector<uint32_t> sleeps;
        uint32_t now = 0;
        void operator()(uint32_t ms) { sleeps.push_back(ms); now += ms; }
    };

    // The 2026-11-19 first-trace case: stale at injection, the first extra refresh fixes it.
    TEST(ReactiveRefreshLogicTest, OneRefreshThatTakesStopsAtTheNextLook) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        FakeClock clock;
        std::vector<uint32_t> refreshAt;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) {
            refreshAt.push_back(clock.now);
            s.Record(Observation::Fresh, 213670);   // Steam answers "package 0" right after
        });

        EXPECT_TRUE(r.healed);
        EXPECT_TRUE(r.stillStale.empty());
        EXPECT_EQ(r.refreshes, 1);
        ASSERT_EQ(refreshAt.size(), 1u);
        EXPECT_EQ(refreshAt[0], kDebounceMs);       // first refresh after the debounce only
        EXPECT_EQ(clock.sleeps, (std::vector<uint32_t>{kDebounceMs, kIntervalMs}));
    }

    // The index never refreshes: exactly kMaxAttempts refreshes, at least kIntervalMs apart,
    // then the loop ends on its own and reports which apps never healed.
    TEST(ReactiveRefreshLogicTest, GivesUpAfterTheBudgetAndListsTheStaleApps) {
        Signals s;
        s.Record(Observation::Stale, 391540);
        s.Record(Observation::Stale, 213670);
        FakeClock clock;
        std::vector<uint32_t> refreshAt;
        std::vector<int> attempts;

        const Summary r = RunWorker(s, std::ref(clock), [&](int attempt) {
            attempts.push_back(attempt);
            refreshAt.push_back(clock.now);
        });

        EXPECT_FALSE(r.healed);
        EXPECT_TRUE(r.stale);
        EXPECT_FALSE(r.stopped);
        EXPECT_EQ(r.refreshes, kMaxAttempts);
        EXPECT_EQ(attempts, (std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
        EXPECT_EQ(r.stillStale, (std::vector<uint32_t>{213670, 391540}));
        ASSERT_EQ(refreshAt.size(), static_cast<size_t>(kMaxAttempts));
        for (size_t i = 1; i < refreshAt.size(); ++i)
            EXPECT_GE(refreshAt[i] - refreshAt[i - 1], kIntervalMs);
        EXPECT_EQ(clock.sleeps.size(), static_cast<size_t>(kMaxAttempts + 1));   // bounded
        EXPECT_EQ(clock.now, 15250u);
    }

    // THE regression (2026-11-19, second test): a native app (A) answers "package 0" at once
    // and has never been stale; the injected app (B) is stale. The worker must keep going
    // until B heals, not stop on A.
    TEST(ReactiveRefreshLogicTest, ANativeFreshAppDoesNotStopTheWorkerWhileAnInjectedAppIsStale) {
        constexpr uint32_t kNative = 228983, kInjected = 213670;
        Signals s;
        s.Record(Observation::Stale, kInjected);
        s.Record(Observation::Fresh, kNative);       // native, never stale

        FakeClock clock;
        int refreshes = 0;
        const Summary r = RunWorker(s, std::ref(clock), [&](int n) {
            ++refreshes;
            s.Record(Observation::Fresh, kNative);   // it keeps answering fresh each time
            if (n == 3) s.Record(Observation::Fresh, kInjected);   // the index takes on the 3rd
        });

        EXPECT_EQ(refreshes, 3);                     // not 0 or 1: it went on for B
        EXPECT_TRUE(r.healed);
        EXPECT_TRUE(r.stillStale.empty());
    }

    // Same setup, but B never heals: the native app must not cut the budget short either.
    TEST(ReactiveRefreshLogicTest, ANativeFreshAppDoesNotCutTheBudgetShort) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        FakeClock clock;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) {
            s.Record(Observation::Fresh, 228983);
            s.Record(Observation::Fresh, 229033);
        });

        EXPECT_EQ(r.refreshes, kMaxAttempts);
        EXPECT_FALSE(r.healed);
        EXPECT_EQ(r.stillStale, (std::vector<uint32_t>{213670}));
    }

    // Several injected apps heal at different refreshes: the worker stops after the last one.
    TEST(ReactiveRefreshLogicTest, StopsOnlyWhenTheLastStaleAppHeals) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        s.Record(Observation::Stale, 3357650);
        s.Record(Observation::Stale, 4115450);
        FakeClock clock;

        const Summary r = RunWorker(s, std::ref(clock), [&](int n) {
            if (n == 2) s.Record(Observation::Fresh, 213670);
            if (n == 4) { s.Record(Observation::Fresh, 3357650); s.Record(Observation::Fresh, 4115450); }
        });

        EXPECT_EQ(r.refreshes, 4);
        EXPECT_TRUE(r.healed);
        EXPECT_TRUE(r.stillStale.empty());
    }

    // Some heal, one stays: the give-up list holds only the one that stayed.
    TEST(ReactiveRefreshLogicTest, TheGiveUpListHoldsOnlyTheAppsThatNeverHealed) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        s.Record(Observation::Stale, 3357650);
        FakeClock clock;

        const Summary r = RunWorker(s, std::ref(clock), [&](int n) {
            if (n == 1) s.Record(Observation::Fresh, 3357650);
        });

        EXPECT_EQ(r.refreshes, kMaxAttempts);
        EXPECT_FALSE(r.healed);
        EXPECT_EQ(r.stillStale, (std::vector<uint32_t>{213670}));
    }

    // Something else (a .lua watcher refresh) healed the apps before our first look: done,
    // no refresh of ours needed.
    TEST(ReactiveRefreshLogicTest, HealedByAnotherRefreshMeansNoRefreshOfOurs) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        s.Record(Observation::Fresh, 213670);
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_TRUE(r.healed);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(clock.sleeps, (std::vector<uint32_t>{kDebounceMs}));
    }

    // Healthy start: only apps that were never stale answer "package 0". There is no evidence
    // of a stale index, so nothing is refreshed; the worker just waits out its (sleeping) budget.
    TEST(ReactiveRefreshLogicTest, NeverStaleMeansNoRefreshAtAll) {
        Signals s;
        s.Record(Observation::Fresh, 391540);
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_EQ(calls, 0);
        EXPECT_FALSE(r.stale);
        EXPECT_FALSE(r.healed);
        EXPECT_EQ(clock.sleeps.size(), static_cast<size_t>(kMaxAttempts + 1));
    }

    // No lua app is asked about at all: the worker waits out its budget and refreshes nothing.
    TEST(ReactiveRefreshLogicTest, NoObservationsMeansNoRefreshAndABoundedWait) {
        Signals s;
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_EQ(calls, 0);
        EXPECT_FALSE(r.healed);
        EXPECT_EQ(clock.sleeps.size(), static_cast<size_t>(kMaxAttempts + 1));
    }

    // Staleness can show up after the first look (a lua app asked about later): waits, then
    // refreshes, and the refresh count is still within the budget.
    TEST(ReactiveRefreshLogicTest, StaleSeenLateStillRefreshes) {
        Signals s;
        FakeClock clock;
        int refreshes = 0;

        const Summary r = RunWorker(s, [&](uint32_t ms) {
            clock(ms);
            if (clock.sleeps.size() == 2) s.Record(Observation::Stale, 1868140);   // during the 2nd sleep
        }, [&](int n) {
            ++refreshes;
            if (n == 2) s.Record(Observation::Fresh, 1868140);
        });

        EXPECT_TRUE(r.healed);
        EXPECT_EQ(refreshes, 2);
        EXPECT_LE(r.refreshes, kMaxAttempts);
    }

    // Regression for C-1: RequestStop before the loop exits immediately with zero sleeps
    // and zero refreshes.
    TEST(ReactiveRefreshLogicTest, StopRequestedBeforeStartExitsImmediately) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        s.RequestStop();
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_TRUE(r.stopped);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(clock.sleeps.empty());
    }

    // Regression for C-1: RequestStop during a sleep aborts before running any further
    // refresh (e.g., Hooks_Package::Uninstall while the worker is sleeping).
    TEST(ReactiveRefreshLogicTest, StopRequestedDuringSleepAbortsBeforeNextRefresh) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, [&](uint32_t ms) {
            clock(ms);
            if (clock.sleeps.size() == 2) s.RequestStop();
        }, [&](int) {
            ++calls;
        });

        EXPECT_TRUE(r.stopped);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(r.refreshes, 1);
        EXPECT_EQ(clock.sleeps.size(), 2u);
    }

}  // namespace
