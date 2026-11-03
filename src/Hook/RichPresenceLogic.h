#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Pure decision logic behind Hooks_NetPacket_RichPresence (Hooks_NetPacket.cpp),
// split out so it compiles in the unit-test binary -- on Windows and on the
// Linux CI -- without protobuf, HookMacros, LuaConfig or CloudRedirectHost.
// Same split as OnlineFixLogic / NetPacketLogic.
//
// Only DECISIONS (plus the pure KV1 parser) live here. g_RPMutex, the cached
// self-PersonaState template, BuildInject and the invokeOriginal re-delivery stay
// in the hook, which feeds these functions pre-computed facts. Nothing here
// reads a global.
//
// Plain uint32_t stands in for AppId_t (a uint32 typedef in Steam/Types.h).
namespace RichPresenceLogic {

    // EClientPersonaStateFlag::k_EClientPersonaStateFlagRichPresence. While this
    // bit is set the UI rebuilds m_mapRichPresence from rich_presence().
    inline constexpr std::uint32_t kStatusFlagRichPresence = 0x1000;

    using KvList = std::vector<std::pair<std::string, std::string>>;

    // CMsgClientGamesPlayed.game_id is a 64-bit GameID; the AppID is its low 32
    // bits.
    [[nodiscard]] std::uint32_t AppIdFromGameId(std::uint64_t gameId) noexcept;

    // May this topmost AppID drive the inject (g_PlayingAppId)? Only an unlocked
    // (lua) game: owned games on top let the server's natural broadcast paint the
    // cache, and -onlinefix games (the fake appid 480) are handled by the
    // OnlineFix path. `isOnlineFix` is "appId is the online-fix fake appid" --
    // the caller owns that constant (kOnlineFixAppId in dllmain.h).
    [[nodiscard]] bool ShouldTrackAppId(std::uint32_t appId, bool isLuaGame,
                                        bool isOnlineFix) noexcept;

    // What TrackSend does when a CMsgClientGamesPlayed arrives.
    enum class TrackAction : std::uint8_t {
        Unchanged,      // same tracked appid as before: nothing to inject
        InjectGame,     // a new unlocked game is on top: inject its rich presence
        InjectClear,    // the games stack went empty: inject a clear so the cache reverts
        DeferToServer,  // an owned / -onlinefix game is on top: let the server paint it
    };

    // playingAppId = what g_PlayingAppId holds now, newTracked = what
    // ShouldTrackAppId allows for this message (0 = none), topmost = the raw top
    // of the stack (0 = stack empty). DeferToServer vs InjectClear is the
    // difference between "stack empty" and "owned game on top": clearing in the
    // latter case would flicker the banner to "Online" between our drop and the
    // server's push.
    [[nodiscard]] TrackAction DecideTrackAction(std::uint32_t playingAppId,
                                                std::uint32_t newTracked,
                                                std::uint32_t topmost) noexcept;

    // Outbound CMsgClientRichPresenceUpload is decoded only once the local
    // SteamID is known and a game is being tracked (the KVs are stored per
    // tracked AppID).
    [[nodiscard]] bool ShouldCaptureRpUpload(std::uint64_t localSteamId,
                                             std::uint32_t playingAppId) noexcept;

    // An inbound self-PersonaState is patched in place with the tracked game
    // only while a game is tracked. (The tracked appid already implies an
    // unlocked game -- ShouldTrackAppId gated it -- so this deliberately does not
    // re-check the lua depot list.)
    [[nodiscard]] bool ShouldInjectRichPresence(std::uint32_t playingAppId) noexcept;

    // status_flags with the RichPresence bit set or cleared:
    //   appId == 0           -> set    (stopped: make the UI rebuild/clear its map)
    //   appId != 0, hasKvs   -> set    (the injected KVs must be picked up)
    //   appId != 0, !hasKvs  -> cleared (a fresh launch's first inject must not
    //                                    wipe the UI's m_mapRichPresence)
    // Every other bit is preserved.
    [[nodiscard]] std::uint32_t WithRichPresenceFlag(std::uint32_t statusFlags,
                                                     std::uint32_t appId,
                                                     bool hasKvs) noexcept;

    // Walk Steam's binary KV1 stream (a top-level "RP" struct around string KVs)
    // and append every string KV at any depth to `out`. Type 0x00 starts a
    // struct, 0x01 a string KV, 0x08 ends a struct; strings are NUL-terminated.
    // A well-formed stream ends `... 08 08`: the first 0x08 closes the "RP"
    // struct, the second (an end marker with no struct left to close) terminates
    // the whole KeyValues. The walk stops there, at an unknown type (e.g. an
    // int32 KV), or at truncated input -- whatever was collected before that is
    // kept. A single 0x08 at depth 0 is therefore already a terminator, while a
    // lone closing 0x08 of the top-level struct is NOT: later siblings are read.
    void ExtractStringKVs(const std::uint8_t* data, std::uint32_t size, KvList& out);

} // namespace RichPresenceLogic
