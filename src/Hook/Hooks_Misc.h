#pragma once

#include "dllmain.h"

#include <optional>
#include <string>

// Catch-all for lightweight info-capture int3 traps and launch-time rewrites
// that don't fit a dedicated category — currently:
//   * GetAppIDForCurrentPipe  -> captures the SteamEngine pointer
//   * SpawnProcess            -> OnlineFix detection + 480 rewrite
//   * GetAppDataFromAppInfo   -> captures the CAppInfoCache pointer
namespace Hooks_Misc {
    void Install();
    void Uninstall();

    // Returns the AppId for the current Steam pipe via the captured engine
    // pointer, or 0 if we haven't yet observed the host calling
    // GetAppIDForCurrentPipe.
    AppId_t GetAppIDForCurrentPipeWrap();

    // True while a -onlinefix game is the active spawn.
    bool IsOnlineFixActive();

    // Call when the game uses SteamNetworkingSockets (IPC interface 46).
    void NotifyNetworkingSocketsUsed();

    // True once P2P started — GetAppID reports 480; before, the real appid.
    bool ShouldReportOnlineFixAppId();

    // Grow a CUtlBuffer to at least 'newCapacity' bytes and set m_Put = newCapacity.
    // Uses CUtlBuffer::EnsureCapacity from steamclient, resolved on first call.
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut = false);

    // Resolve the real appid: if OnlineFix is active return real appid,
    // otherwise fall back to GetAppIDForCurrentPipe().
    AppId_t ResolveAppId();

    // Get localized game name via GetAppDataFromAppInfo (cached). May call into
    // Steam's AppInfo cache on a miss, so do NOT call it while holding one of
    // our own locks -- use TryGetCachedGameName there instead.
    std::string GetGameNameByAppID(AppId_t appId);

    // Cache-only lookup: never calls into Steam. nullopt if the name was never
    // resolved (an empty string means "resolved, but Steam had no name").
    std::optional<std::string> TryGetCachedGameName(AppId_t appId);

}
