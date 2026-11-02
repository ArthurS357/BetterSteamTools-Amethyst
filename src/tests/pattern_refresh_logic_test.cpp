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

// ── H-1: working entries must stay identical ────────────────────

namespace {
    using PatternRefreshLogic::EntryMap;
    using PatternRefreshLogic::PatternEntry;

    constexpr uint32_t kWorking = 0x1111;   // resolves today
    constexpr uint32_t kMissing = 0x2222;   // required, failed to resolve

    // Cache as it is now: one working function, the missing one absent.
    EntryMap CachedToday() {
        return { { kWorking, PatternEntry{ "Working", 0x1000, "48 8B ??" } } };
    }
}

TEST(PatternRefreshShouldReplaceCache, RejectReplacementWhenExistingEntryChanges) {
    EntryMap candidate = CachedToday();
    candidate[kWorking].rva = 0x6666;                                   // moves a working hook
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }));
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0));
}

TEST(PatternRefreshShouldReplaceCache, AcceptReplacementWhenOnlyMissingKeysAdded) {
    EntryMap candidate = CachedToday();
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };       // only the fix
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }));
    EXPECT_TRUE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                   /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0));
}

TEST(PatternRefreshShouldReplaceCache, RejectReplacementWhenSignatureChanges) {
    EntryMap candidate = CachedToday();
    candidate[kWorking].sig = "E8 ?? ?? ?? ??";                         // rewrites a working sig
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0));
}

TEST(PatternRefreshShouldReplaceCache, RejectWhenACarriedMissingEntryIsUnresolvable) {
    // Fixes one missing function but ships a bad entry for another: refused,
    // because the next launch would hook the bad one (fail-closed).
    EntryMap candidate = CachedToday();
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    candidate[0x3333]   = PatternEntry{ "AlsoMissing", 0xFFFFFFFF, "" };
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing, 0x3333 },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/1));
}

TEST(PatternRefreshRvaValidation, RvaMustLieInsideTheImage) {
    static_assert(PatternRefreshLogic::IsRvaWithinImage(0x1000, 0x2000));
    EXPECT_FALSE(PatternRefreshLogic::IsRvaWithinImage(0, 0x2000));        // "no rva"
    EXPECT_FALSE(PatternRefreshLogic::IsRvaWithinImage(0x2000, 0x2000));   // one past the end
    EXPECT_FALSE(PatternRefreshLogic::IsRvaWithinImage(0x1000, 0));        // image unknown
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
