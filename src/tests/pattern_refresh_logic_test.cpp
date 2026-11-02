// Unit tests for PatternRefreshLogic -- the policy behind PatternLoader's
// self-healing cache refresh (v1.3.4): WHEN a cached pattern TOML may be
// re-checked against the mirrors, and WHETHER a downloaded copy may replace it.
// Pure: no WinHTTP, filesystem, TOML parser or live module involved.

#include "Utils/SteamMetadata/PatternRefreshLogic.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>

using namespace std::chrono_literals;
using PatternRefreshLogic::Decision;
using PatternRefreshLogic::RevalidateInputs;
using PatternRefreshLogic::ShouldReplaceCache;
using PatternRefreshLogic::ShouldRevalidate;

namespace {
    // A cache hit with one unresolved required function, never revalidated.
    RevalidateInputs BrokenCacheHit() {
        RevalidateInputs in;
        in.enabled        = true;
        in.fromCache      = true;
        in.cacheFallback  = false;
        in.requiredMisses = 1;
        return in;
    }
}

// ── ShouldRevalidate ─────────────────────────────────────────────

TEST(PatternRefreshShouldRevalidate, BrokenCacheHitNeverCheckedRevalidates) {
    EXPECT_EQ(ShouldRevalidate(BrokenCacheHit()), Decision::Revalidate);
}

TEST(PatternRefreshShouldRevalidate, HealthyCacheNeverTouchesNetwork) {
    RevalidateInputs in = BrokenCacheHit();
    in.requiredMisses = 0;
    EXPECT_EQ(ShouldRevalidate(in), Decision::NoRequiredMisses);
}

TEST(PatternRefreshShouldRevalidate, DisabledByConfigWins) {
    RevalidateInputs in = BrokenCacheHit();
    in.enabled = false;
    EXPECT_EQ(ShouldRevalidate(in), Decision::Disabled);
}

TEST(PatternRefreshShouldRevalidate, FreshRemoteBodyIsAlreadyCurrent) {
    RevalidateInputs in = BrokenCacheHit();
    in.fromCache = false;
    EXPECT_EQ(ShouldRevalidate(in), Decision::NotFromCache);
}

TEST(PatternRefreshShouldRevalidate, CacheFallbackMeansNetworkAlreadyFailed) {
    RevalidateInputs in = BrokenCacheHit();
    in.cacheFallback = true;
    EXPECT_EQ(ShouldRevalidate(in), Decision::CacheFallback);
}

TEST(PatternRefreshShouldRevalidate, ThrottledWithinInterval) {
    RevalidateInputs in = BrokenCacheHit();
    in.sinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(1h);
    EXPECT_EQ(ShouldRevalidate(in), Decision::Throttled);
}

TEST(PatternRefreshShouldRevalidate, ThrottleBoundaryIsExclusive) {
    RevalidateInputs in = BrokenCacheHit();
    in.sinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(24h) - 1s;
    EXPECT_EQ(ShouldRevalidate(in), Decision::Throttled);
    in.sinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(24h);
    EXPECT_EQ(ShouldRevalidate(in), Decision::Revalidate);
}

TEST(PatternRefreshShouldRevalidate, StampInTheFutureDoesNotThrottleForever) {
    RevalidateInputs in = BrokenCacheHit();
    in.sinceLastAttempt = -5min;   // clock moved back after the stamp was written
    EXPECT_EQ(ShouldRevalidate(in), Decision::Revalidate);
}

TEST(PatternRefreshShouldRevalidate, CustomIntervalIsHonoured) {
    RevalidateInputs in = BrokenCacheHit();
    in.sinceLastAttempt = 10s;
    EXPECT_EQ(ShouldRevalidate(in, 5s), Decision::Revalidate);
    EXPECT_EQ(ShouldRevalidate(in, 60s), Decision::Throttled);
}

TEST(PatternRefreshShouldRevalidate, UsableInConstantExpressions) {
    constexpr RevalidateInputs in{ true, true, false, 2, std::nullopt };
    static_assert(ShouldRevalidate(in) == Decision::Revalidate);
    EXPECT_STREQ(PatternRefreshLogic::ToString(Decision::Throttled), "throttled");
}

// ── ShouldReplaceCache ──────────────────────────────────────────

TEST(PatternRefreshShouldReplaceCache, StrictImprovementReplaces) {
    const std::unordered_set<uint32_t> cached{ 1, 2 };
    const std::unordered_set<uint32_t> remote{ 1, 2, 3 };
    EXPECT_TRUE(ShouldReplaceCache(cached, remote, /*bodyDiffers=*/true, /*resolved=*/1));
}

TEST(PatternRefreshShouldReplaceCache, IdenticalBodyNeverReplaces) {
    const std::unordered_set<uint32_t> keys{ 1, 2 };
    EXPECT_FALSE(ShouldReplaceCache(keys, keys, /*bodyDiffers=*/false, 1));
}

TEST(PatternRefreshShouldReplaceCache, EmptyOrGarbageRemoteNeverReplaces) {
    const std::unordered_set<uint32_t> cached{ 1 };
    EXPECT_FALSE(ShouldReplaceCache(cached, {}, true, 1));
}

TEST(PatternRefreshShouldReplaceCache, MustResolveAtLeastOneMissingFunction) {
    const std::unordered_set<uint32_t> cached{ 1 };
    const std::unordered_set<uint32_t> remote{ 1, 2 };
    EXPECT_FALSE(ShouldReplaceCache(cached, remote, true, 0));
}

TEST(PatternRefreshShouldReplaceCache, NeverDropsAFunctionTheCacheHad) {
    // Remote fixes the missing function but loses key 2 -> refused.
    const std::unordered_set<uint32_t> cached{ 1, 2 };
    const std::unordered_set<uint32_t> remote{ 1, 3 };
    EXPECT_FALSE(ShouldReplaceCache(cached, remote, true, 1));
}

TEST(PatternRefreshShouldReplaceCache, SameKeysWithFixedSignatureReplaces) {
    // Same function set, but a corrected sig now resolves the missing one.
    const std::unordered_set<uint32_t> keys{ 1, 2, 3 };
    EXPECT_TRUE(ShouldReplaceCache(keys, keys, true, 1));
}

// ── StampFileName ───────────────────────────────────────────────

TEST(PatternRefreshStampFileName, SitsBesideTheCachedToml) {
    EXPECT_EQ(PatternRefreshLogic::StampFileName("abc123"), "abc123.toml.checked");
}

TEST(PatternRefreshTimeouts, AreShortAndBounded) {
    static_assert(PatternRefreshLogic::kRevalidateTimeoutMs <= 2000);
    static_assert(PatternRefreshLogic::kRevalidateRecvTimeoutMs <= 3000);
    static_assert(PatternRefreshLogic::kMinRevalidateInterval >= std::chrono::hours(1));
    SUCCEED();
}
