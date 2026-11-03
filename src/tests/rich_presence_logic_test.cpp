// Unit tests for RichPresenceLogic -- the pure track/inject decisions and the
// binary-KV1 parser behind Hooks_NetPacket_RichPresence (Hook/Hooks_NetPacket.cpp).
// Extracted so they are testable without protobuf, HookMacros, LuaConfig or
// CloudRedirectHost (and so they run in the Linux CI).
//
// Three categories throughout: happy path, malformed/hostile input, and
// false-positive guards (legitimate-but-unusual input that must NOT be rejected).
// The "ExtractionParity" suite re-states, verbatim, the inline expressions this
// logic replaced and checks the extracted functions agree with them over a whole
// input domain -- the integration in Hooks_NetPacket.cpp cannot be compiled off
// Windows, so this is the evidence that the refactor changed no behaviour.

#include "Hook/RichPresenceLogic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace RichPresenceLogic;

namespace {
    constexpr std::uint32_t kLuaGame     = 3764200;   // an unlocked (lua) game
    constexpr std::uint32_t kOtherLua    = 1245620;
    constexpr std::uint32_t kOwnedGame   = 730;       // owned: not in the lua depot list
    constexpr std::uint32_t kOnlineFixId = 480;       // dllmain.h kOnlineFixAppId

    // Builders for Steam's binary KV1 stream.
    std::string Open(const std::string& name) { return std::string(1, '\x00') + name + '\0'; }
    std::string Str(const std::string& k, const std::string& v) {
        return std::string(1, '\x01') + k + '\0' + v + '\0';
    }
    std::string Close() { return std::string(1, '\x08'); }

    KvList Parse(const std::string& s) {
        KvList out;
        ExtractStringKVs(reinterpret_cast<const std::uint8_t*>(s.data()),
                         static_cast<std::uint32_t>(s.size()), out);
        return out;
    }
}

// ---------------------------------------------------------------------------
// AppIdFromGameId
// ---------------------------------------------------------------------------
TEST(RichPresenceAppIdFromGameId, HappyPath) {
    EXPECT_EQ(AppIdFromGameId(kLuaGame), kLuaGame);
    EXPECT_EQ(AppIdFromGameId(0), 0u);
}

TEST(RichPresenceAppIdFromGameId, MalformedInput) {
    // A GameID carries type/mod bits in the high word; only the low 32 are the AppID.
    EXPECT_EQ(AppIdFromGameId(0xFFFFFFFF00000000ull | kLuaGame), kLuaGame);
    EXPECT_EQ(AppIdFromGameId(0x0200000000000000ull), 0u);
    EXPECT_EQ(AppIdFromGameId(~0ull), 0xFFFFFFFFu);
}

TEST(RichPresenceAppIdFromGameId, FalsePositiveGuards) {
    // The boundary: bit 32 is NOT part of the appid.
    EXPECT_EQ(AppIdFromGameId(0x100000000ull), 0u);
    EXPECT_EQ(AppIdFromGameId(0xFFFFFFFFull), 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// ShouldTrackAppId
// ---------------------------------------------------------------------------
TEST(RichPresenceShouldTrackAppId, HappyPath) {
    EXPECT_TRUE(ShouldTrackAppId(kLuaGame, /*isLuaGame=*/true, /*isOnlineFix=*/false));
}

TEST(RichPresenceShouldTrackAppId, MalformedInput) {
    // "No game" is never tracked, even if the depot lookup says yes for 0.
    EXPECT_FALSE(ShouldTrackAppId(0, true, false));
    EXPECT_FALSE(ShouldTrackAppId(0, false, false));
}

TEST(RichPresenceShouldTrackAppId, FalsePositiveGuards) {
    // Owned game on top: the server's natural broadcast paints the cache.
    EXPECT_FALSE(ShouldTrackAppId(kOwnedGame, false, false));
    // -onlinefix is handled by the OnlineFix path, even when the depot lists it.
    EXPECT_FALSE(ShouldTrackAppId(kOnlineFixId, true, true));
    EXPECT_FALSE(ShouldTrackAppId(kOnlineFixId, false, true));
    // The caller owns the "is the online-fix appid" fact; a game that merely has
    // a depot is tracked regardless of its number.
    EXPECT_TRUE(ShouldTrackAppId(1u, true, false));
    EXPECT_TRUE(ShouldTrackAppId(0xFFFFFFFFu, true, false));
}

// ---------------------------------------------------------------------------
// DecideTrackAction
// ---------------------------------------------------------------------------
TEST(RichPresenceDecideTrackAction, HappyPath) {
    // Nothing tracked -> a lua game appears.
    EXPECT_EQ(DecideTrackAction(0, kLuaGame, kLuaGame), TrackAction::InjectGame);
    // Tracked game replaced by another lua game on top.
    EXPECT_EQ(DecideTrackAction(kLuaGame, kOtherLua, kOtherLua), TrackAction::InjectGame);
    // Tracked game quit and the stack went empty -> clear the cache.
    EXPECT_EQ(DecideTrackAction(kLuaGame, 0, 0), TrackAction::InjectClear);
}

TEST(RichPresenceDecideTrackAction, UnchangedNeverReinjects) {
    // Same tracked appid (re-sent GamesPlayed): nothing to do.
    EXPECT_EQ(DecideTrackAction(kLuaGame, kLuaGame, kLuaGame), TrackAction::Unchanged);
    // Nothing tracked before or now, even with an owned game on top.
    EXPECT_EQ(DecideTrackAction(0, 0, 0), TrackAction::Unchanged);
    EXPECT_EQ(DecideTrackAction(0, 0, kOwnedGame), TrackAction::Unchanged);
    EXPECT_EQ(DecideTrackAction(0, 0, kOnlineFixId), TrackAction::Unchanged);
}

TEST(RichPresenceDecideTrackAction, DeferToServerAvoidsFlicker) {
    // A tracked lua game is replaced by an OWNED game on top: do not inject a
    // clear (it would flicker the banner to "Online"); the server paints it.
    EXPECT_EQ(DecideTrackAction(kLuaGame, 0, kOwnedGame), TrackAction::DeferToServer);
    // Same for -onlinefix.
    EXPECT_EQ(DecideTrackAction(kLuaGame, 0, kOnlineFixId), TrackAction::DeferToServer);
}

TEST(RichPresenceDecideTrackAction, EmptyStackIsNotDeferral) {
    // topmost == 0 is the ONLY way to get a clear; any non-zero topmost that is
    // not tracked defers instead.
    EXPECT_EQ(DecideTrackAction(kLuaGame, 0, 0), TrackAction::InjectClear);
    EXPECT_NE(DecideTrackAction(kLuaGame, 0, 1), TrackAction::InjectClear);
}

// ---------------------------------------------------------------------------
// ShouldCaptureRpUpload / ShouldInjectRichPresence
// ---------------------------------------------------------------------------
TEST(RichPresenceShouldCaptureRpUpload, Matrix) {
    EXPECT_TRUE(ShouldCaptureRpUpload(0x0110000100ABCDEFull, kLuaGame));
    EXPECT_FALSE(ShouldCaptureRpUpload(0, kLuaGame));                      // SteamID not captured yet
    EXPECT_FALSE(ShouldCaptureRpUpload(0x0110000100ABCDEFull, 0));         // no tracked game
    EXPECT_FALSE(ShouldCaptureRpUpload(0, 0));
    // Only the SteamID's truthiness matters, not its shape.
    EXPECT_TRUE(ShouldCaptureRpUpload(1, 1));
    EXPECT_TRUE(ShouldCaptureRpUpload(~0ull, 0xFFFFFFFFu));
}

TEST(RichPresenceShouldInjectRichPresence, Matrix) {
    EXPECT_TRUE(ShouldInjectRichPresence(kLuaGame));
    EXPECT_TRUE(ShouldInjectRichPresence(1));
    EXPECT_FALSE(ShouldInjectRichPresence(0));   // not tracking: leave the live push alone
}

// ---------------------------------------------------------------------------
// WithRichPresenceFlag
// ---------------------------------------------------------------------------
TEST(RichPresenceWithRichPresenceFlag, HappyPath) {
    // Game with KVs: bit set so the UI picks them up.
    EXPECT_EQ(WithRichPresenceFlag(0, kLuaGame, true), kStatusFlagRichPresence);
    // Stopped (appid 0): bit set so the UI rebuilds (clears) its map.
    EXPECT_EQ(WithRichPresenceFlag(0, 0, false), kStatusFlagRichPresence);
}

TEST(RichPresenceWithRichPresenceFlag, FreshLaunchMasksTheBit) {
    // Game with no KVs yet: the bit is CLEARED so the first inject does not wipe
    // the UI's m_mapRichPresence.
    EXPECT_EQ(WithRichPresenceFlag(kStatusFlagRichPresence, kLuaGame, false), 0u);
    EXPECT_EQ(WithRichPresenceFlag(0, kLuaGame, false), 0u);
}

TEST(RichPresenceWithRichPresenceFlag, PreservesEveryOtherBit) {
    constexpr std::uint32_t kAll = 0xFFFFFFFFu;
    EXPECT_EQ(WithRichPresenceFlag(kAll, kLuaGame, false), kAll & ~kStatusFlagRichPresence);
    EXPECT_EQ(WithRichPresenceFlag(kAll, kLuaGame, true), kAll);
    EXPECT_EQ(WithRichPresenceFlag(0x00000FFFu, 0, false), 0x00000FFFu | kStatusFlagRichPresence);
    // Idempotent.
    const std::uint32_t once = WithRichPresenceFlag(0x1234u, kLuaGame, true);
    EXPECT_EQ(WithRichPresenceFlag(once, kLuaGame, true), once);
}

TEST(RichPresenceWithRichPresenceFlag, FlagValueIsPinned) {
    // k_EClientPersonaStateFlagRichPresence -- a wire constant, never change it.
    static_assert(kStatusFlagRichPresence == 0x1000);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// ExtractStringKVs -- Steam's binary KV1 stream
// ---------------------------------------------------------------------------
TEST(RichPresenceExtractStringKVs, HappyPath) {
    const std::string kv = Open("RP") + Str("status", "In match") + Str("steam_display", "#Playing") + Close();
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], (std::pair<std::string, std::string>{ "status", "In match" }));
    EXPECT_EQ(got[1], (std::pair<std::string, std::string>{ "steam_display", "#Playing" }));
}

TEST(RichPresenceExtractStringKVs, CollectsAtAnyDepth) {
    const std::string kv = Open("RP") + Str("a", "1") + Open("sub") + Str("b", "2")
                         + Open("deeper") + Str("c", "3") + Close() + Close() + Str("d", "4") + Close();
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0].first, "a");
    EXPECT_EQ(got[1].first, "b");
    EXPECT_EQ(got[2].first, "c");
    EXPECT_EQ(got[3].first, "d");
}

TEST(RichPresenceExtractStringKVs, StopsAtTheStreamTerminator) {
    // A real stream ends `08 08`: the first closes "RP", the second (no struct
    // left open) ends the KeyValues. Anything after that is ignored.
    const std::string kv = Open("RP") + Str("a", "1") + Close() + Close() + Str("ignored", "x");
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].first, "a");
}

TEST(RichPresenceExtractStringKVs, ClosingTheTopStructAloneDoesNotTerminate) {
    // Pins the verbatim behaviour: a single 0x08 only closes the "RP" struct
    // (depth 1 -> 0) and the walk continues, so a KV after it is still read.
    const std::string kv = Open("RP") + Str("a", "1") + Close() + Str("sibling", "x");
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].first, "a");
    EXPECT_EQ(got[1].first, "sibling");
}

TEST(RichPresenceExtractStringKVs, StrayEndAtDepthZeroStops) {
    // A leading 0x08 with no open struct: nothing to collect, nothing after it read.
    EXPECT_TRUE(Parse(Close() + Str("a", "1")).empty());
}

TEST(RichPresenceExtractStringKVs, EmptyAndNullInput) {
    EXPECT_TRUE(Parse("").empty());
    KvList out;
    ExtractStringKVs(nullptr, 0, out);    // size 0: must not dereference
    EXPECT_TRUE(out.empty());
}

TEST(RichPresenceExtractStringKVs, UnknownTypeKeepsWhatWasCollected) {
    // 0x02 (int32) is not handled: stop, but keep the string KVs read so far.
    const std::string kv = Open("RP") + Str("a", "1") + std::string(1, '\x02') + std::string("n\0\x01\x00\x00\x00", 6)
                         + Str("never", "reached") + Close();
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].first, "a");
}

TEST(RichPresenceExtractStringKVs, TruncatedInputNeverOverreads) {
    const std::string full = Open("RP") + Str("key", "value") + Str("k2", "v2") + Close();
    // Cut at every possible length: each prefix must parse without reading past
    // `size` and only ever yield fully-terminated pairs. Each prefix is copied
    // into an exact-size heap buffer so a read one byte too far is caught by
    // AddressSanitizer (the Linux CI builds this suite with it).
    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        const std::vector<std::uint8_t> exact(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        KvList out;
        ExtractStringKVs(exact.data(), static_cast<std::uint32_t>(cut), out);
        ASSERT_LE(out.size(), 2u) << "cut=" << cut;
        for (const auto& [k, v] : out) {
            EXPECT_TRUE((k == "key" && v == "value") || (k == "k2" && v == "v2")) << "cut=" << cut;
        }
    }
}

TEST(RichPresenceExtractStringKVs, UnterminatedStringsAreDropped) {
    // Key with no NUL, then a value with no NUL.
    EXPECT_TRUE(Parse(Open("RP") + std::string(1, '\x01') + "keyonly").empty());
    EXPECT_TRUE(Parse(Open("RP") + std::string(1, '\x01') + std::string("key\0valueNoNul", 14)).empty());
    // A struct whose name never terminates.
    EXPECT_TRUE(Parse(std::string(1, '\x00') + "RPnoNul").empty());
}

TEST(RichPresenceExtractStringKVs, FalsePositiveGuards) {
    // Empty key and empty value are legal.
    const KvList empties = Parse(Open("RP") + Str("", "") + Str("k", "") + Str("", "v") + Close());
    ASSERT_EQ(empties.size(), 3u);
    EXPECT_EQ(empties[0], (std::pair<std::string, std::string>{ "", "" }));
    EXPECT_EQ(empties[1], (std::pair<std::string, std::string>{ "k", "" }));
    EXPECT_EQ(empties[2], (std::pair<std::string, std::string>{ "", "v" }));
    // UTF-8 / high bytes in values pass through untouched.
    const std::string utf8 = "Em partida \xE2\x80\x94 Jo\xC3\xA3o \xE6\x97\xA5\xE6\x9C\xAC";
    const KvList u = Parse(Open("RP") + Str("status", utf8) + Close());
    ASSERT_EQ(u.size(), 1u);
    EXPECT_EQ(u[0].second, utf8);
}

TEST(RichPresenceExtractStringKVs, AppendsAndDoesNotClear) {
    // The caller clears (TrackRPSend does kvs.clear() first); the parser appends.
    KvList out = { { "pre", "existing" } };
    const std::string kv = Open("RP") + Str("a", "1") + Close();
    ExtractStringKVs(reinterpret_cast<const std::uint8_t*>(kv.data()),
                     static_cast<std::uint32_t>(kv.size()), out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "pre");
    EXPECT_EQ(out[1].first, "a");
}

TEST(RichPresenceExtractStringKVs, DeepNestingDoesNotRecurse) {
    // 20000 nested structs, each closed, with one KV in the middle. Iterative, so
    // a hostile depth cannot exhaust the stack.
    constexpr int kDepth = 20000;
    std::string kv;
    for (int i = 0; i < kDepth; ++i) kv += Open("n");
    kv += Str("deep", "value");
    for (int i = 0; i < kDepth; ++i) kv += Close();
    const KvList got = Parse(kv);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].first, "deep");
}

// ---------------------------------------------------------------------------
// ExtractionParity -- the pre-extraction inline expressions, verbatim
// ---------------------------------------------------------------------------
namespace {
    // TrackSend (before):
    //   newTracked = (topmost != 0 && topmost != kOnlineFixAppId && HasDepot(topmost)) ? topmost : 0;
    std::uint32_t OldNewTracked(std::uint32_t topmost, bool lua) {
        return (topmost != 0 && topmost != kOnlineFixId && lua) ? topmost : 0;
    }

    enum class OldBranch { Unchanged, Track, Clear, Defer };
    // TrackSend (before), under g_RPMutex:
    //   if (g_PlayingAppId == newTracked) { /* nothing */ }
    //   else { g_PlayingAppId = newTracked;
    //          if (newTracked != 0) Track; else if (topmost == 0) Clear; else Defer; }
    OldBranch OldTransition(std::uint32_t playing, std::uint32_t newTracked, std::uint32_t topmost) {
        if (playing == newTracked) return OldBranch::Unchanged;
        if (newTracked != 0)       return OldBranch::Track;
        if (topmost == 0)          return OldBranch::Clear;
        return OldBranch::Defer;
    }
    TrackAction ToAction(OldBranch b) {
        switch (b) {
        case OldBranch::Unchanged: return TrackAction::Unchanged;
        case OldBranch::Track:     return TrackAction::InjectGame;
        case OldBranch::Clear:     return TrackAction::InjectClear;
        case OldBranch::Defer:     return TrackAction::DeferToServer;
        }
        return TrackAction::Unchanged;
    }
    // ApplyGameFields (before): appid ? (hasKvs ? set : clear) : set
    std::uint32_t OldFlags(std::uint32_t flags, std::uint32_t appid, bool hasKvs) {
        constexpr std::uint32_t kFlag = 0x1000;
        if (appid) return hasKvs ? (flags | kFlag) : (flags & ~kFlag);
        return flags | kFlag;
    }
}

TEST(RichPresenceExtractionParity, TrackingDecisionOverTheWholeDomain) {
    const std::uint32_t apps[] = { 0, 1, kOwnedGame, kOnlineFixId, kLuaGame, kOtherLua, 0xFFFFFFFFu };
    for (std::uint32_t topmost : apps)
        for (bool lua : { false, true }) {
            const std::uint32_t oldNew = OldNewTracked(topmost, lua);
            const std::uint32_t newNew = ShouldTrackAppId(topmost, lua, topmost == kOnlineFixId) ? topmost : 0;
            ASSERT_EQ(newNew, oldNew) << "topmost=" << topmost << " lua=" << lua;

            for (std::uint32_t playing : apps) {
                EXPECT_EQ(DecideTrackAction(playing, newNew, topmost),
                          ToAction(OldTransition(playing, oldNew, topmost)))
                    << "playing=" << playing << " topmost=" << topmost << " lua=" << lua;
            }
        }
}

TEST(RichPresenceExtractionParity, StatusFlagOverTheWholeDomain) {
    for (std::uint32_t flags : { 0u, 0x1000u, 0x0FFFu, 0xFFFFFFFFu, 0x12345678u })
        for (std::uint32_t appid : { 0u, 1u, kLuaGame })
            for (bool hasKvs : { false, true }) {
                // appid == 0 never consults the KV map in the hook; the call passes false.
                const bool kvs = appid != 0 && hasKvs;
                EXPECT_EQ(WithRichPresenceFlag(flags, appid, kvs), OldFlags(flags, appid, kvs))
                    << "flags=" << flags << " appid=" << appid << " hasKvs=" << kvs;
            }
}

TEST(RichPresenceExtractionParity, GuardsCollapseToTheOldConditions) {
    // Explicit array: `{ uint64_t{0}, ~0ull }` fails to deduce where uint64_t is
    // `unsigned long` (Linux) rather than `unsigned long long` (MSVC).
    const std::uint64_t sids[] = { 0, 1, ~0ull };
    for (std::uint64_t sid : sids)
        for (std::uint32_t playing : { 0u, 1u, kLuaGame }) {
            // TrackRPSend (before): if (g_LocalSteamId == 0 || g_PlayingAppId == 0) return;
            EXPECT_EQ(ShouldCaptureRpUpload(sid, playing), !(sid == 0 || playing == 0));
            // HandleRecv (before): if (g_PlayingAppId == 0) return false;
            EXPECT_EQ(ShouldInjectRichPresence(playing), !(playing == 0));
        }
}
