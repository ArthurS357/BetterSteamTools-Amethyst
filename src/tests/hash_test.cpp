// Unit tests for OSTPlatform::Hash::Sha256OfBuffer — the digest that backs the
// RemoteToml cache-integrity check. Contract per Hash.cpp: lower-case hex, 64
// chars; null data with non-zero size returns ""; null+zero and empty hash the
// empty message. Verified against the published SHA-256 test vectors so a broken
// BCrypt wiring can't pass by merely being self-consistent.

#include "OSTPlatform/include/Hash.h"

#include <gtest/gtest.h>

#include <string>

using namespace OSTPlatform;

namespace {
// NIST/FIPS-180-4 published vectors (lower-case).
constexpr const char* kSha256Empty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kSha256Abc =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string HashOf(const std::string& s) {
    return Hash::Sha256OfBuffer(s.data(), s.size());
}
} // namespace

TEST(HashSha256, KnownVectors) {
    EXPECT_EQ(HashOf("abc"), kSha256Abc);
    EXPECT_EQ(HashOf(""), kSha256Empty);
}

TEST(HashSha256, OutputShapeIsLowerHex64) {
    const std::string digest = HashOf("anything");
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_EQ(digest.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(HashSha256, Deterministic) {
    EXPECT_EQ(HashOf("BetterSteamTools"), HashOf("BetterSteamTools"));
}

TEST(HashSha256, DistinctInputsDistinctDigests) {
    // One-bit-ish difference must diverge (no accidental collision / truncation).
    EXPECT_NE(HashOf("payload-a"), HashOf("payload-b"));
}

TEST(HashSha256, NullWithZeroSizeHashesEmptyMessage) {
    // The guard only rejects null WITH a non-zero size; null+0 is the empty message.
    EXPECT_EQ(Hash::Sha256OfBuffer(nullptr, 0), kSha256Empty);
}

TEST(HashSha256, NullWithNonZeroSizeFailsClosed) {
    // A lying (data=null, size>0) call must return "" and never dereference null.
    EXPECT_TRUE(Hash::Sha256OfBuffer(nullptr, 32).empty());
}

TEST(HashSha256, EmbeddedNulBytesAreHashed) {
    // The digest must cover the whole buffer, including interior NULs — otherwise
    // two distinct payloads that share a prefix before a NUL would collide.
    std::string a("A\0A", 3);
    std::string b("A\0B", 3);
    EXPECT_EQ(a.size(), 3u);
    EXPECT_NE(HashOf(a), HashOf(b));
}
