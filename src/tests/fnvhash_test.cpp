// Unit tests for Fnv1aHash — the compile-time FNV-1a 32-bit hash used to switch
// on interface/method names. Verified against the canonical FNV-1a 32 test
// vectors and exercised as a constant expression (its whole reason to exist).

#include "Utils/Support/FnvHash.h"

#include <gtest/gtest.h>

#include <cstdint>

// Canonical FNV-1a 32-bit vectors (ASCII inputs, so no signed-char sign-extension
// ambiguity). offset basis 0x811c9dc5, prime 0x01000193.
TEST(Fnv1aHash, KnownVectors) {
    EXPECT_EQ(Fnv1aHash(""), 0x811c9dc5u);       // empty -> offset basis
    EXPECT_EQ(Fnv1aHash("a"), 0xe40c292cu);
    EXPECT_EQ(Fnv1aHash("foobar"), 0xbf9cf968u);
}

TEST(Fnv1aHash, Deterministic) {
    EXPECT_EQ(Fnv1aHash("ISteamUser"), Fnv1aHash("ISteamUser"));
}

TEST(Fnv1aHash, DistinctInputsDiffer) {
    EXPECT_NE(Fnv1aHash("ISteamUser"), Fnv1aHash("ISteamUtils"));
    EXPECT_NE(Fnv1aHash("GetAppOwnershipTicket"), Fnv1aHash("GetEncryptedAppTicket"));
}

TEST(Fnv1aHash, UsableInConstantExpression) {
    // The switch-on-hash pattern depends on this being a core constant expression.
    // A known-value static_assert proves both compile-time evaluation AND
    // correctness (a tautological self-comparison would prove neither).
    static_assert(Fnv1aHash("") == 0x811c9dc5u,
                  "Fnv1aHash must evaluate correctly at compile time");
    constexpr uint32_t h = Fnv1aHash("compile-time");
    EXPECT_EQ(h, Fnv1aHash("compile-time"));
}
