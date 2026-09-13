#pragma once

#include "dllmain.h"

#include <cstdint>
#include <vector>

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // One entry of the license list Steam sends after logon. The access token is
    // required: GetPackageInfo will not return a PackageInfo without it.
    struct License {
        PackageId_t packageId    = 0;
        uint64_t    accessToken  = 0;
    };

    // Hand over the account's licenses, parsed from CMsgClientLicenseList.
    // Stored rather than acted on immediately — resolving them needs the
    // GetPackageInfo `this` pointer, which is only captured once Steam itself
    // calls that function, and that has usually not happened yet at logon.
    void OnLicenseList(std::vector<License> licenses);

    // Resolve any stored licenses to their app/depot lists and log them.
    // Safe to call repeatedly and from anywhere: it no-ops unless licenses are
    // pending and the GetPackageInfo capture is ready. Driven from
    // CheckAppOwnership, which runs often enough to guarantee both eventually.
    void TryDumpOwnedDepots();

    // Every depot id reachable through the account's licenses.
    //
    // `ready` distinguishes "not resolved yet" from "owns nothing" — before the
    // license list arrives the set is legitimately empty, and a caller that
    // cannot tell the difference would conclude the user owns no depots.
    struct OwnedDepots {
        bool                          ready = false;
        std::vector<uint32_t>         depots;
    };
    OwnedDepots GetOwnedDepots();
}
