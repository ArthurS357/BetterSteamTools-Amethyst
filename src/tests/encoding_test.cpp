// Unit tests for OSTPlatform::Encoding — UTF-16 <-> UTF-8 conversion used on
// paths, registry values and config strings. Contract per Encoding.cpp:
// CP_UTF8 with flags=0, so MALFORMED input is replaced with U+FFFD rather than
// rejected (the functions must not crash and must not return "" for merely
// unusual-but-valid input). Empty in -> empty out.

#include "OSTPlatform/include/Encoding.h"

#include <gtest/gtest.h>

#include <string>

using namespace OSTPlatform;

// Helper: keep u8"" literals (char8_t in C++20) usable as std::string without
// changing any production type. Local to the test TU only. Declared before use.
inline std::string u8_to_string(const char8_t* s) {
    return std::string(reinterpret_cast<const char*>(s));
}

// ---------------------------------------------------------------------------
// Happy path + round-trip
// ---------------------------------------------------------------------------
TEST(EncodingRoundTrip, AsciiIsStable) {
    const std::string ascii = "SteamTools/AmethystTool_123";
    EXPECT_EQ(Encoding::WideToUtf8(Encoding::Utf8ToWide(ascii)), ascii);
}

TEST(EncodingRoundTrip, EmptyMapsToEmpty) {
    EXPECT_TRUE(Encoding::WideToUtf8(L"").empty());
    EXPECT_TRUE(Encoding::Utf8ToWide("").empty());
}

// ---------------------------------------------------------------------------
// False-positive guards: legitimate non-ASCII must survive intact
// ---------------------------------------------------------------------------
TEST(EncodingFalsePositive, AccentedPortugueseRoundTrips) {
    // A real install path can contain accented characters; must not be mangled.
    const std::string accented = u8_to_string(u8"C:/Usuários/São Paulo/configuração.toml");
    EXPECT_EQ(Encoding::WideToUtf8(Encoding::Utf8ToWide(accented)), accented);
}

TEST(EncodingFalsePositive, MultibyteAndEmojiRoundTrip) {
    const std::string mixed = u8_to_string(u8"日本語 + café + 🎮");
    const std::wstring wide = Encoding::Utf8ToWide(mixed);
    EXPECT_FALSE(wide.empty());
    EXPECT_EQ(Encoding::WideToUtf8(wide), mixed);
}

TEST(EncodingFalsePositive, EmbeddedNulIsPreserved) {
    // Conversion uses explicit lengths, so an interior NUL is data, not a terminator.
    std::string withNul = "a";
    withNul.push_back('\0');
    withNul += "b"; // "a\0b", size 3
    const std::wstring wide = Encoding::Utf8ToWide(withNul);
    EXPECT_EQ(wide.size(), 3u);
    EXPECT_EQ(Encoding::WideToUtf8(wide), withNul);
}

// ---------------------------------------------------------------------------
// Malicious / malformed: must not crash; replaced, not rejected outright
// ---------------------------------------------------------------------------
TEST(EncodingMalformed, InvalidUtf8DoesNotCrash) {
    // 0xFF/0xFE are never valid UTF-8 lead bytes; a lone 0x80 is a stray
    // continuation byte. flags=0 -> each is replaced with U+FFFD, output non-empty.
    std::string bad = "\xFF\xFE\x80";
    const std::wstring wide = Encoding::Utf8ToWide(bad);
    EXPECT_FALSE(wide.empty()); // replacement chars, not a hard failure
}

TEST(EncodingMalformed, TruncatedMultibyteSequenceHandled) {
    // 0xE6 begins a 3-byte sequence but the following bytes are missing.
    std::string truncated = "abc\xE6";
    const std::wstring wide = Encoding::Utf8ToWide(truncated);
    EXPECT_FALSE(wide.empty()); // "abc" + replacement; no read past the end
}

TEST(EncodingMalformed, LoneHighSurrogateWideToUtf8DoesNotCrash) {
    // 0xD800 is a high surrogate with no low surrogate following — invalid UTF-16.
    std::wstring loneSurrogate;
    loneSurrogate.push_back(static_cast<wchar_t>(0xD800));
    loneSurrogate += L"x";
    const std::string utf8 = Encoding::WideToUtf8(loneSurrogate);
    EXPECT_FALSE(utf8.empty()); // replacement + 'x'
}
