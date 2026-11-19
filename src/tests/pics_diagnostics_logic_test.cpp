// Unit tests for PicsDiagnosticsLogic (Hook/PicsDiagnosticsLogic.h): the pure part of the
// PICS reply diagnostics -- counting depots in an appinfo buffer and deciding whether a lua app
// came back without a usable depot list.

#include "Hook/PicsDiagnosticsLogic.h"

#include <gtest/gtest.h>

#include <string>

using PicsDiagnosticsLogic::AppSummary;
using PicsDiagnosticsLogic::CountAppInfoDepots;
using PicsDiagnosticsLogic::LuaAppCameBackEmpty;

namespace {

    // The shape Steam sends: depots has numeric sections plus non-depot keys.
    constexpr const char* kTwoDepots = R"("appinfo"
{
	"appid"		"391540"
	"depots"
	{
		"228990"
		{
			"config" { "oslist" "windows" }
			"manifests" { "public" { "gid" "123" "size" "456" } }
		}
		"391541"
		{
			"manifests" { "public" { "gid" "789" } }
		}
		"branches" { "public" { "buildid" "4279148" } }
		"baselanguages"		"english"
		"overridescddb"		"1"
	}
	"common" { "name" "Undertale" }
}
)";

    // ── CountAppInfoDepots ───────────────────────────────────────────────

    TEST(PicsDiagnosticsLogicTest, CountsNumericDepotSectionsOnly) {
        const auto n = CountAppInfoDepots(kTwoDepots);
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 2u);   // "branches" and the plain string keys are not depots
    }

    TEST(PicsDiagnosticsLogicTest, TextBufferWithoutDepotsSectionIsZero) {
        const auto n = CountAppInfoDepots(R"("appinfo" { "appid" "5" "common" { "name" "x" } })");
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 0u);
    }

    TEST(PicsDiagnosticsLogicTest, EmptyDepotsSectionIsZero) {
        const auto n = CountAppInfoDepots(R"("appinfo" { "depots" { "branches" { } } })");
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 0u);
    }

    // A depots-looking section anywhere else is not counted.
    TEST(PicsDiagnosticsLogicTest, OnlyDirectChildrenOfAppinfoDepotsCount) {
        const auto n = CountAppInfoDepots(
            R"("appinfo" { "extended" { "depots" { "111" { } } } "depots" { "222" { "depots" { "333" { } } } } })");
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 1u);   // only 222
    }

    TEST(PicsDiagnosticsLogicTest, UnknownWhenEmptyOrBinary) {
        EXPECT_FALSE(CountAppInfoDepots("").has_value());
        EXPECT_FALSE(CountAppInfoDepots(std::string(3, '\0')).has_value());
        EXPECT_FALSE(CountAppInfoDepots(std::string("\x00\x61ppinfo\x00\x08", 10)).has_value());
    }

    TEST(PicsDiagnosticsLogicTest, TrailingNulAndLeadingSpaceAreAccepted) {
        std::string buf = std::string("  \n") + kTwoDepots;
        buf.push_back('\0');
        const auto n = CountAppInfoDepots(buf);
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 2u);
    }

    TEST(PicsDiagnosticsLogicTest, EscapedQuoteInsideAValueDoesNotBreakTheScan) {
        const auto n = CountAppInfoDepots(
            R"("appinfo" { "common" { "name" "say \"hi\" { }" } "depots" { "42" { } } })");
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 1u);
    }

    // Hostile or truncated input: never crashes, never reads out of range.
    TEST(PicsDiagnosticsLogicTest, TruncatedAndUnbalancedInputIsSafe) {
        const std::string full = kTwoDepots;
        for (size_t cut = 0; cut <= full.size(); ++cut)
            (void)CountAppInfoDepots(std::string_view(full).substr(0, cut));

        EXPECT_NO_FATAL_FAILURE((void)CountAppInfoDepots("\"appinfo\" { \"depots\" { \"1\" {"));
        EXPECT_NO_FATAL_FAILURE((void)CountAppInfoDepots("\"unterminated"));
        EXPECT_NO_FATAL_FAILURE((void)CountAppInfoDepots("\"a\" } } } }"));
    }

    TEST(PicsDiagnosticsLogicTest, VeryDeepNestingIsBounded) {
        std::string deep = "\"appinfo\" ";
        for (int i = 0; i < 5000; ++i) deep += "{ \"k\" ";
        const auto n = CountAppInfoDepots(deep);
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, 0u);
    }

    // ── LuaAppCameBackEmpty ──────────────────────────────────────────────

    AppSummary Lua() {
        AppSummary s;
        s.appId  = 391540;
        s.luaApp = true;
        return s;
    }

    TEST(PicsDiagnosticsLogicTest, MissingTokenFlagsALuaApp) {
        AppSummary s = Lua();
        s.missingToken = true;
        s.bufferBytes  = 40;
        EXPECT_TRUE(LuaAppCameBackEmpty(s));
    }

    TEST(PicsDiagnosticsLogicTest, EmptyBufferFlagsALuaApp) {
        AppSummary s = Lua();
        s.bufferBytes = 0;
        EXPECT_TRUE(LuaAppCameBackEmpty(s));
    }

    TEST(PicsDiagnosticsLogicTest, ZeroDepotsInARealBufferFlagsALuaApp) {
        AppSummary s = Lua();
        s.bufferBytes = 900;
        s.depots      = 0;
        EXPECT_TRUE(LuaAppCameBackEmpty(s));
    }

    TEST(PicsDiagnosticsLogicTest, HealthyLuaAppIsNotFlagged) {
        AppSummary s = Lua();
        s.bufferBytes = 5000;
        s.depots      = 3;
        EXPECT_FALSE(LuaAppCameBackEmpty(s));
    }

    // A present buffer that could not be counted (binary) is never reported as empty.
    TEST(PicsDiagnosticsLogicTest, UnreadableButPresentBufferIsNotFlagged) {
        AppSummary s = Lua();
        s.bufferBytes = 5000;
        s.depots.reset();
        EXPECT_FALSE(LuaAppCameBackEmpty(s));
    }

    TEST(PicsDiagnosticsLogicTest, NonLuaAppsAreNeverFlagged) {
        AppSummary s;
        s.appId        = 5;
        s.luaApp       = false;
        s.missingToken = true;
        s.bufferBytes  = 0;
        s.depots       = 0;
        EXPECT_FALSE(LuaAppCameBackEmpty(s));
    }

}  // namespace
