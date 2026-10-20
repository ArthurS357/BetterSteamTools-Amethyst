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

    // YldLoadDepotManifest pre-seed gate (ported from upstream dec29b7). True
    // when the hook should fetch the archived manifest before Steam's own
    // depotcache check.
    //
    // cacheConfigured ([cache] url set) comes first: without it the feature is
    // off and the hook must not touch the network -- ManifestCache::EnsureCached
    // enforces the same gate again, this just keeps the opt-in visible here.
    // A depot qualifies when a lua pins its gid (pinned wins even if owned, same
    // as the old PreseedDepots) or a lua declares it and the account owns
    // neither the app nor the depot -- owned content gets working request codes
    // from Steam and is left alone.
    [[nodiscard]] constexpr bool ShouldPreseedOnManifestLoad(
        bool cacheConfigured, uint32_t depotId, uint64_t manifestGid,
        bool pinned, bool luaDeclaresDepot, bool owned) noexcept
    {
        return cacheConfigured && depotId != 0 && manifestGid != 0 &&
               (pinned || (luaDeclaresDepot && !owned));
    }

} // namespace ManifestHookLogic
