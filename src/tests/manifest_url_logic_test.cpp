// Unit tests for ManifestUrlLogic::Build -- the pure URL-selection/
// construction logic behind ManifestClient::FetchActive (see
// Utils/SteamMetadata/ManifestUrlLogic.h/.cpp), extracted specifically so it
// is testable without WinHTTP, Config, or LuaConfig. No network call is
// possible from this test binary.
//
// Context (see the session's Fase C audit): Valve made the manifest request
// code depot-bound on 2026-09-09, so a gid-only URL only works for
// free-to-play carrier depots -- every other depot 401s at the CDN. The
// depot-aware shape (app/depot/gid) is the fix, and it must never be
// bypassed silently by an unrelated feature: a user's [manifest] url_template
// override has to keep working exactly as before (short-circuiting provider
// selection entirely), while gaining the OPTION to go depot-aware too.

#include "Utils/SteamMetadata/ManifestUrlLogic.h"

#include <gtest/gtest.h>

using ManifestUrlLogic::Build;
using ManifestUrlLogic::Choice;
using ManifestUrlLogic::Shape;

namespace {
    constexpr const char* kGidOnlyTemplate  = "https://manifest.example/%llu";
    constexpr const char* kDepotAwareTemplate = "https://manifest.example/%u/%u/%llu";
}

// ── Override short-circuits everything ───────────────────────────────────
TEST(ManifestUrlLogicTest, OverrideWinsOverDepotAwareProvider) {
    // Even though the provider has a depot-aware template AND depotId != 0
    // (both conditions that would normally select DepotAware), a non-empty
    // override must still win unconditionally.
    Choice c = Build("https://mine.example/{gid}", kGidOnlyTemplate, kDepotAwareTemplate,
                     /*appId=*/730, /*depotId=*/731, /*gid=*/123456789);
    EXPECT_EQ(c.shape, Shape::Override);
    EXPECT_EQ(c.url, "https://mine.example/123456789");
    EXPECT_FALSE(c.truncated);
}

TEST(ManifestUrlLogicTest, OverrideWithGidOnlyPlaceholderStaysGidOnlyShapeOfUrl) {
    // A template written before {appid}/{depotid} existed, using only {gid},
    // must not be forced to migrate -- it just doesn't get the extra fields.
    Choice c = Build("https://mine.example/manifest/{gid}", kGidOnlyTemplate, kDepotAwareTemplate,
                     42, 100, 999);
    EXPECT_EQ(c.shape, Shape::Override);
    EXPECT_EQ(c.url, "https://mine.example/manifest/999");
}

TEST(ManifestUrlLogicTest, OverrideWithAllThreePlaceholdersSubstitutesAll) {
    Choice c = Build("https://mine.example/{appid}/{depotid}/{gid}",
                     kGidOnlyTemplate, kDepotAwareTemplate,
                     730, 731, 123456789);
    EXPECT_EQ(c.shape, Shape::Override);
    EXPECT_EQ(c.url, "https://mine.example/730/731/123456789");
}

TEST(ManifestUrlLogicTest, OverrideWithRepeatedPlaceholderSubstitutesEveryOccurrence) {
    Choice c = Build("https://mine.example/{gid}?backup={gid}", kGidOnlyTemplate, nullptr,
                     0, 0, 55);
    EXPECT_EQ(c.url, "https://mine.example/55?backup=55");
}

TEST(ManifestUrlLogicTest, OverrideNeverReportsTruncationRegardlessOfLength) {
    // Deliberately build a URL far longer than kMaxProviderUrlLen (256) to
    // prove the override path is not routed through the fixed-size buffer
    // the provider path uses.
    std::string longOverride = "https://mine.example/";
    longOverride.append(400, 'x');
    longOverride += "/{gid}";
    Choice c = Build(longOverride, kGidOnlyTemplate, nullptr, 0, 0, 1);
    EXPECT_FALSE(c.truncated);
    EXPECT_GT(c.url.size(), ManifestUrlLogic::kMaxProviderUrlLen);
}

// ── No override: provider selection ──────────────────────────────────────
TEST(ManifestUrlLogicTest, NoOverrideDepotAwareProviderWithKnownDepotUsesDepotAware) {
    Choice c = Build("", kGidOnlyTemplate, kDepotAwareTemplate, 730, 731, 999);
    EXPECT_EQ(c.shape, Shape::DepotAware);
    EXPECT_EQ(c.url, "https://manifest.example/730/731/999");
}

TEST(ManifestUrlLogicTest, NoOverrideDepotAwareProviderWithZeroDepotFallsBackToGidOnly) {
    // depotId == 0 means "unknown depot" -- a depot-aware URL naming depot 0
    // would be meaningless, so this must NOT select DepotAware even though
    // the provider supports it.
    Choice c = Build("", kGidOnlyTemplate, kDepotAwareTemplate, 730, /*depotId=*/0, 999);
    EXPECT_EQ(c.shape, Shape::GidOnly);
    EXPECT_EQ(c.url, "https://manifest.example/999");
}

TEST(ManifestUrlLogicTest, NoOverrideProviderWithoutExTemplateAlwaysGidOnly) {
    // wudrm/steamrun: providerTemplateEx == nullptr. Even with a real depotId,
    // there is no depot-aware route to use.
    Choice c = Build("", kGidOnlyTemplate, /*providerTemplateEx=*/nullptr, 730, 731, 999);
    EXPECT_EQ(c.shape, Shape::GidOnly);
    EXPECT_EQ(c.url, "https://manifest.example/999");
}

TEST(ManifestUrlLogicTest, GidOnlyUrlDoesNotMentionAppOrDepot) {
    // Sanity check on the provider's own gid-only template: it must not leak
    // appid/depotid formatting artifacts (e.g. from a copy-paste template bug).
    Choice c = Build("", "https://manifest.example/gid/%llu", nullptr, 730, 731, 999);
    EXPECT_EQ(c.url, "https://manifest.example/gid/999");
}

// ── Truncation (provider path only) ──────────────────────────────────────
TEST(ManifestUrlLogicTest, OversizedProviderTemplateReportsTruncated) {
    // A pathological (never-shipped) template whose literal prefix alone
    // exceeds kMaxProviderUrlLen -- proves the defensive check fires; not a
    // realistic provider row, all 3 built-ins are short fixed literals.
    std::string hugePrefix = "https://";
    hugePrefix.append(ManifestUrlLogic::kMaxProviderUrlLen + 64, 'x');
    hugePrefix += "/%llu";

    Choice c = Build("", hugePrefix.c_str(), nullptr, 0, 0, 1);
    EXPECT_TRUE(c.truncated);
    EXPECT_EQ(c.shape, Shape::GidOnly);
    EXPECT_EQ(c.url.size(), ManifestUrlLogic::kMaxProviderUrlLen - 1);
}

// ── Empty override is "unset", not a degenerate override ─────────────────
TEST(ManifestUrlLogicTest, EmptyOverrideTemplateIsTreatedAsUnset) {
    Choice c = Build(/*overrideTemplate=*/"", kGidOnlyTemplate, kDepotAwareTemplate, 730, 731, 999);
    EXPECT_NE(c.shape, Shape::Override);
    EXPECT_EQ(c.shape, Shape::DepotAware);
}

// ── ShouldFallbackToGidOnly ────────────────────────────────────────────────
// Regression coverage for the manifest.opensteamtool.com depot-aware
// regression (Fase C.2, commit 2c24e56): FetchActive built exactly one URL
// and never retried, so a provider that doesn't (yet) accept the depot-aware
// shape broke every new-game download outright. Only DepotAware has anything
// worth falling back to.
using ManifestUrlLogic::ShouldFallbackToGidOnly;

TEST(ManifestUrlLogicTest, FallbackDepotAwareOnFailure) {
    EXPECT_TRUE(ShouldFallbackToGidOnly(Shape::DepotAware, /*requestFailed=*/true));
}

TEST(ManifestUrlLogicTest, NoFallbackDepotAwareOnSuccess) {
    EXPECT_FALSE(ShouldFallbackToGidOnly(Shape::DepotAware, /*requestFailed=*/false));
}

TEST(ManifestUrlLogicTest, NoFallbackGidOnly) {
    // Already the simplest shape -- a failure here has nothing left to fall
    // back to, failed or not.
    EXPECT_FALSE(ShouldFallbackToGidOnly(Shape::GidOnly, /*requestFailed=*/true));
    EXPECT_FALSE(ShouldFallbackToGidOnly(Shape::GidOnly, /*requestFailed=*/false));
}

TEST(ManifestUrlLogicTest, NoFallbackOverride) {
    // The user's own url_template is never silently rewritten to something
    // else, regardless of whether their request failed.
    EXPECT_FALSE(ShouldFallbackToGidOnly(Shape::Override, /*requestFailed=*/true));
    EXPECT_FALSE(ShouldFallbackToGidOnly(Shape::Override, /*requestFailed=*/false));
}
