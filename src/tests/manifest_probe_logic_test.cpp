// Unit tests for ManifestProbeLogic -- the pure jobid-range predicate behind
// Hooks_NetPacket_ManifestProbe (see Hook/ManifestProbeLogic.h and
// Hooks_NetPacket.cpp's ManifestProbe namespace). Header-only and constexpr,
// so this needs no CMake source registration beyond this test file itself.
//
// Context (Fase C session audit, Tarefa 3): ManifestProbe originates
// GetManifestRequestCode calls with a synthetic jobid clear of Steam's own
// range (which counts up from small values), so its own replies can be
// recognized and consumed before the normal Manifest handler ever sees them.
// This is the one boundary check that decides that split.

#include "Hook/ManifestProbeLogic.h"

#include <gtest/gtest.h>

using ManifestProbeLogic::IsSyntheticJobId;
using ManifestProbeLogic::kJobIdBase;

TEST(ManifestProbeLogicTest, JobIdBaseMatchesDocumentedConstant) {
    // Pinned exact value -- session 7's audit and this fork's README/commit
    // messages both cite 0x7E51'0000'0000'0000; a silent change here would
    // make those docs wrong without anything else failing.
    EXPECT_EQ(kJobIdBase, 0x7E51'0000'0000'0000ull);
}

TEST(ManifestProbeLogicTest, ExactBaseValueIsSynthetic) {
    EXPECT_TRUE(IsSyntheticJobId(kJobIdBase));
}

TEST(ManifestProbeLogicTest, OneBelowBaseIsNotSynthetic) {
    EXPECT_FALSE(IsSyntheticJobId(kJobIdBase - 1));
}

TEST(ManifestProbeLogicTest, OneAboveBaseIsSynthetic) {
    // g_NextJobId starts at kJobIdBase and is pre-incremented before first
    // use (++g_NextJobId in Request()), so the first jobid actually handed
    // out is kJobIdBase + 1 -- must still be recognized.
    EXPECT_TRUE(IsSyntheticJobId(kJobIdBase + 1));
}

TEST(ManifestProbeLogicTest, ZeroIsNotSynthetic) {
    // Steam's own jobids count up from small values (near 0); this is the
    // most common real case ManifestProbe::HandleRecv must reject cheaply.
    EXPECT_FALSE(IsSyntheticJobId(0));
}

TEST(ManifestProbeLogicTest, SmallSteamLikeJobIdIsNotSynthetic) {
    EXPECT_FALSE(IsSyntheticJobId(12345));
}

TEST(ManifestProbeLogicTest, MaxUint64IsSynthetic) {
    // Sanity bound at the other extreme -- no risk of overflow/wraparound
    // in a plain >= comparison, but worth pinning explicitly.
    EXPECT_TRUE(IsSyntheticJobId(0xFFFF'FFFF'FFFF'FFFFull));
}
