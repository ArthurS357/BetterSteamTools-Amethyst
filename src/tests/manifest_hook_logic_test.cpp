// Unit tests for ManifestHookLogic -- the pure predicates behind the hooks in
// Hooks_Manifest.cpp (see Hook/ManifestHookLogic.h), extracted so they are
// testable without the hook engine, LuaConfig or a live Steam.

#include "Hook/ManifestHookLogic.h"

#include <gtest/gtest.h>

using ManifestHookLogic::ShouldRejectEmptyDepotList;

namespace {

    constexpr uint32_t kLuaApp = 3768760;   // seen locally finishing with "0 mounted depots"

    // ── BuildDepotDependency empty-list guard ────────────────────────────

    TEST(ManifestHookLogicTest, RejectsEmptyOrMissingOwnListForLuaApp) {
        EXPECT_TRUE(ShouldRejectEmptyDepotList(kLuaApp, /*hasOwnList=*/true,  0, /*lua=*/true));
        EXPECT_TRUE(ShouldRejectEmptyDepotList(kLuaApp, /*hasOwnList=*/false, 0, /*lua=*/true));
    }

    // False-positive guard: a healthy lua app, and a non-lua app that genuinely
    // has no depots, must both go through untouched.
    TEST(ManifestHookLogicTest, LeavesNonEmptyListsAndNonLuaAppsAlone) {
        EXPECT_FALSE(ShouldRejectEmptyDepotList(kLuaApp, true, 1, /*lua=*/true));
        EXPECT_FALSE(ShouldRejectEmptyDepotList(kLuaApp, true, 0, /*lua=*/false));
        EXPECT_FALSE(ShouldRejectEmptyDepotList(kLuaApp, false, 0, /*lua=*/false));
    }

    TEST(ManifestHookLogicTest, NeverRejectsAppIdZero) {
        EXPECT_FALSE(ShouldRejectEmptyDepotList(0, true, 0, /*lua=*/true));
    }

} // namespace
