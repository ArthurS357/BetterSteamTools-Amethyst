// Unit tests for LicenseListLogic -- the pure parsing logic behind
// CMsgClientLicenseList (eMsg 780, see Hook/Hooks_NetPacket.cpp). Extracted
// specifically so it is testable without HookMacros.h's Detour dependency or
// the rest of the hook engine.
//
// Three categories throughout: happy path, malicious/malformed input, and
// false-positive guards (legitimate-but-unusual input that must NOT be
// rejected).

#include "Hook/LicenseListLogic.h"

#include "steam_messages.pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace LicenseListLogic;

namespace {

// Builds a serialized CMsgClientLicenseList from (package_id, access_token)
// pairs. A zero access_token is left unset (has_access_token() == false),
// mirroring a license entry Steam sends with no token field at all.
std::string BuildLicenseList(std::initializer_list<std::pair<uint32_t, uint64_t>> licenses) {
    CMsgClientLicenseList msg;
    for (const auto& [packageId, accessToken] : licenses) {
        auto* lic = msg.add_licenses();
        lic->set_package_id(packageId);
        if (accessToken) lic->set_access_token(accessToken);
    }
    std::string bytes;
    msg.SerializeToString(&bytes);
    return bytes;
}

const uint8_t* Bytes(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

} // namespace

// ---------------------------------------------------------------------------
// ParseOwnedLicenses -- happy path
// ---------------------------------------------------------------------------
TEST(ParseOwnedLicenses, HappyPathMultipleLicenses) {
    const std::string body = BuildLicenseList({{1001, 111111}, {2002, 222222}, {3003, 333333}});

    std::vector<LicenseEntry> out;
    ASSERT_TRUE(ParseOwnedLicenses(Bytes(body), static_cast<uint32_t>(body.size()), out));

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].packageId, 1001u);
    EXPECT_EQ(out[0].accessToken, 111111u);
    EXPECT_EQ(out[1].packageId, 2002u);
    EXPECT_EQ(out[1].accessToken, 222222u);
    EXPECT_EQ(out[2].packageId, 3003u);
    EXPECT_EQ(out[2].accessToken, 333333u);
}

TEST(ParseOwnedLicenses, MissingAccessTokenDefaultsToZero) {
    const std::string body = BuildLicenseList({{4004, 0}});

    std::vector<LicenseEntry> out;
    ASSERT_TRUE(ParseOwnedLicenses(Bytes(body), static_cast<uint32_t>(body.size()), out));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].packageId, 4004u);
    EXPECT_EQ(out[0].accessToken, 0u);
}

TEST(ParseOwnedLicenses, EmptyLicenseListParsesToEmptyOutput) {
    // A CMsgClientLicenseList with zero License entries is a well-formed
    // message (an account can genuinely hold no packages), distinct from a
    // parse failure -- callers must be able to tell the two apart.
    const std::string body = BuildLicenseList({});

    std::vector<LicenseEntry> out{{9999, 9999}};   // pre-seeded, must be cleared
    ASSERT_TRUE(ParseOwnedLicenses(Bytes(body), static_cast<uint32_t>(body.size()), out));
    EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// ParseOwnedLicenses -- malicious / malformed input
// ---------------------------------------------------------------------------
TEST(ParseOwnedLicenses, CorruptedPayloadFailsClosed) {
    const uint8_t garbage[] = {0xFF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};

    std::vector<LicenseEntry> out{{1234, 5678}};   // sentinel, must be left untouched
    EXPECT_FALSE(ParseOwnedLicenses(garbage, sizeof(garbage), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].packageId, 1234u);
    EXPECT_EQ(out[0].accessToken, 5678u);
}

TEST(ParseOwnedLicenses, TruncatedMessageFailsClosed) {
    const std::string body = BuildLicenseList({{1001, 111111}});
    // Cut the serialized message mid-field so it cannot possibly parse.
    const std::string truncated = body.substr(0, body.size() > 1 ? body.size() - 1 : 0);

    std::vector<LicenseEntry> out;
    EXPECT_FALSE(ParseOwnedLicenses(Bytes(truncated), static_cast<uint32_t>(truncated.size()), out));
}

TEST(ParseOwnedLicenses, NullBodyRejectedRegardlessOfSize) {
    std::vector<LicenseEntry> out;
    EXPECT_FALSE(ParseOwnedLicenses(nullptr, 0, out));
    EXPECT_FALSE(ParseOwnedLicenses(nullptr, 64, out));
}

TEST(ParseOwnedLicenses, OversizedLengthRejectedWithoutTouchingTheBuffer) {
    // A real caller can only ever pass a size describing an actual buffer, but
    // this function is the last line of defense against a corrupted/adversarial
    // length: `size` here claims ~4 GB while `small` is 4 bytes. Without the
    // int-overflow guard, casting size to `int` truncates/goes negative and
    // protobuf would be asked to read up to ~4 GB starting at `small` --
    // exactly the out-of-bounds read a hostile or buggy caller could trigger.
    // The function must reject this before ever calling into protobuf.
    const uint8_t small[4] = {0x01, 0x02, 0x03, 0x04};

    std::vector<LicenseEntry> out;
    EXPECT_FALSE(ParseOwnedLicenses(small, 0xFFFFFFFFu, out));
    EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// ParseOwnedLicenses -- false-positive guards
// ---------------------------------------------------------------------------
TEST(ParseOwnedLicenses, LicenseMissingPackageIdIsSkippedNotRejected) {
    // A license entry with no package_id carries nothing GetPackageInfo could
    // resolve, so it is silently skipped -- it must NOT fail the whole parse
    // (Steam's real license lists are large; one odd entry should not drop
    // every other package the account owns).
    CMsgClientLicenseList msg;
    msg.add_licenses()->set_access_token(777);          // no package_id set
    auto* good = msg.add_licenses();
    good->set_package_id(5005);
    good->set_access_token(888);

    std::string body;
    ASSERT_TRUE(msg.SerializeToString(&body));

    std::vector<LicenseEntry> out;
    ASSERT_TRUE(ParseOwnedLicenses(Bytes(body), static_cast<uint32_t>(body.size()), out));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].packageId, 5005u);
    EXPECT_EQ(out[0].accessToken, 888u);
}

TEST(ParseOwnedLicenses, PackageIdZeroIsKeptWhenExplicitlyPresent) {
    // package_id == 0 is a legitimate (if unusual) value once the field is
    // actually present -- only its ABSENCE (has_package_id() == false) is a
    // skip condition, not the value zero.
    const std::string body = BuildLicenseList({{0, 42}});

    std::vector<LicenseEntry> out;
    ASSERT_TRUE(ParseOwnedLicenses(Bytes(body), static_cast<uint32_t>(body.size()), out));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].packageId, 0u);
    EXPECT_EQ(out[0].accessToken, 42u);
}
