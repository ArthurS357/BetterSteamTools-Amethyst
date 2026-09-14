// Unit tests for ManifestDonorLogic -- the pure network-gating predicates and
// wanted-list line parser behind ManifestDonor (see
// Utils/SteamMetadata/ManifestDonorLogic.h/.cpp and ManifestDonor.h/.cpp),
// extracted so they are testable without OSTPlatform::Http/Thread, Config,
// or a live Steam session. No network call is possible from this test binary.
//
// Context (Fase C session audit + this fork's INEGOCIÁVEL decision):
// [donate] defaults to enabled=false (opt-IN), the opposite of upstream
// BetterSteamTools' enabled=true (opt-out) default. Every predicate here is
// one of the gates that must independently keep the donor silent while
// disabled -- ShouldStartDonor gates the whole worker thread, and
// ShouldTrackSentRequest / ShouldSubmitCapturedCode form a second and third
// independent gate on the passive-capture path (registering a sent request,
// then submitting a captured code for it), so a bug in any ONE gate cannot
// alone cause network activity while [donate] is off.

#include "Utils/SteamMetadata/ManifestDonorLogic.h"

#include <gtest/gtest.h>

using ManifestDonorLogic::ParseWantedLine;
using ManifestDonorLogic::ShouldStartDonor;
using ManifestDonorLogic::ShouldSubmitCapturedCode;
using ManifestDonorLogic::ShouldTrackSentRequest;
using ManifestDonorLogic::WantedEntry;

// ── ShouldStartDonor -- gates the entire worker thread ───────────────────
TEST(ManifestDonorLogicTest, ShouldStartDonorMirrorsEnabledFlag) {
    EXPECT_TRUE(ShouldStartDonor(true));
    EXPECT_FALSE(ShouldStartDonor(false));
}

// ── ShouldTrackSentRequest -- first gate on the passive-capture path ─────
TEST(ManifestDonorLogicTest, ShouldTrackSentRequestRequiresBothJobAndEnabled) {
    EXPECT_TRUE(ShouldTrackSentRequest(/*haveJob=*/true, /*donateEnabled=*/true));
    EXPECT_FALSE(ShouldTrackSentRequest(/*haveJob=*/true, /*donateEnabled=*/false));
    EXPECT_FALSE(ShouldTrackSentRequest(/*haveJob=*/false, /*donateEnabled=*/true));
    EXPECT_FALSE(ShouldTrackSentRequest(/*haveJob=*/false, /*donateEnabled=*/false));
}

// ── ShouldSubmitCapturedCode -- second, independent gate ─────────────────
TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRequiresEveryFieldNonZeroAndEnabled) {
    EXPECT_TRUE(ShouldSubmitCapturedCode(/*depotId=*/730, /*gid=*/123, /*code=*/456, /*enabled=*/true));
}

TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRejectsZeroDepot) {
    EXPECT_FALSE(ShouldSubmitCapturedCode(0, 123, 456, true));
}

TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRejectsZeroGid) {
    EXPECT_FALSE(ShouldSubmitCapturedCode(730, 0, 456, true));
}

TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRejectsZeroCode) {
    EXPECT_FALSE(ShouldSubmitCapturedCode(730, 123, 0, true));
}

TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRejectsWhenDisabledEvenWithValidFields) {
    // The critical case: donation was toggled off between HandleSend
    // (tracking gated on the same flag) and this reply arriving -- must
    // still refuse the submit, not rely on the earlier gate alone.
    EXPECT_FALSE(ShouldSubmitCapturedCode(730, 123, 456, /*enabled=*/false));
}

TEST(ManifestDonorLogicTest, ShouldSubmitCapturedCodeRejectsAllZerosRegardlessOfEnabled) {
    EXPECT_FALSE(ShouldSubmitCapturedCode(0, 0, 0, true));
    EXPECT_FALSE(ShouldSubmitCapturedCode(0, 0, 0, false));
}

// ── ParseWantedLine -- happy path ─────────────────────────────────────────
TEST(ManifestDonorLogicTest, ParsesWellFormedLine) {
    WantedEntry e;
    ASSERT_TRUE(ParseWantedLine("730:731:123456789", e));
    EXPECT_EQ(e.appId, 730u);
    EXPECT_EQ(e.depotId, 731u);
    EXPECT_EQ(e.gid, 123456789u);
}

TEST(ManifestDonorLogicTest, AppIdZeroIsValidAndKept) {
    // app_id 0 only satisfies Steam's access check and is not part of the
    // code -- the server may legitimately send it (see ManifestDonor.h /
    // upstream API.md), so it must NOT be treated as a parse failure.
    WantedEntry e;
    ASSERT_TRUE(ParseWantedLine("0:731:123456789", e));
    EXPECT_EQ(e.appId, 0u);
    EXPECT_EQ(e.depotId, 731u);
}

// ── ParseWantedLine -- malformed / malicious input ────────────────────────
TEST(ManifestDonorLogicTest, RejectsZeroDepotId) {
    // Unlike app_id, depot_id 0 is never meaningful -- a request for depot 0
    // would be nonsensical and must be rejected outright.
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:0:123456789", e));
}

TEST(ManifestDonorLogicTest, RejectsZeroGid) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:731:0", e));
}

TEST(ManifestDonorLogicTest, RejectsMissingColon) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730731123456789", e));
}

TEST(ManifestDonorLogicTest, RejectsOnlyOneColon) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:731", e));
}

TEST(ManifestDonorLogicTest, RejectsEmptyLine) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("", e));
}

TEST(ManifestDonorLogicTest, RejectsNonNumericAppId) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("abc:731:123456789", e));
}

TEST(ManifestDonorLogicTest, RejectsNonNumericDepotId) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:abc:123456789", e));
}

TEST(ManifestDonorLogicTest, RejectsNonNumericGid) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:731:abc", e));
}

TEST(ManifestDonorLogicTest, RejectsEmptyFieldBetweenColons) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730::123456789", e));
}

TEST(ManifestDonorLogicTest, RejectsTrailingColonWithEmptyGid) {
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:731:", e));
}

TEST(ManifestDonorLogicTest, RejectsNegativeNumbers) {
    // std::from_chars on an unsigned type rejects a leading '-' outright
    // (it does not silently wrap to a huge value) -- confirm that holds
    // through this parser too.
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:-1:123456789", e));
}

TEST(ManifestDonorLogicTest, ExtraColonInGidFieldIsRejectedNotTruncated) {
    // The gid field is everything after the second colon; from_chars requires
    // the WHOLE remaining substring to be numeric, so a stray extra colon
    // must reject rather than silently parsing only the numeric prefix.
    WantedEntry e;
    EXPECT_FALSE(ParseWantedLine("730:731:123:456", e));
}

TEST(ManifestDonorLogicTest, OutParamUntouchedOnFailure) {
    // A caller that doesn't check the return value should not end up with a
    // half-populated (and therefore misleading) entry.
    WantedEntry e{99, 99, 99};
    EXPECT_FALSE(ParseWantedLine("not:valid", e));
    EXPECT_EQ(e.appId, 99u);
    EXPECT_EQ(e.depotId, 99u);
    EXPECT_EQ(e.gid, 99u);
}
