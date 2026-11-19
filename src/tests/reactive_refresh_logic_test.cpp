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
        EXPECT_FALSE(s.HasFresh());

        EXPECT_TRUE(s.Record(Observation::Stale, 391540));
        EXPECT_FALSE(s.Record(Observation::Stale, 1868140));
        EXPECT_EQ(s.FirstStaleApp(), 391540u);

        EXPECT_TRUE(s.Record(Observation::Fresh, 213670));
        EXPECT_FALSE(s.Record(Observation::Fresh, 391540));
        EXPECT_EQ(s.FirstFreshApp(), 213670u);

        EXPECT_FALSE(s.Record(Observation::Irrelevant, 5));
        EXPECT_TRUE(s.HasStale());
        EXPECT_TRUE(s.HasFresh());
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

    // ── NextAction ───────────────────────────────────────────────────────

    TEST(ReactiveRefreshLogicTest, FreshStopsAtOnce) {
        EXPECT_EQ(NextAction(0, true, true), Action::Stop);
        EXPECT_EQ(NextAction(3, true, false), Action::Stop);
    }

    TEST(ReactiveRefreshLogicTest, StaleRefreshesUntilTheBudgetRunsOut) {
        for (int i = 0; i < kMaxAttempts; ++i)
            EXPECT_EQ(NextAction(i, false, true), Action::Refresh) << i;
        EXPECT_EQ(NextAction(kMaxAttempts, false, true), Action::Stop);
    }

    TEST(ReactiveRefreshLogicTest, NoEvidenceMeansNoRefresh) {
        EXPECT_EQ(NextAction(0, false, false), Action::Wait);
        EXPECT_EQ(NextAction(kMaxAttempts, false, false), Action::Stop);
    }

    // ── RunWorker (the worker loop) ────────────────────────────────────────────

    struct FakeClock {
        std::vector<uint32_t> sleeps;
        uint32_t now = 0;
        void operator()(uint32_t ms) { sleeps.push_back(ms); now += ms; }
    };

    // The 2026-11-19 case: stale at injection, the first extra refresh fixes the index.
    TEST(ReactiveRefreshLogicTest, OneRefreshThatTakesStopsAtTheNextLook) {
        Signals s;
        s.Record(Observation::Stale, 213670);
        FakeClock clock;
        std::vector<uint32_t> refreshAt;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) {
            refreshAt.push_back(clock.now);
            s.Record(Observation::Fresh, 213670);   // Steam answers "package 0" right after
        });

        EXPECT_TRUE(r.fresh);
        EXPECT_EQ(r.refreshes, 1);
        ASSERT_EQ(refreshAt.size(), 1u);
        EXPECT_EQ(refreshAt[0], kDebounceMs);       // first refresh after the debounce only
        EXPECT_EQ(clock.sleeps, (std::vector<uint32_t>{kDebounceMs, kIntervalMs}));
    }

    // The index never refreshes: exactly kMaxAttempts refreshes, at least kIntervalMs apart,
    // then the loop ends on its own.
    TEST(ReactiveRefreshLogicTest, GivesUpAfterTheBudgetWithTheIntervalRespected) {
        Signals s;
        s.Record(Observation::Stale, 391540);
        FakeClock clock;
        std::vector<uint32_t> refreshAt;
        std::vector<int> attempts;

        const Summary r = RunWorker(s, std::ref(clock), [&](int attempt) {
            attempts.push_back(attempt);
            refreshAt.push_back(clock.now);
        });

        EXPECT_FALSE(r.fresh);
        EXPECT_TRUE(r.stale);
        EXPECT_FALSE(r.stopped);
        EXPECT_EQ(r.refreshes, kMaxAttempts);
        EXPECT_EQ(attempts, (std::vector<int>{1, 2, 3, 4, 5}));
        ASSERT_EQ(refreshAt.size(), static_cast<size_t>(kMaxAttempts));
        for (size_t i = 1; i < refreshAt.size(); ++i)
            EXPECT_GE(refreshAt[i] - refreshAt[i - 1], kIntervalMs);
        EXPECT_EQ(clock.sleeps.size(), static_cast<size_t>(kMaxAttempts + 1));   // bounded
    }

    // Healthy start: the index already lists the lua apps, nothing is refreshed.
    TEST(ReactiveRefreshLogicTest, AlreadyFreshMeansNoRefreshAtAll) {
        Signals s;
        s.Record(Observation::Fresh, 391540);
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_TRUE(r.fresh);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(clock.sleeps, (std::vector<uint32_t>{kDebounceMs}));
    }

    // No lua app is asked about at all: the worker waits out its budget and refreshes nothing.
    TEST(ReactiveRefreshLogicTest, NoObservationsMeansNoRefreshAndABoundedWait) {
        Signals s;
        FakeClock clock;
        int calls = 0;

        const Summary r = RunWorker(s, std::ref(clock), [&](int) { ++calls; });

        EXPECT_EQ(calls, 0);
        EXPECT_FALSE(r.fresh);
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

        EXPECT_TRUE(r.fresh);
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
