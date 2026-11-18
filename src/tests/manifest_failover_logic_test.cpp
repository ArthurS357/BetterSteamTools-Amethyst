// Unit tests for ManifestFailoverLogic.h -- the provider chain, time budget and
// circuit breaker behind ManifestClient's failover (v1.5.0). Everything is driven
// by a fake clock and fake providers: no network, no sleeping.

#include "Utils/SteamMetadata/ManifestFailoverLogic.h"
#include "Utils/SteamMetadata/ManifestUrlLogic.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ManifestFailoverLogic;
using std::chrono::minutes;
using std::chrono::seconds;

namespace {

    struct FakeClock {
        Clock::time_point t{};
        Clock::time_point operator()() const { return t; }
        void Advance(Millis d) { t += d; }
    };

    // What a fake provider does when asked: how long it takes and what it answers.
    struct Behaviour {
        Millis        takes{ 100 };
        bool          transportOk = true;
        std::uint32_t status = 200;
        bool          gotCode = true;
        std::uint64_t code = 4242;
    };

    // A set of fake providers that records who was called and with which deadline.
    struct FakeProviders {
        FakeClock& clock;
        std::vector<std::pair<std::string, Behaviour>> table;
        std::vector<std::string> called;
        std::vector<Millis> grantedBudget;   // deadline - now at each call

        AttemptOutcome operator()(const std::string& provider, Clock::time_point deadline) {
            called.push_back(provider);
            grantedBudget.push_back(std::chrono::duration_cast<Millis>(deadline - clock()));
            for (const auto& [name, b] : table) {
                if (name != provider) continue;
                clock.Advance(b.takes);
                return { b.transportOk, b.status, b.gotCode, b.code };
            }
            ADD_FAILURE() << "unexpected provider " << provider;
            return {};
        }
    };

    Behaviour Answers(std::uint64_t code, Millis takes = Millis{ 100 }) {
        return { takes, true, 200, true, code };
    }
    Behaviour TimesOut(Millis takes) {
        return { takes, false, 0, false, 0 };
    }
    Behaviour Status(std::uint32_t status, Millis takes = Millis{ 100 }) {
        return { takes, true, status, false, 0 };
    }

    ChainResult RunFakeChain(const std::vector<std::string>& chain, CircuitBreaker& breaker,
                    FakeClock& clock, FakeProviders& providers, Millis budget = kTotalBudget) {
        return RunChain(chain, breaker, budget, [&clock] { return clock(); },
                        [&providers](const std::string& p, Clock::time_point d) { return providers(p, d); });
    }

} // namespace

// ── BuildChain ──────────────────────────────────────────────────

TEST(ManifestFailoverChain, DefaultChainStartsWithTheDefaultProvider) {
    EXPECT_EQ(kDefaultChain[0], ManifestUrlLogic::kDefaultProviderName);
}

TEST(ManifestFailoverChain, ConfiguredProviderFirstThenTheOtherDefaults) {
    EXPECT_EQ(BuildChain("wudrm", true, false), (std::vector<std::string>{ "wudrm", "steamrun" }));
    EXPECT_EQ(BuildChain("steamrun", true, false), (std::vector<std::string>{ "steamrun", "wudrm" }));
}

TEST(ManifestFailoverChain, AProviderOutsideTheDefaultsIsKeptAndFollowedByThem) {
    EXPECT_EQ(BuildChain("opensteamtool", true, false),
              (std::vector<std::string>{ "opensteamtool", "wudrm", "steamrun" }));
}

TEST(ManifestFailoverChain, FailoverOffMeansTheConfiguredProviderAlone) {
    EXPECT_EQ(BuildChain("wudrm", false, false), (std::vector<std::string>{ "wudrm" }));
}

TEST(ManifestFailoverChain, AnOverrideIsNeverSwappedForABuiltInProvider) {
    EXPECT_EQ(BuildChain("wudrm", true, true), (std::vector<std::string>{ "wudrm" }));
}

// ── IsProviderDown ──────────────────────────────────────────────

TEST(ManifestFailoverDown, NoResponseAndServerErrorsAreDown) {
    EXPECT_TRUE(IsProviderDown(false, 0));
    EXPECT_TRUE(IsProviderDown(false, 200));   // no transport trumps a stale status
    EXPECT_TRUE(IsProviderDown(true, 500));
    EXPECT_TRUE(IsProviderDown(true, 502));
    EXPECT_TRUE(IsProviderDown(true, 503));
}

TEST(ManifestFailoverDown, ClientErrorsAndSuccessMeanTheProviderIsUp) {
    EXPECT_FALSE(IsProviderDown(true, 200));
    EXPECT_FALSE(IsProviderDown(true, 401));
    EXPECT_FALSE(IsProviderDown(true, 403));
    EXPECT_FALSE(IsProviderDown(true, 404));   // "I do not have that code"
    EXPECT_FALSE(IsProviderDown(true, 499));
}

TEST(ManifestFailoverDown, TooManyRequestsIsNotAQuarantineOffence) {
    // Several depots ask at once, so a 429 can be our own burst; it moves the request
    // on to the next provider but must not bench the provider for 5 minutes.
    EXPECT_FALSE(IsProviderDown(true, 429));
}

// ── ShouldRetryGidOnly ──────────────────────────────────────────

TEST(ManifestFailoverRetry, NeverRetriesWhenTheShapeForbidsIt) {
    EXPECT_FALSE(ShouldRetryGidOnly(false, false, true, 404, Millis{ 9'000 }));
    EXPECT_FALSE(ShouldRetryGidOnly(false, true, true, 404, Millis{ 9'000 }));
}

TEST(ManifestFailoverRetry, ASingleProviderKeepsTheOldRuleWhateverTheAnswer) {
    // fit = false: exactly the pre-v1.5.0 behaviour, even for a dead provider or no time left.
    EXPECT_TRUE(ShouldRetryGidOnly(true, false, false, 0, Millis::zero()));
    EXPECT_TRUE(ShouldRetryGidOnly(true, false, true, 503, Millis{ -10 }));
    EXPECT_TRUE(ShouldRetryGidOnly(true, false, true, 404, Millis{ 9'000 }));
}

TEST(ManifestFailoverRetry, WithOthersQueuedADeadProviderGetsOneRequestNotTwo) {
    EXPECT_FALSE(ShouldRetryGidOnly(true, true, false, 0, Millis{ 9'000 }));
    EXPECT_FALSE(ShouldRetryGidOnly(true, true, true, 503, Millis{ 9'000 }));
}

TEST(ManifestFailoverRetry, WithOthersQueuedTheRetryNeedsTimeToFinish) {
    EXPECT_TRUE(ShouldRetryGidOnly(true, true, true, 404, kMinAttemptBudget));
    EXPECT_FALSE(ShouldRetryGidOnly(true, true, true, 404, kMinAttemptBudget - Millis{ 1 }));
    EXPECT_FALSE(ShouldRetryGidOnly(true, true, true, 404, Millis{ -5 }));
}

// ── AttemptBudget ───────────────────────────────────────────────

TEST(ManifestFailoverBudget, SplitsWhatIsLeftEvenlyAmongTheRemainingAttempts) {
    EXPECT_EQ(AttemptBudget(Millis{ 10'000 }, 2), Millis{ 5'000 });
    EXPECT_EQ(AttemptBudget(Millis{ 10'000 }, 1), Millis{ 10'000 });
    EXPECT_EQ(AttemptBudget(Millis{ 9'000 }, 3), Millis{ 3'000 });
}

TEST(ManifestFailoverBudget, NeverGivesAnAttemptLessThanTheMinimumWhileTimeRemains) {
    EXPECT_EQ(AttemptBudget(Millis{ 4'000 }, 5), kMinAttemptBudget);
    EXPECT_EQ(AttemptBudget(kMinAttemptBudget, 3), kMinAttemptBudget);
}

TEST(ManifestFailoverBudget, DoesNotStartAnAttemptThatCannotFinish) {
    EXPECT_EQ(AttemptBudget(kMinAttemptBudget - Millis{ 1 }, 1), Millis::zero());
    EXPECT_EQ(AttemptBudget(Millis::zero(), 2), Millis::zero());
    EXPECT_EQ(AttemptBudget(Millis{ -50 }, 2), Millis::zero());
    EXPECT_EQ(AttemptBudget(Millis{ 10'000 }, 0), Millis::zero());
}

// ── CapTimeouts ─────────────────────────────────────────────────

TEST(ManifestFailoverTimeouts, LeavesTimeoutsThatAlreadyFit) {
    const PhaseTimeouts cfg{ 1000, 1000, 2000, 2000 };
    const PhaseTimeouts out = CapTimeouts(cfg, Millis{ 10'000 });
    EXPECT_EQ(out.resolve, 1000u);
    EXPECT_EQ(out.connect, 1000u);
    EXPECT_EQ(out.send, 2000u);
    EXPECT_EQ(out.recv, 2000u);
}

TEST(ManifestFailoverTimeouts, CapsEveryPhaseAtTheBudget) {
    const PhaseTimeouts cfg;   // 5000, 5000, 10000, 10000
    const PhaseTimeouts out = CapTimeouts(cfg, Millis{ 6'000 });
    EXPECT_EQ(out.resolve, 5000u);
    EXPECT_EQ(out.connect, 5000u);
    EXPECT_EQ(out.send, 6000u);
    EXPECT_EQ(out.recv, 6000u);
}

TEST(ManifestFailoverTimeouts, ADefaultConnectSurvivesTheShareOfTwoProviders) {
    // Regression guard (review of v1.5.0): scaling the four phases down in proportion
    // gave a 5 s connect about 0.8 s, less than the ~3 s one lost SYN costs, so a healthy
    // provider on a lossy line would time out and be quarantined.
    const PhaseTimeouts out = CapTimeouts(PhaseTimeouts{}, AttemptBudget(kTotalBudget, 2));
    EXPECT_GE(out.resolve, 3000u);
    EXPECT_GE(out.connect, 3000u);
}

TEST(ManifestFailoverTimeouts, ZeroMeansNoLimitToWinHttpSoItBecomesTheBudget) {
    const PhaseTimeouts cfg{ 0, 5000, 10000, 10000 };
    const PhaseTimeouts out = CapTimeouts(cfg, Millis{ 2'000 });
    EXPECT_EQ(out.resolve, 2000u);
    EXPECT_EQ(out.connect, 2000u);
}

TEST(ManifestFailoverTimeouts, NoPhaseEverBecomesZero) {
    const PhaseTimeouts cfg{ 1, 0, 1, 100000 };
    for (const Millis budget : { Millis{ 1'500 }, Millis::zero(), Millis{ -4'000 } }) {
        const PhaseTimeouts out = CapTimeouts(cfg, budget);
        EXPECT_GE(out.resolve, 1u) << budget.count();
        EXPECT_GE(out.connect, 1u) << budget.count();
        EXPECT_GE(out.send, 1u) << budget.count();
        EXPECT_GE(out.recv, 1u) << budget.count();
    }
}

TEST(ManifestFailoverTimeouts, HugeValuesDoNotOverflow) {
    const PhaseTimeouts cfg{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    const PhaseTimeouts out = CapTimeouts(cfg, Millis{ 7'000 });
    EXPECT_EQ(out.resolve, 7000u);
    EXPECT_EQ(out.recv, 7000u);
    // A budget larger than any 32-bit timeout leaves the configured value alone.
    const PhaseTimeouts same = CapTimeouts(cfg, Millis{ 1LL << 40 });
    EXPECT_EQ(same.send, UINT32_MAX);
}

// ── CircuitBreaker ──────────────────────────────────────────────

TEST(ManifestFailoverBreaker, QuarantinesForFiveMinutesThenLetsTheProviderBackIn) {
    CircuitBreaker breaker;
    const Clock::time_point t0{};
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", t0));

    breaker.RecordDown("wudrm", t0);
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", t0));
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", t0 + minutes(4) + seconds(59)));
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", t0 + minutes(5)));
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", t0 + minutes(6)));
}

TEST(ManifestFailoverBreaker, ProvidersAreIndependent) {
    CircuitBreaker breaker;
    breaker.RecordDown("wudrm", Clock::time_point{});
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", Clock::time_point{}));
    EXPECT_FALSE(breaker.IsQuarantined("steamrun", Clock::time_point{}));
}

TEST(ManifestFailoverBreaker, ANewFailureRestartsTheClock) {
    CircuitBreaker breaker;
    const Clock::time_point t0{};
    breaker.RecordDown("wudrm", t0);
    breaker.RecordDown("wudrm", t0 + minutes(4));
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", t0 + minutes(8)));
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", t0 + minutes(9)));
}

TEST(ManifestFailoverBreaker, AnAnswerLiftsTheQuarantine) {
    CircuitBreaker breaker;
    breaker.RecordDown("wudrm", Clock::time_point{});
    breaker.RecordUp("wudrm");
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", Clock::time_point{}));
    breaker.RecordUp("never-seen");   // harmless
}

TEST(ManifestFailoverBreaker, ConcurrentUseLeavesAConsistentState) {
    CircuitBreaker breaker;
    const Clock::time_point t0{};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&breaker, t0, i] {
            for (int n = 0; n < 500; ++n) {
                breaker.RecordDown(i % 2 ? "wudrm" : "steamrun", t0);
                (void)breaker.IsQuarantined("wudrm", t0);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", t0));
    EXPECT_TRUE(breaker.IsQuarantined("steamrun", t0));
}

// ── EffectiveChain ──────────────────────────────────────────────

TEST(ManifestFailoverEffectiveChain, DropsQuarantinedProviders) {
    CircuitBreaker breaker;
    breaker.RecordDown("wudrm", Clock::time_point{});
    EXPECT_EQ(EffectiveChain({ "wudrm", "steamrun" }, breaker, Clock::time_point{}),
              (std::vector<std::string>{ "steamrun" }));
}

TEST(ManifestFailoverEffectiveChain, KeepsEveryoneWhenEveryoneIsQuarantined) {
    CircuitBreaker breaker;
    breaker.RecordDown("wudrm", Clock::time_point{});
    breaker.RecordDown("steamrun", Clock::time_point{});
    EXPECT_EQ(EffectiveChain({ "wudrm", "steamrun" }, breaker, Clock::time_point{}),
              (std::vector<std::string>{ "wudrm", "steamrun" }));
}

// ── RunChain ────────────────────────────────────────────────────

TEST(ManifestFailoverRun, AnAnsweringPrimaryIsTheOnlyProviderAsked) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Answers(111) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, 111u);
    EXPECT_EQ(r.provider, "wudrm");
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(providers.called, (std::vector<std::string>{ "wudrm" }));
    EXPECT_EQ(providers.grantedBudget[0], Millis{ 5'000 });
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", clock()));
}

TEST(ManifestFailoverRun, ATimedOutPrimaryHandsOverAndIsQuarantined) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", TimesOut(Millis{ 5'000 }) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, 222u);
    EXPECT_EQ(r.provider, "steamrun");
    EXPECT_EQ(r.attempts, 2u);
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", clock()));
    EXPECT_FALSE(breaker.IsQuarantined("steamrun", clock()));
    // The second attempt gets everything the first one left unused.
    ASSERT_EQ(providers.grantedBudget.size(), 2u);
    EXPECT_EQ(providers.grantedBudget[1], Millis{ 5'000 });
}

TEST(ManifestFailoverRun, AFastFailureGivesItsUnusedTimeToTheNextProvider) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Status(404, Millis{ 200 }) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.provider, "steamrun");
    ASSERT_EQ(providers.grantedBudget.size(), 2u);
    EXPECT_EQ(providers.grantedBudget[1], Millis{ 9'800 });
}

TEST(ManifestFailoverRun, ANotFoundFallsThroughWithoutQuarantine) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Status(404) }, { "steamrun", Answers(222) } } };

    (void)RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_FALSE(breaker.IsQuarantined("wudrm", clock()));   // the provider is up, it just lacks the code
}

TEST(ManifestFailoverRun, AnUnparsableOkBodyFallsThroughWithoutQuarantine) {
    FakeClock clock;
    CircuitBreaker breaker;
    Behaviour garbage = Status(200);   // 200 but gotCode = false
    FakeProviders providers{ clock, { { "wudrm", garbage }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.provider, "steamrun");
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", clock()));
}

TEST(ManifestFailoverRun, ACodeOfZeroIsNotAnAnswer) {
    // Steam's receive path treats 0 as "no code", so "0" from a provider must not end the chain.
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Answers(0) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, 222u);
    EXPECT_EQ(r.provider, "steamrun");
    EXPECT_EQ(providers.called.size(), 2u);
}

TEST(ManifestFailoverRun, TooManyRequestsFallsThroughWithoutQuarantine) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Status(429) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", clock()));
}

TEST(ManifestFailoverRun, AServerErrorQuarantinesAndFallsThrough) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", Status(503) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", clock()));
}

TEST(ManifestFailoverRun, WhenEveryProviderFailsTheWholeChainStaysInsideTheBudget) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", TimesOut(Millis{ 5'000 }) }, { "steamrun", TimesOut(Millis{ 5'000 }) } } };
    const Clock::time_point start = clock();

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.attempts, 2u);
    EXPECT_LE(clock() - start, kTotalBudget);
    EXPECT_TRUE(breaker.IsQuarantined("wudrm", clock()));
    EXPECT_TRUE(breaker.IsQuarantined("steamrun", clock()));
}

TEST(ManifestFailoverRun, AProviderThatOverrunsLeavesNoTimeForTheNextOne) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", TimesOut(Millis{ 10'500 }) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(providers.called, (std::vector<std::string>{ "wudrm" }));   // steamrun never started
}

TEST(ManifestFailoverRun, AQuarantinedProviderIsSkippedOnTheNextRequest) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders first{ clock, { { "wudrm", TimesOut(Millis{ 5'000 }) }, { "steamrun", Answers(222) } } };
    (void)RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, first);

    FakeProviders second{ clock, { { "wudrm", Answers(111) }, { "steamrun", Answers(222) } } };
    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, second);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.provider, "steamrun");
    EXPECT_EQ(second.called, (std::vector<std::string>{ "steamrun" }));
    EXPECT_EQ(second.grantedBudget[0], kTotalBudget);   // it is the only attempt left, so it gets everything
}

TEST(ManifestFailoverRun, ThePrimaryComesBackAfterTheQuarantine) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders first{ clock, { { "wudrm", TimesOut(Millis{ 5'000 }) }, { "steamrun", Answers(222) } } };
    (void)RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, first);

    clock.Advance(minutes(5));
    FakeProviders later{ clock, { { "wudrm", Answers(111) }, { "steamrun", Answers(222) } } };
    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, later);

    EXPECT_EQ(r.provider, "wudrm");
    EXPECT_EQ(later.called, (std::vector<std::string>{ "wudrm" }));
}

TEST(ManifestFailoverRun, WithEveryProviderQuarantinedAllAreStillTried) {
    FakeClock clock;
    CircuitBreaker breaker;
    breaker.RecordDown("wudrm", clock());
    breaker.RecordDown("steamrun", clock());
    FakeProviders providers{ clock, { { "wudrm", Answers(111) }, { "steamrun", Answers(222) } } };

    const ChainResult r = RunFakeChain({ "wudrm", "steamrun" }, breaker, clock, providers);

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.provider, "wudrm");
    EXPECT_FALSE(breaker.IsQuarantined("wudrm", clock()));   // it answered, so it is back
}

TEST(ManifestFailoverRun, ASingleProviderChainGetsTheWholeBudgetAndNothingElse) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, { { "wudrm", TimesOut(Millis{ 3'000 }) } } };

    const ChainResult r = RunFakeChain({ "wudrm" }, breaker, clock, providers);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(providers.grantedBudget[0], kTotalBudget);
}

TEST(ManifestFailoverRun, AnEmptyChainDoesNothing) {
    FakeClock clock;
    CircuitBreaker breaker;
    FakeProviders providers{ clock, {} };

    const ChainResult r = RunFakeChain({}, breaker, clock, providers);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.attempts, 0u);
    EXPECT_TRUE(providers.called.empty());
}
