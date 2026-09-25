#pragma once

#include <cstdint>

// Pure predicates behind the hooks in Hooks_Manifest.cpp, split out
// (header-only, constexpr) so the decisions are unit-testable without the hook
// engine, LuaConfig or a live Steam -- same pattern as ManifestProbeLogic.h.
namespace ManifestHookLogic {

    // BuildDepotDependency guard (ported from upstream 0b776c5). True when the
    // call must be failed instead of letting Steam store the depot list it just
    // produced: the app's OWN list is empty or absent while a lua declares the
    // app.
    //
    // Steam reports zero depots for an app while a PICS refresh swaps its
    // appinfo in -- the old set is dropped before the new one lands (upstream
    // observed nCount 1 -> 0 -> 1 ~9s apart for app 3293260; the fake license
    // adding thousands of apps widens that window). The caller stores whatever
    // comes back, so acting on the transient writes an EMPTY depot config and
    // the update "completes" having downloaded nothing ("0 mounted depots" --
    // seen locally for lua apps 3768760 and 391540). Failing the call makes
    // Steam keep its previous config and redo it once appinfo has landed.
    //
    // Only the app's own list counts: the shared list holds OTHER apps' depots
    // (sharedinstall/depotfromapp redirects), which say nothing about this
    // app's config -- testing it too is what let app 3751260 through upstream.
    //
    // Accepted residual risk (decision D2): "lua declares the app" is
    // DepotKeySet membership, which any addappid(id) satisfies, so a
    // lua-declared DLC that genuinely has no depots of its own is failed too.
    [[nodiscard]] constexpr bool ShouldRejectEmptyDepotList(
        uint32_t appId, bool hasOwnList, uint32_t ownDepotCount, bool luaDeclaresApp) noexcept
    {
        return appId != 0 && (!hasOwnList || ownDepotCount == 0) && luaDeclaresApp;
    }

} // namespace ManifestHookLogic
