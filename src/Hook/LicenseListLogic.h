#pragma once

#include <cstdint>
#include <vector>

// Pure parsing logic behind CMsgClientLicenseList (eMsg 780), split out of
// Hooks_NetPacket.cpp so it compiles here without HookMacros/Detour/the hook
// engine (see src/tests/CMakeLists.txt) -- same split as OnlineFixLogic.
namespace LicenseListLogic {

    // One license entry Steam sends after logon. accessToken is required:
    // GetPackageInfo will not return a PackageInfo without it.
    struct LicenseEntry {
        uint32_t packageId   = 0;
        uint64_t accessToken = 0;
    };

    // Parses a CMsgClientLicenseList protobuf body into (package_id,
    // access_token) pairs. Entries missing package_id are skipped -- there is
    // nothing GetPackageInfo could resolve without one. accessToken defaults
    // to 0 when the field is absent.
    //
    // Returns false only when `body` does not parse as a CMsgClientLicenseList
    // at all (including a null body or a size too large to hand to protobuf);
    // `out` is left untouched in that case. A message that parses but yields
    // zero usable entries returns true with `out` cleared and empty -- callers
    // must not treat a parse failure and an empty owned-license list the same.
    [[nodiscard]] bool ParseOwnedLicenses(const uint8_t* body, uint32_t size,
                                          std::vector<LicenseEntry>& out);

} // namespace LicenseListLogic
