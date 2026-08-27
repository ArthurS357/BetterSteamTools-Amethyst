// Unit tests for OSTPlatform::Encoding — UTF-16 <-> UTF-8 conversion used on
// paths, registry values and config strings. Contract per Encoding.cpp:
// CP_UTF8 with flags=0, so MALFORMED input is replaced with U+FFFD rather than
// rejected (the functions must not crash and must not return "" for merely
// unusual-but-valid input). Empty in -> empty out.
//
// Utf8ToPath/PathToUtf8 below cover the companion bug: std::filesystem::path
// (std::string) and path::string() decode/encode via the host's ANSI codepage
// on MSVC, not UTF-8 -- these two always mean UTF-8 regardless of codepage
// (see Encoding.h). The non-ASCII cases here are specifically ones that would
// mis-round-trip through the ANSI-codepage constructor even on a codepage
// that "supports" accented Latin characters (e.g. Western European 1252),
// because UTF-8's multi-byte sequences aren't ANSI byte sequences at all.

#include "OSTPlatform/include/Encoding.h"

#include <gtest/gtest.h>

#include <filesystem>
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

// ---------------------------------------------------------------------------
// Utf8ToPath / PathToUtf8 — filesystem::path via the C++20 char8_t
// constructor, bypassing MSVC's ANSI-codepage path(std::string)/string().
// ---------------------------------------------------------------------------
TEST(EncodingPathRoundTrip, AsciiIsStable) {
    const std::string ascii = "SteamTools\\AmethystTool_123.toml";
    EXPECT_EQ(Encoding::PathToUtf8(Encoding::Utf8ToPath(ascii)), ascii);
}

TEST(EncodingPathRoundTrip, EmptyMapsToEmpty) {
    EXPECT_TRUE(Encoding::Utf8ToPath("").empty());
    EXPECT_TRUE(Encoding::PathToUtf8(std::filesystem::path()).empty());
}

TEST(EncodingPathFalsePositive, AccentedPortugueseFilenameRoundTrips) {
    // The exact class of real-world path this bug affects: a Windows user profile or
    // Steam install directory with accented characters. std::filesystem::path(str) on
    // MSVC decodes via the host's ANSI codepage, not UTF-8 -- wrong even on a codepage
    // that "supports" accented Latin characters (e.g. Western European 1252), because
    // UTF-8's multi-byte sequences are not ANSI byte sequences at all.
    const std::string accented = u8_to_string(u8"Usuários São Paulo configuração.toml");
    EXPECT_EQ(Encoding::PathToUtf8(Encoding::Utf8ToPath(accented)), accented);
}

TEST(EncodingPathFalsePositive, MultibyteFilenameRoundTrips) {
    const std::string mixed = u8_to_string(u8"日本語ファイル名.toml");
    const std::filesystem::path path = Encoding::Utf8ToPath(mixed);
    EXPECT_FALSE(path.empty());
    EXPECT_EQ(Encoding::PathToUtf8(path), mixed);
}

TEST(EncodingPathFalsePositive, JoinPreservesNonAsciiParent) {
    // Mirrors the actual call pattern (Config.cpp, RemoteToml.cpp, CloudRedirectHost.cpp,
    // ...): decode a UTF-8 base directory once via Utf8ToPath, then join an
    // ASCII-literal component onto it with operator/.
    const std::string parent = u8_to_string(u8"C:\\Jogos Steam\\configuração");
    const std::filesystem::path joined = Encoding::Utf8ToPath(parent) / "amethysttool.toml";
    EXPECT_EQ(joined.filename().string(), "amethysttool.toml");
    // Round-tripping the joined path's UTF-8 rendering back through Utf8ToPath must
    // reproduce the identical path -- confirms the non-ASCII parent survives the join
    // uncorrupted, not just that the ASCII suffix does.
    EXPECT_EQ(Encoding::Utf8ToPath(Encoding::PathToUtf8(joined)), joined);
}

// ---------------------------------------------------------------------------
// Malicious / malformed: must degrade safely, never crash.
// ---------------------------------------------------------------------------
TEST(EncodingPathMalformed, InvalidUtf8DoesNotCrash) {
    // No in-tree caller can actually produce this: WideToUtf8 always emits
    // well-formed UTF-8, and TOML content is validated as UTF-8 by toml++ during
    // parsing before it ever reaches Utf8ToPath. This is defense in depth, not a
    // documented contract -- unlike Utf8ToWide/WideToUtf8 above, the standard's
    // char8_t path constructor is free to throw on malformed input instead of
    // substituting U+FFFD, so both outcomes are accepted here; only a hard crash
    // (which no catch clause could stop anyway) would be a real failure.
    const std::string bad = "\xFF\xFE\x80";
    try {
        const std::filesystem::path path = Encoding::Utf8ToPath(bad);
        (void)Encoding::PathToUtf8(path);
    } catch (const std::exception&) {
        // Controlled, catchable failure on malformed input is acceptable here.
    }
}
