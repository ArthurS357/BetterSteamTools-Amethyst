// Unit tests for DonateConfigLogic::Apply -- the pure [donate] TOML parsing
// behind Config::Load (see Utils/Config/DonateConfigLogic.h/.cpp), extracted
// specifically so it is testable without Lua/LuaConfig, WinHTTP, or spdlog.
//
// [donate] is the Amethyst fork's ONE opt-IN-by-default-false feature that
// sends data to a third party (see Config::DonateSettings and
// ManifestDonor.h), so the default-value and absent-section cases here are
// deliberately explicit rather than incidental: Config::DonateSettings{}
// must come up disabled with no TOML at all, exactly like upstream
// BetterSteamTools' opt-out default was flipped to opt-in for this fork.

#include "Utils/Config/DonateConfigLogic.h"

#include <gtest/gtest.h>

using Config::DonateSettings;
using DonateConfigLogic::Apply;
using DonateConfigLogic::RangeViolation;

namespace {

    // Parses `tomlText` and returns its root table's ["donate"] subtable,
    // asserting the subtable is present -- every test here supplies one.
    // Parameter deliberately not named "toml" -- that would shadow the
    // toml:: namespace used inside this function's own body.
    //
    // This project builds with exceptions enabled (/EHsc), so toml++'s
    // TOML_EXCEPTIONS=1 mode is in effect: toml::parse() returns a
    // toml::table directly (throwing toml::parse_error on malformed input)
    // rather than the no-exceptions toml::parse_result wrapper -- same as
    // Config.cpp's own toml::parse_file() call.
    toml::table DonateSection(std::string_view tomlText) {
        toml::table root = toml::parse(tomlText);
        auto* donate = root["donate"].as_table();
        EXPECT_NE(donate, nullptr);
        return donate ? *donate : toml::table{};
    }

} // namespace

// ── The inegotiable default: no [donate] section at all ─────────────────
// Config.cpp only calls Apply() when tbl["donate"].as_table() succeeds, so
// the "absent entirely" case is the plain struct default -- verified here so
// a future refactor of DonateSettings can't silently flip it back to true.
TEST(DonateConfigLogicTest, DefaultConstructedSettingsAreDisabled) {
    DonateSettings settings;
    EXPECT_FALSE(settings.enabled);
    EXPECT_TRUE(settings.url.empty());
}

// ── enabled/url ───────────────────────────────────────────────────────────
TEST(DonateConfigLogicTest, EnabledTrueIsApplied) {
    DonateSettings settings;
    auto violations = Apply(DonateSection("[donate]\nenabled = true\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_TRUE(settings.enabled);
}

TEST(DonateConfigLogicTest, EnabledFalseExplicitStaysDisabled) {
    DonateSettings settings;
    settings.enabled = true; // simulate a prior load that turned it on
    auto violations = Apply(DonateSection("[donate]\nenabled = false\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_FALSE(settings.enabled);
}

TEST(DonateConfigLogicTest, UrlOverrideIsApplied) {
    DonateSettings settings;
    auto violations = Apply(
        DonateSection("[donate]\nurl = \"https://mirror.example/donate\"\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_EQ(settings.url, "https://mirror.example/donate");
}

// ── Partial keys: everything absent from the TOML keeps its prior/default
//    value untouched (Apply never resets a field it didn't see a key for) ──
TEST(DonateConfigLogicTest, PartialKeysPreserveDefaultsForTheRest) {
    DonateSettings settings; // all fields at their Config.h defaults
    auto violations = Apply(DonateSection("[donate]\nenabled = true\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_TRUE(settings.enabled);
    EXPECT_TRUE(settings.url.empty());
    EXPECT_EQ(settings.intervalSecs, 30u);
    EXPECT_EQ(settings.maxMintsPerCycle, 25u);
    EXPECT_EQ(settings.minMintIntervalMs, 2000u);
    EXPECT_EQ(settings.maxMintsPerSession, 0u);
    EXPECT_EQ(settings.wantedRefreshSecs, 300u);
}

// ── Numeric clamping: every field, both directions ───────────────────────
TEST(DonateConfigLogicTest, AllNumericFieldsAcceptedWithinRange) {
    DonateSettings settings;
    auto violations = Apply(DonateSection(
        "[donate]\n"
        "interval_secs = 60\n"
        "max_mints_per_cycle = 10\n"
        "min_mint_interval_ms = 500\n"
        "max_mints_per_session = 1000\n"
        "wanted_refresh_secs = 600\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_EQ(settings.intervalSecs, 60u);
    EXPECT_EQ(settings.maxMintsPerCycle, 10u);
    EXPECT_EQ(settings.minMintIntervalMs, 500u);
    EXPECT_EQ(settings.maxMintsPerSession, 1000u);
    EXPECT_EQ(settings.wantedRefreshSecs, 600u);
}

TEST(DonateConfigLogicTest, BoundaryValuesAreAcceptedNotRejected) {
    DonateSettings settings;
    auto violations = Apply(DonateSection(
        "[donate]\n"
        "interval_secs = 30\n"          // lo
        "max_mints_per_cycle = 500\n"   // hi
        "min_mint_interval_ms = 0\n"    // lo
        "max_mints_per_session = 100000\n" // hi
        "wanted_refresh_secs = 86400\n"  // hi
        ), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_EQ(settings.intervalSecs, 30u);
    EXPECT_EQ(settings.maxMintsPerCycle, 500u);
    EXPECT_EQ(settings.minMintIntervalMs, 0u);
    EXPECT_EQ(settings.maxMintsPerSession, 100000u);
    EXPECT_EQ(settings.wantedRefreshSecs, 86400u);
}

TEST(DonateConfigLogicTest, OutOfRangeValueIsRejectedAndDefaultKept) {
    DonateSettings settings; // intervalSecs defaults to 30
    auto violations = Apply(DonateSection("[donate]\ninterval_secs = 5\n"), settings);
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations[0].key, "interval_secs");
    EXPECT_EQ(violations[0].value, 5);
    EXPECT_EQ(violations[0].lo, 30u);
    EXPECT_EQ(violations[0].hi, 86400u);
    EXPECT_EQ(violations[0].kept, 30u);
    // The out-of-range write must not have landed.
    EXPECT_EQ(settings.intervalSecs, 30u);
}

TEST(DonateConfigLogicTest, NegativeValueIsRejectedNotWrappedToUnsigned) {
    DonateSettings settings;
    auto violations = Apply(DonateSection("[donate]\nmax_mints_per_cycle = -1\n"), settings);
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations[0].value, -1);
    // Must not have been reinterpreted as a huge uint32_t.
    EXPECT_EQ(settings.maxMintsPerCycle, 25u);
}

TEST(DonateConfigLogicTest, MultipleOutOfRangeKeysEachReported) {
    DonateSettings settings;
    auto violations = Apply(DonateSection(
        "[donate]\n"
        "interval_secs = 1\n"
        "max_mints_per_cycle = 9999\n"), settings);
    EXPECT_EQ(violations.size(), 2u);
    EXPECT_EQ(settings.intervalSecs, 30u);
    EXPECT_EQ(settings.maxMintsPerCycle, 25u);
}

TEST(DonateConfigLogicTest, OneValidAndOneInvalidKeyBothTakeEffectIndependently) {
    DonateSettings settings;
    auto violations = Apply(DonateSection(
        "[donate]\n"
        "interval_secs = 45\n"        // valid
        "wanted_refresh_secs = 5\n"), // invalid (< 30)
        settings);
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations[0].key, "wanted_refresh_secs");
    EXPECT_EQ(settings.intervalSecs, 45u);        // applied
    EXPECT_EQ(settings.wantedRefreshSecs, 300u);  // rejected, default kept
}

// ── Malformed / wrong-type input: rejected, not misread ──────────────────
TEST(DonateConfigLogicTest, WrongTypeForEnabledIsIgnoredNotCoerced) {
    DonateSettings settings;
    // "enabled" as a string rather than a bool: value<bool>() returns
    // nullopt, so the field must be left at its (safe, disabled) default.
    auto violations = Apply(DonateSection("[donate]\nenabled = \"yes\"\n"), settings);
    EXPECT_TRUE(violations.empty()); // not a range violation -- silently not-a-bool
    EXPECT_FALSE(settings.enabled);
}

TEST(DonateConfigLogicTest, WrongTypeForNumericKeyIsIgnoredNotCoerced) {
    DonateSettings settings;
    auto violations = Apply(DonateSection("[donate]\ninterval_secs = \"soon\"\n"), settings);
    EXPECT_TRUE(violations.empty()); // value<int64_t>() sees a string -> nullopt, no violation logged
    EXPECT_EQ(settings.intervalSecs, 30u);
}

TEST(DonateConfigLogicTest, UnknownKeyIsIgnoredSilently) {
    DonateSettings settings;
    auto violations = Apply(
        DonateSection("[donate]\nenabled = true\nnot_a_real_key = 123\n"), settings);
    EXPECT_TRUE(violations.empty());
    EXPECT_TRUE(settings.enabled);
}

TEST(DonateConfigLogicTest, EmptyDonateTableChangesNothing) {
    DonateSettings settings; // all defaults
    auto violations = Apply(DonateSection("[donate]\n"), settings);
    EXPECT_TRUE(violations.empty());
    // DonateSettings has no operator== (a plain aggregate) -- compare fields.
    EXPECT_FALSE(settings.enabled);
    EXPECT_TRUE(settings.url.empty());
    EXPECT_EQ(settings.intervalSecs, 30u);
    EXPECT_EQ(settings.maxMintsPerCycle, 25u);
    EXPECT_EQ(settings.minMintIntervalMs, 2000u);
    EXPECT_EQ(settings.maxMintsPerSession, 0u);
    EXPECT_EQ(settings.wantedRefreshSecs, 300u);
}
