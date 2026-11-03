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
    constexpr uint32_t kOptional = 0x4444;  // an optional function the cache never had

    // Loaded module image size used by the H-1 / H-1b tests. Every RVA those
    // tests mean to be "inside the image" is below it.
    constexpr size_t kImage = 0x10000;

    // Cache as it is now: one working function, the missing one absent.
    EntryMap CachedToday() {
        return { { kWorking, PatternEntry{ "Working", 0x1000, "48 8B ??" } } };
    }
}

TEST(PatternRefreshShouldReplaceCache, RejectReplacementWhenExistingEntryChanges) {
    EntryMap candidate = CachedToday();
    candidate[kWorking].rva = 0x6666;                                   // moves a working hook
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0, kImage));
}

TEST(PatternRefreshShouldReplaceCache, AcceptReplacementWhenOnlyMissingKeysAdded) {
    EntryMap candidate = CachedToday();
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };       // only the fix
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    EXPECT_TRUE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                   /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0, kImage));
}

TEST(PatternRefreshShouldReplaceCache, RejectReplacementWhenSignatureChanges) {
    EntryMap candidate = CachedToday();
    candidate[kWorking].sig = "E8 ?? ?? ?? ??";                         // rewrites a working sig
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0, kImage));
}

TEST(PatternRefreshShouldReplaceCache, RejectWhenACarriedMissingEntryIsUnresolvable) {
    // Fixes one missing function but ships a bad entry for another: refused,
    // because the next launch would hook the bad one (fail-closed).
    EntryMap candidate = CachedToday();
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    candidate[0x3333]   = PatternEntry{ "AlsoMissing", 0xFFFFFFFF, "" };
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing, 0x3333 },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/1, kImage));
}

// ── H-1b: entries ADDED outside the missing set ─────────────────
//
// The caller only probes the missing REQUIRED functions, and there is no cached
// entry to compare a brand-new key against -- so before H-1b a mirror could add
// e.g. an optional function with a wild RVA and have it hooked, unvalidated, on
// the next launch.

TEST(PatternRefreshNewEntries, RejectNewOptionalEntryWithRvaOutsideTheImage) {
    EntryMap candidate = CachedToday();
    candidate[kMissing]  = PatternEntry{ "Missing", 0x2000, "" };               // the legitimate fix
    candidate[kOptional] = PatternEntry{ "OptionalHook", 0x0FFFFFFF, "" };      // wild RVA, not "missing"
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    EXPECT_FALSE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                    /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0, kImage));
}

TEST(PatternRefreshNewEntries, AcceptNewOptionalEntryWithRvaInsideTheImage) {
    EntryMap candidate = CachedToday();
    candidate[kMissing]  = PatternEntry{ "Missing", 0x2000, "" };
    candidate[kOptional] = PatternEntry{ "OptionalHook", 0x3000, "" };          // plausible: inside the image
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    EXPECT_TRUE(ShouldReplaceCache(CachedToday(), candidate, { kMissing },
                                   /*bodyDiffers=*/true, /*resolved=*/1, /*unresolvable=*/0, kImage));
}

TEST(PatternRefreshNewEntries, AcceptNewSignatureOnlyEntry) {
    // A sig-only entry has no RVA to trust blindly: the scan finds nothing if the
    // pattern is wrong. It is not refused.
    EntryMap candidate = CachedToday();
    candidate[kMissing]  = PatternEntry{ "Missing", 0x2000, "" };
    candidate[kOptional] = PatternEntry{ "OptionalHook", 0, "48 89 5C 24 ??" };
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
}

TEST(PatternRefreshNewEntries, AcceptNewEntryWithNeitherRvaNorSig) {
    // Nothing to resolve, nothing to hook.
    EntryMap candidate = CachedToday();
    candidate[kOptional] = PatternEntry{ "Empty", 0, "" };
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
}

TEST(PatternRefreshNewEntries, ImageBoundaryIsExclusive) {
    EntryMap candidate = CachedToday();
    candidate[kOptional] = PatternEntry{ "OptionalHook", kImage - 1, "" };      // last valid byte
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    candidate[kOptional].rva = kImage;                                          // one past the end
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
    candidate[kOptional].rva = static_cast<std::uintptr_t>(-1);                 // far outside
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
}

TEST(PatternRefreshNewEntries, UnknownImageSizeFailsClosed) {
    // GetModuleImage failed (imageSize = 0): no new RVA can be validated, so a
    // candidate that adds one is refused ...
    EntryMap withRva = CachedToday();
    withRva[kOptional] = PatternEntry{ "OptionalHook", 0x3000, "" };
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), withRva, { kMissing }, 0));
    // ... but a candidate that adds nothing needing validation is still fine, so a
    // transient GetModuleImage failure does not block an otherwise clean fix.
    EntryMap clean = CachedToday();
    clean[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), clean, { kMissing }, 0));
}

TEST(PatternRefreshNewEntries, EntriesForMissingKeysAreTheCallersJob) {
    // A missing key's entry is probed by the caller (and counted as unresolvable
    // when bad); this predicate must not double-judge it, or a candidate that
    // fixes one function and merely carries an unchanged bad entry for another
    // could never be accepted.
    EntryMap candidate = CachedToday();
    candidate[kMissing] = PatternEntry{ "Missing", 0xFFFFFFFF, "" };            // outside the image
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), candidate, { kMissing }, kImage));
}

TEST(PatternRefreshNewEntries, UnchangedExistingEntriesAreNeverJudgedAgainstTheImage) {
    // An entry the cache already had, byte-identical in the candidate, is today's
    // state: even if its RVA were (oddly) beyond the size we measured, refusing
    // it would break every revalidation for no gain.
    EntryMap cached = { { kWorking, PatternEntry{ "Working", 0x20000, "" } } };   // > kImage
    EntryMap candidate = cached;
    candidate[kMissing] = PatternEntry{ "Missing", 0x2000, "" };
    EXPECT_TRUE(PatternRefreshLogic::EntriesAreConsistent(cached, candidate, { kMissing }, kImage));
}

TEST(PatternRefreshNewEntries, StillRejectsChangedOrDroppedWorkingEntriesWhenNewOnesAreFine) {
    // H-1 keeps working alongside H-1b.
    EntryMap moved = CachedToday();
    moved[kWorking].rva = 0x6666;
    moved[kOptional] = PatternEntry{ "OptionalHook", 0x3000, "" };
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), moved, { kMissing }, kImage));

    EntryMap dropped = { { kOptional, PatternEntry{ "OptionalHook", 0x3000, "" } } };   // kWorking gone
    EXPECT_FALSE(PatternRefreshLogic::EntriesAreConsistent(CachedToday(), dropped, { kMissing }, kImage));
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
