#pragma once

#include <string_view>

// Pure decision logic behind the online-fix AppID flip (see Hooks_Misc.cpp).
// No HookMacros/Detour/PatternLoader dependency, so this pair compiles
// directly into the test binary (see src/tests/CMakeLists.txt) instead of
// linking the hook engine.
namespace OnlineFixLogic {

    // True if `cmdLine` carries the "-realappid" launch option. Same substring
    // match as the pre-existing "-onlinefix" detection in OnSpawnProcessHit --
    // deliberately not word-boundary-aware (e.g. "-realappidx" also matches).
    // Ported as-is from upstream BetterSteamTools (issue #146 opt-out).
    [[nodiscard]] bool ShouldSuppressAppIdFlip(std::string_view cmdLine) noexcept;

    // The AppID-flip decision, factored out of the 3 globals Hooks_Misc.cpp
    // reads so it is testable as a pure function of state:
    //   suppressFlip     - this launch opted out via -realappid
    //   hasRealAppId     - an online-fix game is the active spawn
    //   networkingActive - the game started SteamNetworkingSockets P2P
    //
    // The flip exists so a P2P socket's appid matches the 480 session cert,
    // which some titles need (#146). It is blunt though: from the moment it
    // trips, every GetAppID answer is the fake appid for the rest of the
    // process's life. Games that ask Steam for their own appid during later
    // startup then get 480 and misbehave -- Bodycam (2406770) black-screens
    // straight after login this way. Both behaviours are needed by different
    // games, and the call itself gives no way to tell them apart, so
    // -realappid opts out per launch.
    [[nodiscard]] bool ShouldReportOnlineFixAppId(bool suppressFlip, bool hasRealAppId,
                                                   bool networkingActive) noexcept;

} // namespace OnlineFixLogic
