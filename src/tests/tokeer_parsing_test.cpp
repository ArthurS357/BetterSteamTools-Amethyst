// Unit tests for TokeerBridge::Parsing — the pure hex/JSON/URI parsing helpers
// behind the amethysttool:// bridge (code redemption). Extracted from
// TokeerBridge.cpp specifically so they're testable without Http/Dialog/
// SteamCredentialStore (see TokeerParsing.h).
//
// Three categories throughout: happy path, malicious/malformed input, and
// false-positive guards (legitimate-but-unusual input that must NOT be rejected).

#include "Utils/Tokeer/TokeerParsing.h"

#include <gtest/gtest.h>

using namespace TokeerBridge::Parsing;

// ---------------------------------------------------------------------------
// HexToBytes
// ---------------------------------------------------------------------------
TEST(HexToBytes, HappyPath) {
    const auto bytes = HexToBytes("deadBEEF");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(HexToBytes, MaliciousAndMalformed) {
    EXPECT_FALSE(HexToBytes("").has_value());       // empty
    EXPECT_FALSE(HexToBytes("a").has_value());       // odd length
    EXPECT_FALSE(HexToBytes("zz").has_value());      // non-hex chars
    EXPECT_FALSE(HexToBytes("de ad").has_value());   // embedded whitespace, not a separator here
    EXPECT_FALSE(HexToBytes("0xAB").has_value());    // "0x" prefix not accepted, 'x' is not hex
}

TEST(HexToBytes, FalsePositiveGuards) {
    EXPECT_EQ(HexToBytes("00"), (std::optional<std::vector<uint8_t>>{{0x00}}));
    EXPECT_EQ(HexToBytes("FF"), (std::optional<std::vector<uint8_t>>{{0xFF}}));
    // Mixed case is legitimate.
    EXPECT_EQ(HexToBytes("aAbB"), (std::optional<std::vector<uint8_t>>{{0xAA, 0xBB}}));
}

// ---------------------------------------------------------------------------
// JsonString — minimal flat "key":"value" extraction
// ---------------------------------------------------------------------------
TEST(JsonString, HappyPath) {
    std::string out;
    EXPECT_TRUE(JsonString(R"({"app_id":"1361510","reason":"ok"})", "app_id", out));
    EXPECT_EQ(out, "1361510");
    EXPECT_TRUE(JsonString(R"({"app_id":"1361510","reason":"ok"})", "reason", out));
    EXPECT_EQ(out, "ok");
}

TEST(JsonString, MaliciousAndMalformed) {
    std::string out = "unchanged";
    EXPECT_FALSE(JsonString(R"({"other":"x"})", "app_id", out));       // key absent
    EXPECT_EQ(out, "unchanged");                                       // must not clobber on failure
    EXPECT_FALSE(JsonString(R"({"app_id":123})", "app_id", out));      // value is a number, not a string
    EXPECT_FALSE(JsonString(R"({"app_id":null})", "app_id", out));     // value is null
    EXPECT_FALSE(JsonString(R"({"app_id":)", "app_id", out));          // truncated body
    EXPECT_FALSE(JsonString("", "app_id", out));                       // empty body
}

TEST(JsonString, FalsePositiveGuards) {
    std::string out;
    // Empty string value is legitimate JSON, must not be confused with "absent".
    EXPECT_TRUE(JsonString(R"({"reason":""})", "reason", out));
    EXPECT_EQ(out, "");
    // Value containing a colon/comma-like substring must not confuse the scan.
    EXPECT_TRUE(JsonString(R"({"note":"a: b, c"})", "note", out));
    EXPECT_EQ(out, "a: b, c");
}

// ---------------------------------------------------------------------------
// JsonTrue
// ---------------------------------------------------------------------------
TEST(JsonTrue, HappyPath) {
    EXPECT_TRUE(JsonTrue(R"({"success":true})", "success"));
    EXPECT_TRUE(JsonTrue(R"({"success": true })", "success")); // incidental whitespace
}

TEST(JsonTrue, MaliciousAndMalformed) {
    EXPECT_FALSE(JsonTrue(R"({"success":false})", "success"));
    EXPECT_FALSE(JsonTrue(R"({"success":"true"})", "success")); // quoted string, not the literal
    EXPECT_FALSE(JsonTrue(R"({"other":true})", "success"));     // key absent
    EXPECT_FALSE(JsonTrue("", "success"));
}

// ---------------------------------------------------------------------------
// ParseUri — amethysttool:// scheme cleanup + action/arg split
// ---------------------------------------------------------------------------
TEST(ParseUri, HappyPath) {
    const auto p = ParseUri("amethysttool://redeem/ABC123");
    EXPECT_TRUE(p.matchedScheme);
    EXPECT_EQ(p.action, "redeem");
    EXPECT_EQ(p.arg, "ABC123");
}

TEST(ParseUri, StripsRundll32QuotingAndWhitespace) {
    // rundll32 leaves the literal quotes from the registry command's "%1" in argv.
    const auto p = ParseUri("  \"amethysttool://redeem/ABC123\"  ");
    EXPECT_TRUE(p.matchedScheme);
    EXPECT_EQ(p.action, "redeem");
    EXPECT_EQ(p.arg, "ABC123");
}

TEST(ParseUri, StripsTrailingSlashAndNewline) {
    const auto p = ParseUri("amethysttool://redeem/ABC123/\r\n");
    EXPECT_TRUE(p.matchedScheme);
    EXPECT_EQ(p.action, "redeem");
    EXPECT_EQ(p.arg, "ABC123");
}

TEST(ParseUri, NoArgSegment) {
    const auto p = ParseUri("amethysttool://redeem");
    EXPECT_TRUE(p.matchedScheme);
    EXPECT_EQ(p.action, "redeem");
    EXPECT_EQ(p.arg, "");
}

TEST(ParseUri, MaliciousAndMalformed) {
    EXPECT_FALSE(ParseUri("http://example.com/redeem/ABC").matchedScheme); // wrong scheme
    EXPECT_FALSE(ParseUri("").matchedScheme);                              // empty
    EXPECT_FALSE(ParseUri("amethysttoolx://redeem/ABC").matchedScheme);    // scheme prefix, not exact
    EXPECT_FALSE(ParseUri("not-a-uri-at-all").matchedScheme);
}

TEST(ParseUri, FalsePositiveGuards) {
    // A single mismatched wrapping quote must NOT be stripped (only a matched pair is).
    const auto p = ParseUri("\"amethysttool://redeem/ABC123");
    EXPECT_FALSE(p.matchedScheme);

    // An arg that itself contains a slash: split only on the FIRST '/' after the scheme.
    const auto withSlash = ParseUri("amethysttool://redeem/AB/CD");
    EXPECT_TRUE(withSlash.matchedScheme);
    EXPECT_EQ(withSlash.action, "redeem");
    EXPECT_EQ(withSlash.arg, "AB/CD");
}
