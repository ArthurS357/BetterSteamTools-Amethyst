// Unit tests for InjectedOwnershipLogic (Hook/InjectedOwnershipLogic.h): the AppOwnership
// answer the CheckAppOwnership hook returns for a lua-declared app. Regression for apps that
// showed up as "from your Steam family library" because Steam's own lent-license markers
// were passed through next to bOwnsLicense = true.

#include "Hook/InjectedOwnershipLogic.h"

#include <gtest/gtest.h>

#include <type_traits>

using InjectedOwnershipLogic::ClaimsLentLicense;
using InjectedOwnershipLogic::MarkDirectOwnership;

namespace {

    constexpr AccountID_t kSelf   = 0x01234567;
    constexpr AccountID_t kLender = 0x0BADF00D;

    // The hook writes through a pointer into Steam's struct: only plain-old-data is safe.
    static_assert(std::is_trivially_copyable_v<AppOwnership>);

    AppOwnership LentAnswer() {
        AppOwnership o{};
        o.bBorrowed     = true;
        o.bFamilyShared = true;
        o.SteamId32     = kLender;
        o.ReleaseState  = EAppReleaseState::Unavailable;
        o.bFreeLicense  = true;
        return o;
    }

    TEST(InjectedOwnershipLogicTest, ClearsLentMarkersAndClaimsDirectOwnership) {
        AppOwnership o = LentAnswer();
        EXPECT_TRUE(ClaimsLentLicense(o));

        EXPECT_TRUE(MarkDirectOwnership(o, /*packageId=*/0, kSelf));

        EXPECT_FALSE(o.bBorrowed);
        EXPECT_FALSE(o.bFamilyShared);
        EXPECT_FALSE(ClaimsLentLicense(o));
        EXPECT_EQ(o.SteamId32, kSelf);              // the lender is replaced by the active account
        EXPECT_TRUE(o.bOwnsLicense);
        EXPECT_FALSE(o.bFreeLicense);               // true would hide the app from the library
        EXPECT_EQ(o.PackageId, 0u);
        EXPECT_EQ(o.ReleaseState, EAppReleaseState::Released);
    }

    // Each marker alone is enough to count as lent.
    TEST(InjectedOwnershipLogicTest, EitherMarkerAloneIsLent) {
        AppOwnership a{}; a.bBorrowed = true;
        AppOwnership b{}; b.bFamilyShared = true;
        EXPECT_TRUE(ClaimsLentLicense(a));
        EXPECT_TRUE(ClaimsLentLicense(b));
        EXPECT_TRUE(MarkDirectOwnership(a, 0, kSelf));
        EXPECT_TRUE(MarkDirectOwnership(b, 0, kSelf));
        EXPECT_FALSE(a.bBorrowed);
        EXPECT_FALSE(b.bFamilyShared);
    }

    // An answer that was never lent keeps its SteamId32: only the flags and the fields the hook
    // always set are written. Reports false so the hook does not log a correction.
    TEST(InjectedOwnershipLogicTest, NotLentKeepsOwnerId) {
        AppOwnership o{};
        o.SteamId32 = kLender;   // whatever Steam wrote; not ours to rewrite without a lent marker

        EXPECT_FALSE(MarkDirectOwnership(o, 0, kSelf));

        EXPECT_EQ(o.SteamId32, kLender);
        EXPECT_TRUE(o.bOwnsLicense);
        EXPECT_FALSE(o.bBorrowed);
        EXPECT_FALSE(o.bFamilyShared);
    }

    // Unknown active account (credential store unreadable): clear the flags but never write
    // an id of 0 over the owner field.
    TEST(InjectedOwnershipLogicTest, UnknownSelfClearsFlagsButKeepsOwnerId) {
        AppOwnership o = LentAnswer();

        EXPECT_TRUE(MarkDirectOwnership(o, 0, /*self=*/0));

        EXPECT_FALSE(o.bBorrowed);
        EXPECT_FALSE(o.bFamilyShared);
        EXPECT_EQ(o.SteamId32, kLender);
        EXPECT_TRUE(o.bOwnsLicense);
    }

    // Fields this logic must not touch: they belong to Steam's own bookkeeping.
    TEST(InjectedOwnershipLogicTest, LeavesUnrelatedFieldsAlone) {
        AppOwnership o = LentAnswer();
        o.ExistInPackageNums = 1;
        o.TimeStamp          = 1700000000;
        o.TimeExpire         = 0;
        o.bLicenseExpired    = false;
        o.bIsPermanent       = true;
        o.bLicenseLocked     = false;

        MarkDirectOwnership(o, 0, kSelf);

        EXPECT_EQ(o.ExistInPackageNums, 1u);
        EXPECT_EQ(o.TimeStamp, 1700000000u);
        EXPECT_EQ(o.TimeExpire, 0u);
        EXPECT_FALSE(o.bLicenseExpired);
        EXPECT_TRUE(o.bIsPermanent);
        EXPECT_FALSE(o.bLicenseLocked);
    }

    // Idempotent: the hook runs on every CheckAppOwnership call for the app.
    TEST(InjectedOwnershipLogicTest, SecondCallChangesNothing) {
        AppOwnership o = LentAnswer();
        EXPECT_TRUE(MarkDirectOwnership(o, 0, kSelf));

        EXPECT_FALSE(MarkDirectOwnership(o, 0, kSelf));
        EXPECT_EQ(o.SteamId32, kSelf);
        EXPECT_FALSE(ClaimsLentLicense(o));
        EXPECT_TRUE(o.bOwnsLicense);
        EXPECT_FALSE(o.bFreeLicense);
    }

}  // namespace
