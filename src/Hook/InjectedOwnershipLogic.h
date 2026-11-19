#pragma once

#include "Steam/Structs.h"

// Pure part of the CheckAppOwnership hook (Hooks_Package.cpp), split out so the shape of the
// answer the hook hands back for a lua-declared app is testable without a live Steam.
//
// There is no "fake license" struct to classify: the injection extends the AppIdVec of Steam's
// own package 0 (PackageInfo carries no owner field), and what the UI and the depot code read
// as "whose license is this" is the AppOwnership answer. The hook used to set PackageId /
// ReleaseState / bOwnsLicense / bFreeLicense and leave every other field as Steam wrote it.
// When Steam's own answer for the app already carried a lent-license marker (bBorrowed or
// bFamilyShared, with SteamId32 naming the lender), the app was handed back as owned but still
// flagged lent, which is how it can show up under "from your Steam family library".
//
// This only normalises OUR answer for an app a lua declares. It does not touch Steam's
// own Family Sharing validation or any network message (the v1.3.6 logout came from
// dropping those, not from this struct).
namespace InjectedOwnershipLogic {

    // Steam's answer marks the app as lent by someone else's library.
    [[nodiscard]] inline bool ClaimsLentLicense(const AppOwnership& o) {
        return o.bBorrowed || o.bFamilyShared;
    }

    // Rewrites `o` into "owned directly through `packageId`". `self` is the active account
    // (0 if unknown); it replaces the lender in SteamId32 only when a lent marker was
    // present, so an answer that was never lent keeps whatever Steam put there.
    // Returns true when a lent marker was cleared.
    inline bool MarkDirectOwnership(AppOwnership& o, PackageId_t packageId, AccountID_t self) {
        const bool lent = ClaimsLentLicense(o);

        o.PackageId    = packageId;
        o.ReleaseState = EAppReleaseState::Released;
        o.bOwnsLicense = true;   // also forces DLCs of family-shared games that are not owned
        o.bFreeLicense = false;  // true would hide the app from the library UI

        o.bBorrowed     = false;
        o.bFamilyShared = false;
        if (lent && self != 0) o.SteamId32 = self;
        return lent;
    }

}  // namespace InjectedOwnershipLogic
