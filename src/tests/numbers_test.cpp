// Unit tests for OSTPlatform::Numbers — the strict integer parsers used on
// external input (config values, URI arguments, registry strings). Contract per
// Numbers.h / Numbers.cpp: std::from_chars-backed, must consume the WHOLE input,
// value must fit the target type, empty rejected, no leading '+', no whitespace,
// no thousands separators. Hex accepts one optional 0x/0X prefix.
//
// Three categories throughout: happy path, malicious/malformed input, and
// false-positive guards (legitimate-but-unusual input that must NOT be rejected).
// EXPECT_* (not ASSERT_*) so a single wrong case doesn't hide the others.

#include "OSTPlatform/include/Numbers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using namespace OSTPlatform;

// ---------------------------------------------------------------------------
// ParseUInt64 — decimal, unsigned
// ---------------------------------------------------------------------------
TEST(NumbersParseUInt64, HappyPath) {
    EXPECT_EQ(Numbers::ParseUInt64("0"), std::optional<uint64_t>{0});
    EXPECT_EQ(Numbers::ParseUInt64("1"), std::optional<uint64_t>{1});
    EXPECT_EQ(Numbers::ParseUInt64("42"), std::optional<uint64_t>{42});
    // Steam ids are 64-bit; a real one must parse.
    EXPECT_EQ(Numbers::ParseUInt64("76561197960287930"),
              std::optional<uint64_t>{76561197960287930ull});
}

TEST(NumbersParseUInt64, FalsePositiveGuards) {
    // Exact maximum must be accepted, not mistaken for overflow.
    EXPECT_EQ(Numbers::ParseUInt64("18446744073709551615"),
              std::optional<uint64_t>{std::numeric_limits<uint64_t>::max()});
    // Leading zeros are legitimate (from_chars consumes them); value is 123.
    EXPECT_EQ(Numbers::ParseUInt64("00123"), std::optional<uint64_t>{123});
}

TEST(NumbersParseUInt64, MaliciousAndMalformed) {
    EXPECT_FALSE(Numbers::ParseUInt64("").has_value());                     // empty
    EXPECT_FALSE(Numbers::ParseUInt64("18446744073709551616").has_value()); // max + 1 (overflow)
    EXPECT_FALSE(Numbers::ParseUInt64("-1").has_value());                   // negative into unsigned
    EXPECT_FALSE(Numbers::ParseUInt64("+5").has_value());                   // from_chars rejects '+'
    EXPECT_FALSE(Numbers::ParseUInt64(" 12").has_value());                  // leading space
    EXPECT_FALSE(Numbers::ParseUInt64("12 ").has_value());                  // trailing space
    EXPECT_FALSE(Numbers::ParseUInt64("12abc").has_value());               // trailing junk
    EXPECT_FALSE(Numbers::ParseUInt64("0x1F").has_value());                // hex not valid decimal
    EXPECT_FALSE(Numbers::ParseUInt64("1e3").has_value());                 // scientific notation
    EXPECT_FALSE(Numbers::ParseUInt64("12;34").has_value());               // shell metachar embedded
    EXPECT_FALSE(Numbers::ParseUInt64("1,000").has_value());               // thousands separator
    // Explicit UTF-8 bytes for the full-width digits U+FF11 U+FF12 U+FF13, so
    // the case tests the real high bytes regardless of the TU's source charset
    // (the test target has no /utf-8). from_chars must reject them as non-digits.
    EXPECT_FALSE(Numbers::ParseUInt64("\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93").has_value());
}

// ---------------------------------------------------------------------------
// ParseUInt32 — decimal, unsigned, tighter bound catches truncation bugs
// ---------------------------------------------------------------------------
TEST(NumbersParseUInt32, BoundaryAndOverflow) {
    EXPECT_EQ(Numbers::ParseUInt32("4294967295"),
              std::optional<uint32_t>{std::numeric_limits<uint32_t>::max()});
    // 2^32 must not silently truncate to 0.
    EXPECT_FALSE(Numbers::ParseUInt32("4294967296").has_value());
    // A value that fits uint64 but not uint32 must be rejected by the 32-bit parser.
    EXPECT_FALSE(Numbers::ParseUInt32("18446744073709551615").has_value());
}

// ---------------------------------------------------------------------------
// ParseInt64 / ParseInt32 — signed
// ---------------------------------------------------------------------------
TEST(NumbersParseSigned, HappyAndBoundary) {
    EXPECT_EQ(Numbers::ParseInt64("-1"), std::optional<int64_t>{-1});
    EXPECT_EQ(Numbers::ParseInt32("-2147483648"),
              std::optional<int32_t>{std::numeric_limits<int32_t>::min()});
    EXPECT_EQ(Numbers::ParseInt32("2147483647"),
              std::optional<int32_t>{std::numeric_limits<int32_t>::max()});
}

TEST(NumbersParseSigned, OverflowAndSignRules) {
    EXPECT_FALSE(Numbers::ParseInt32("2147483648").has_value());   // INT32_MAX + 1
    EXPECT_FALSE(Numbers::ParseInt32("-2147483649").has_value());  // INT32_MIN - 1
    EXPECT_FALSE(Numbers::ParseInt64("+7").has_value());           // '+' rejected
    EXPECT_FALSE(Numbers::ParseInt64("--7").has_value());          // double sign
}

// ---------------------------------------------------------------------------
// ParseHexUInt* — hex with optional single 0x/0X prefix
// ---------------------------------------------------------------------------
TEST(NumbersParseHex, HappyPath) {
    EXPECT_EQ(Numbers::ParseHexUInt32("1F"), std::optional<uint32_t>{0x1F});
    EXPECT_EQ(Numbers::ParseHexUInt32("0x1f"), std::optional<uint32_t>{0x1F});
    EXPECT_EQ(Numbers::ParseHexUInt32("0XABCD"), std::optional<uint32_t>{0xABCD});
    EXPECT_EQ(Numbers::ParseHexUInt64("deadBEEF"), std::optional<uint64_t>{0xDEADBEEFull});
}

TEST(NumbersParseHex, MaliciousAndMalformed) {
    EXPECT_FALSE(Numbers::ParseHexUInt32("0x").has_value());   // prefix only, no digits
    EXPECT_FALSE(Numbers::ParseHexUInt32("").has_value());     // empty
    EXPECT_FALSE(Numbers::ParseHexUInt32("0xg1").has_value()); // non-hex digit
    EXPECT_FALSE(Numbers::ParseHexUInt32("0x0x1").has_value());// double prefix (only one stripped)
    EXPECT_FALSE(Numbers::ParseHexUInt32("-1").has_value());   // negative into unsigned
    EXPECT_FALSE(Numbers::ParseHexUInt32("1F ").has_value());  // trailing space
}

TEST(NumbersParseHexUInt8, ByteBoundary) {
    EXPECT_EQ(Numbers::ParseHexUInt8("00"), std::optional<uint8_t>{0x00});
    EXPECT_EQ(Numbers::ParseHexUInt8("ff"), std::optional<uint8_t>{0xFF});   // exact max byte
    EXPECT_EQ(Numbers::ParseHexUInt8("0xFF"), std::optional<uint8_t>{0xFF});
    EXPECT_FALSE(Numbers::ParseHexUInt8("100").has_value());                 // 256 > uint8 max
}

// ---------------------------------------------------------------------------
// TextSlice overload — offset/count windowing into a larger buffer
// ---------------------------------------------------------------------------
TEST(NumbersTextSlice, ParsesSubrange) {
    // "id=1234;" — parse just the "1234" window (offset 3, count 4).
    EXPECT_EQ(Numbers::ParseUInt32(std::string_view{"id=1234;"}, Numbers::TextSlice{3, 4}),
              std::optional<uint32_t>{1234});
}

TEST(NumbersTextSlice, RejectsOutOfRangeOffset) {
    // Offset past the end must fail closed, not read out of bounds.
    EXPECT_FALSE(
        Numbers::ParseUInt32(std::string_view{"abc"}, Numbers::TextSlice{10, 4}).has_value());
}

TEST(NumbersTextSlice, SliceThatStillLeavesJunkIsRejected) {
    // Window "1234;" includes the trailing ';' -> not fully consumed -> reject.
    EXPECT_FALSE(
        Numbers::ParseUInt32(std::string_view{"id=1234;"}, Numbers::TextSlice{3, 5}).has_value());
}

// ---------------------------------------------------------------------------
// Wide-string overloads — must agree with the narrow ones after UTF-16->UTF-8
// ---------------------------------------------------------------------------
TEST(NumbersWide, MatchesNarrow) {
    EXPECT_EQ(Numbers::ParseUInt64(L"12345"), std::optional<uint64_t>{12345});
    EXPECT_EQ(Numbers::ParseHexUInt32(L"0x1F"), std::optional<uint32_t>{0x1F});
    EXPECT_FALSE(Numbers::ParseUInt64(L"12x").has_value());
}

// ---------------------------------------------------------------------------
// const char* overloads — nullptr must be handled, never dereferenced
// ---------------------------------------------------------------------------
TEST(NumbersCharPtr, NullAndValid) {
    const char* nullInput = nullptr;
    EXPECT_FALSE(Numbers::ParseUInt64(nullInput).has_value());
    EXPECT_EQ(Numbers::ParseUInt64("999"), std::optional<uint64_t>{999});
}
