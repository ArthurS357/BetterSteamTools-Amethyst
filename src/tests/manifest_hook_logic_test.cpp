// Unit tests for ManifestHookLogic -- the pure predicates behind the hooks in
// Hooks_Manifest.cpp (see Hook/ManifestHookLogic.h), extracted so they are
// testable without the hook engine, LuaConfig or a live Steam.

#include "Hook/ManifestHookLogic.h"

#include <gtest/gtest.h>

#include <cstdint>

using ManifestHookLogic::ShouldPreseedOnManifestLoad;
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

    // ── YldLoadDepotManifest pre-seed gate ───────────────────────────

    constexpr uint32_t kDepot = 3768761;
    constexpr uint64_t kGid   = 0x1234'5678'9ABC'DEF0ull;

    // Phase B regression: without [cache] url nothing may reach the network,
    // whatever the lua state -- pinned, declared, unowned, all of it.
    TEST(ManifestHookLogicTest, PreseedNeverFiresWithoutCacheUrl) {
        for (const bool pinned : { false, true })
            for (const bool lua : { false, true })
                for (const bool owned : { false, true })
                    EXPECT_FALSE(ShouldPreseedOnManifestLoad(/*cacheConfigured=*/false, kDepot, kGid, pinned, lua, owned))
                        << "pinned=" << pinned << " lua=" << lua << " owned=" << owned;
    }

    TEST(ManifestHookLogicTest, PreseedsLuaDeclaredUnownedDepot) {
        EXPECT_TRUE(ShouldPreseedOnManifestLoad(true, kDepot, kGid, /*pinned=*/false, /*luaDeclaresDepot=*/true, /*owned=*/false));
    }

    // A pin states the gid Steam will request; it is pre-seeded even when owned
    // (same policy as the PreseedDepots sweep it replaces).
    TEST(ManifestHookLogicTest, PreseedsPinnedDepotEvenWhenOwned) {
        EXPECT_TRUE(ShouldPreseedOnManifestLoad(true, kDepot, kGid, /*pinned=*/true, /*luaDeclaresDepot=*/true,  /*owned=*/true));
        EXPECT_TRUE(ShouldPreseedOnManifestLoad(true, kDepot, kGid, /*pinned=*/true, /*luaDeclaresDepot=*/false, /*owned=*/false));
    }

    // Owned, unpinned content gets working request codes from Steam itself.
    TEST(ManifestHookLogicTest, LeavesOwnedOrNonLuaDepotsToSteam) {
        EXPECT_FALSE(ShouldPreseedOnManifestLoad(true, kDepot, kGid, false, /*luaDeclaresDepot=*/true,  /*owned=*/true));
        EXPECT_FALSE(ShouldPreseedOnManifestLoad(true, kDepot, kGid, false, /*luaDeclaresDepot=*/false, /*owned=*/false));
    }

    TEST(ManifestHookLogicTest, PreseedRejectsZeroDepotOrGid) {
        EXPECT_FALSE(ShouldPreseedOnManifestLoad(true, 0, kGid, true, true, false));
        EXPECT_FALSE(ShouldPreseedOnManifestLoad(true, kDepot, 0, true, true, false));
    }

} // namespace
