// Unit tests for AchievementsLogic -- the pure hold/rebuild/skip decisions behind
// the "groups" achievement path in Hook/Hooks_NetPacket.cpp. Extracted so they
// are testable without protobuf, HookMacros or the credential store (and so they
// run in the Linux CI).
//
// Three categories throughout: happy path, malformed/hostile input, and
// false-positive guards (legitimate-but-unusual input that must NOT be rejected).
// The "ExtractionParity" suite re-states, verbatim, the inline expressions this
// logic replaced and checks the extracted functions agree with them over a whole
// input domain -- the integration in Hooks_NetPacket.cpp cannot be compiled off
// Windows, so this is the evidence that the refactor changed no behaviour.

#include "Hook/AchievementsLogic.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace AchievementsLogic;
using namespace std::chrono_literals;

namespace {
    constexpr std::uint32_t kSelf   = 0x00ABCDEF;
    constexpr std::uint32_t kFriend = 0x00123456;

    // A real SteamID64: universe 1 (public) | type 1 (individual) | instance 1.
    constexpr std::uint64_t kSteamIdBase = 0x0110000100000000ull;
}

// ---------------------------------------------------------------------------
// RequestedAccount
// ---------------------------------------------------------------------------
TEST(AchievementsRequestedAccount, HappyPath) {
    // No steamid in the request: the client is asking about itself.
    EXPECT_EQ(RequestedAccount(false, 0, kSelf), kSelf);
    // A steamid: the account is its low 32 bits.
    EXPECT_EQ(RequestedAccount(true, kSteamIdBase | kFriend, kSelf), kFriend);
    EXPECT_EQ(RequestedAccount(true, kSteamIdBase | kSelf, kSelf), kSelf);
}

TEST(AchievementsRequestedAccount, MalformedInput) {
    // has_steamid = false must ignore whatever the (default) field value is.
    EXPECT_EQ(RequestedAccount(false, 0xDEADBEEFCAFEF00Dull, kSelf), kSelf);
    // Universe/type bits only, account 0.
    EXPECT_EQ(RequestedAccount(true, kSteamIdBase, kSelf), 0u);
    // All bits set: the low 32 are all ones, nothing is shifted out of range.
    EXPECT_EQ(RequestedAccount(true, ~0ull, kSelf), 0xFFFFFFFFu);
    // No active user and no steamid -> 0.
    EXPECT_EQ(RequestedAccount(false, 0, 0), 0u);
}

TEST(AchievementsRequestedAccount, FalsePositiveGuards) {
    // The account is the LOW word. A steamid whose HIGH word equals the active
    // account must not be mistaken for it.
    EXPECT_EQ(RequestedAccount(true, (static_cast<std::uint64_t>(kSelf) << 32) | kFriend, kSelf), kFriend);
}

// ---------------------------------------------------------------------------
// ShouldSkipReconstruction -- send side: whose page is it?
// ---------------------------------------------------------------------------
TEST(AchievementsShouldSkipReconstruction, HappyPath) {
    // Own page: rebuild (do not skip).
    EXPECT_FALSE(ShouldSkipReconstruction(kSelf, kSelf));
    // A friend's page: Valve's reply must pass through.
    EXPECT_TRUE(ShouldSkipReconstruction(kFriend, kSelf));
}

TEST(AchievementsShouldSkipReconstruction, MalformedInput) {
    // Nobody is logged in: never reconstruct, even for "account 0 == account 0".
    EXPECT_TRUE(ShouldSkipReconstruction(0, 0));
    EXPECT_TRUE(ShouldSkipReconstruction(kSelf, 0));
    // A request for account 0 while someone IS logged in is not their page.
    EXPECT_TRUE(ShouldSkipReconstruction(0, kSelf));
}

TEST(AchievementsShouldSkipReconstruction, FalsePositiveGuards) {
    // Off-by-one neighbours are other people.
    EXPECT_TRUE(ShouldSkipReconstruction(kSelf + 1, kSelf));
    EXPECT_TRUE(ShouldSkipReconstruction(kSelf - 1, kSelf));
    // The largest account id is still matched exactly.
    EXPECT_FALSE(ShouldSkipReconstruction(0xFFFFFFFFu, 0xFFFFFFFFu));
    EXPECT_FALSE(ShouldSkipReconstruction(1u, 1u));
}

// ---------------------------------------------------------------------------
// ShouldRebuildReply -- recv side
// ---------------------------------------------------------------------------
TEST(AchievementsShouldRebuildReply, HappyPath) {
    EXPECT_TRUE(ShouldRebuildReply(kSelf, /*isLuaGame=*/true));
}

TEST(AchievementsShouldRebuildReply, MalformedInput) {
    // acct 0 = "not the logged-in user's page" (stored by the send hook).
    EXPECT_FALSE(ShouldRebuildReply(0, true));
    EXPECT_FALSE(ShouldRebuildReply(0, false));
}

TEST(AchievementsShouldRebuildReply, FalsePositiveGuards) {
    // An owned (non-lua) game keeps Valve's reply even on the user's own page.
    EXPECT_FALSE(ShouldRebuildReply(kSelf, false));
}

// ---------------------------------------------------------------------------
// ShouldHoldReply -- schema not here yet?
// ---------------------------------------------------------------------------
TEST(AchievementsShouldHoldReply, HappyPath) {
    // Reply beat its schema and there is room: hold it.
    EXPECT_TRUE(ShouldHoldReply(/*schemaCached=*/false, 0));
    EXPECT_TRUE(ShouldHoldReply(false, 5));
}

TEST(AchievementsShouldHoldReply, QueueBoundary) {
    EXPECT_TRUE(ShouldHoldReply(false, kMaxHeldUserAch - 1));    // last free slot
    EXPECT_FALSE(ShouldHoldReply(false, kMaxHeldUserAch));       // full: leave Valve's reply alone
    EXPECT_FALSE(ShouldHoldReply(false, kMaxHeldUserAch + 1));   // over-full (must not underflow/wrap)
    EXPECT_FALSE(ShouldHoldReply(false, static_cast<std::size_t>(-1)));
}

TEST(AchievementsShouldHoldReply, FalsePositiveGuards) {
    // A cached schema means "rebuild now", never "hold", whatever the queue holds.
    EXPECT_FALSE(ShouldHoldReply(true, 0));
    EXPECT_FALSE(ShouldHoldReply(true, kMaxHeldUserAch - 1));
    EXPECT_FALSE(ShouldHoldReply(true, kMaxHeldUserAch));
}

// ---------------------------------------------------------------------------
// ShouldReleaseHeld -- hold timeout
// ---------------------------------------------------------------------------
TEST(AchievementsShouldReleaseHeld, HappyPath) {
    EXPECT_TRUE(ShouldReleaseHeld(kHoldTimeout));        // exactly at the timeout
    EXPECT_TRUE(ShouldReleaseHeld(kHoldTimeout + 1s));
    EXPECT_TRUE(ShouldReleaseHeld(1h));
}

TEST(AchievementsShouldReleaseHeld, MalformedInput) {
    // steady_clock never goes backwards, but a negative age must not release.
    EXPECT_FALSE(ShouldReleaseHeld(-1s));
    EXPECT_FALSE(ShouldReleaseHeld(std::chrono::steady_clock::duration::min()));
}

TEST(AchievementsShouldReleaseHeld, FalsePositiveGuards) {
    EXPECT_FALSE(ShouldReleaseHeld(0ms));
    EXPECT_FALSE(ShouldReleaseHeld(kHoldTimeout - 1ms));   // one tick short of the timeout
    EXPECT_FALSE(ShouldReleaseHeld(1s));
}

// ---------------------------------------------------------------------------
// ShouldKeepCachedSchema -- hash_only replies
// ---------------------------------------------------------------------------
TEST(AchievementsShouldKeepCachedSchema, HappyPath) {
    // An empty (hash_only) reply must not wipe a good cache.
    EXPECT_TRUE(ShouldKeepCachedSchema(0, /*haveCachedSchema=*/true));
}

TEST(AchievementsShouldKeepCachedSchema, FalsePositiveGuards) {
    // First sight of an app: store even an empty reply (nothing to protect).
    EXPECT_FALSE(ShouldKeepCachedSchema(0, false));
    // A real reply always replaces the cache (schema updates must land).
    EXPECT_FALSE(ShouldKeepCachedSchema(1, true));
    EXPECT_FALSE(ShouldKeepCachedSchema(500, true));
    EXPECT_FALSE(ShouldKeepCachedSchema(1, false));
}

// ---------------------------------------------------------------------------
// Policy constants
// ---------------------------------------------------------------------------
TEST(AchievementsPolicy, ConstantsAreSane) {
    static_assert(kMaxHeldUserAch > 0, "a zero-size hold queue would disable the feature");
    static_assert(kHoldTimeout > std::chrono::seconds(0), "a zero timeout would never hold");
    // Held replies must be released well before Steam's own job timeout; 8s was
    // the value validated in the field, pin it so a change is deliberate.
    EXPECT_EQ(kHoldTimeout, 8s);
    EXPECT_EQ(kMaxHeldUserAch, 16u);
}

// ---------------------------------------------------------------------------
// ExtractionParity -- the pre-extraction inline expressions, verbatim
// ---------------------------------------------------------------------------
namespace {
    // HandleSend_GetUserAchievements (before):
    //   target = req.has_steamid() ? (AccountID_t)(req.steamid() & 0xFFFFFFFF) : self;
    //   acct   = (self != 0 && target == self) ? self : 0;
    std::uint32_t OldSendAcct(bool has, std::uint64_t steamid, std::uint32_t self) {
        const std::uint32_t target = has ? static_cast<std::uint32_t>(steamid & 0xFFFFFFFFull) : self;
        return (self != 0 && target == self) ? self : 0;
    }
    // HandleRecv_GetUserAchievementsResponse (before):
    //   if (job.acct == 0 || !HasDepot(job.appId)) return;     // true = proceed
    bool OldProceed(std::uint32_t acct, bool lua) { return !(acct == 0 || !lua); }
}

TEST(AchievementsExtractionParity, SendSideAccountSelection) {
    const std::uint32_t selfs[]   = { 0, 1, kSelf, 0xFFFFFFFFu };
    const std::uint64_t ids[]     = { 0, 1, kSelf, kFriend, kSteamIdBase, kSteamIdBase | kSelf,
                                      kSteamIdBase | kFriend, ~0ull, 0x00000000FFFFFFFFull,
                                      (static_cast<std::uint64_t>(kSelf) << 32) };
    for (std::uint32_t self : selfs)
        for (bool has : { false, true })
            for (std::uint64_t id : ids) {
                const std::uint32_t target = RequestedAccount(has, id, self);
                const std::uint32_t acct   = ShouldSkipReconstruction(target, self) ? 0 : self;
                EXPECT_EQ(acct, OldSendAcct(has, id, self))
                    << "self=" << self << " has=" << has << " id=" << id;
            }
}

TEST(AchievementsExtractionParity, RecvSideProceed) {
    for (std::uint32_t acct : { 0u, 1u, kSelf })
        for (bool lua : { false, true })
            EXPECT_EQ(ShouldRebuildReply(acct, lua), OldProceed(acct, lua))
                << "acct=" << acct << " lua=" << lua;
}

TEST(AchievementsExtractionParity, HoldDecision) {
    // Before: if (!Fill(...)) { if (held.size() >= kMax) return; /* else hold */ }
    //         -> held iff the schema was NOT cached AND size < kMax.
    for (bool cached : { false, true })
        for (std::size_t n = 0; n <= kMaxHeldUserAch + 2; ++n) {
            const bool oldHeld = !cached && !(n >= kMaxHeldUserAch);
            EXPECT_EQ(ShouldHoldReply(cached, n), oldHeld) << "cached=" << cached << " n=" << n;
        }
}

TEST(AchievementsExtractionParity, ReleaseAndKeepCache) {
    // Before: now - h->at >= kHoldTimeout
    for (auto ms : { -1000, 0, 1, 7999, 8000, 8001, 60000 }) {
        const auto age = std::chrono::milliseconds(ms);
        EXPECT_EQ(ShouldReleaseHeld(age), age >= std::chrono::seconds(8)) << "ms=" << ms;
    }
    // Before: schema.empty() && g_GameAchSchema.count(appId)
    for (std::size_t n : { std::size_t{0}, std::size_t{1}, std::size_t{40} })
        for (bool have : { false, true })
            EXPECT_EQ(ShouldKeepCachedSchema(n, have), (n == 0 && have)) << "n=" << n << " have=" << have;
}
