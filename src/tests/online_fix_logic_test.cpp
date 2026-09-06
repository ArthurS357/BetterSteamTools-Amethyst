// Unit tests for OnlineFixLogic -- the pure decision logic behind the
// online-fix AppID flip (see Hook/Hooks_Misc.cpp). Extracted specifically so
// it is testable without HookMacros.h's Detour/PatternLoader dependency.
//
// Three categories throughout: happy path, malicious/malformed input, and
// false-positive guards (legitimate-but-unusual input that must NOT be rejected).

#include "Hook/OnlineFixLogic.h"

#include <gtest/gtest.h>

using namespace OnlineFixLogic;

// ---------------------------------------------------------------------------
// ShouldSuppressAppIdFlip -- "-realappid" launch-option detection
// ---------------------------------------------------------------------------
TEST(ShouldSuppressAppIdFlip, HappyPath) {
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-realappid"));
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-onlinefix -realappid"));
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-realappid -onlinefix"));
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-onlinefix -realappid -windowed"));
}

TEST(ShouldSuppressAppIdFlip, MaliciousAndMalformed) {
    EXPECT_FALSE(ShouldSuppressAppIdFlip(""));                    // empty command line
    EXPECT_FALSE(ShouldSuppressAppIdFlip("-onlinefix"));           // flag absent
    EXPECT_FALSE(ShouldSuppressAppIdFlip("-realapp"));             // truncated flag, not a match
    // Case-sensitive by design (matches upstream's plain strstr semantics) --
    // a differently-cased flag must NOT be treated as present.
    EXPECT_FALSE(ShouldSuppressAppIdFlip("-REALAPPID"));
    EXPECT_FALSE(ShouldSuppressAppIdFlip("-RealAppId"));
}

TEST(ShouldSuppressAppIdFlip, FalsePositiveGuards) {
    // Deliberately a plain substring match (documented in the header), same as
    // the pre-existing "-onlinefix" detection it sits next to in
    // OnSpawnProcessHit -- "-realappid" as part of a longer token still counts.
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-realappidx"));
    EXPECT_TRUE(ShouldSuppressAppIdFlip("foo-realappid-bar"));
    // Multiple, unrelated launch options around it must not confuse the scan.
    EXPECT_TRUE(ShouldSuppressAppIdFlip("-onlinefix -windowed -realappid -novid -high"));
}

// ---------------------------------------------------------------------------
// ShouldReportOnlineFixAppId -- the AppID-flip decision (3 independent bools)
// ---------------------------------------------------------------------------
TEST(ShouldReportOnlineFixAppId, RequiresRealAppIdAndNetworkingWhenNotSuppressed) {
    // suppressFlip=false: the flip fires only once BOTH other conditions hold.
    EXPECT_FALSE(ShouldReportOnlineFixAppId(false, false, false));
    EXPECT_FALSE(ShouldReportOnlineFixAppId(false, true,  false));
    EXPECT_FALSE(ShouldReportOnlineFixAppId(false, false, true));
    EXPECT_TRUE (ShouldReportOnlineFixAppId(false, true,  true));
}

TEST(ShouldReportOnlineFixAppId, SuppressFlipAlwaysWinsRegardlessOfOtherState) {
    // suppressFlip=true: -realappid opts out unconditionally, even when both
    // other conditions would otherwise trigger the flip.
    EXPECT_FALSE(ShouldReportOnlineFixAppId(true, false, false));
    EXPECT_FALSE(ShouldReportOnlineFixAppId(true, true,  false));
    EXPECT_FALSE(ShouldReportOnlineFixAppId(true, false, true));
    EXPECT_FALSE(ShouldReportOnlineFixAppId(true, true,  true));
}
