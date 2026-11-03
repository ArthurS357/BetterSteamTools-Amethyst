#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// Pure decision logic behind the "groups" achievement path in
// Hooks_NetPacket.cpp (Player.GetUserAchievements#1 / GetGameAchievements#1),
// split out so it compiles in the unit-test binary -- on Windows and on the
// Linux CI -- without protobuf, HookMacros, LuaConfig or the credential store.
// Same split as OnlineFixLogic / NetPacketLogic.
//
// Only DECISIONS live here. The wire parsing, the g_AchMutex / g_JobMutex
// bookkeeping, FillUserAchievements and the invokeOriginal re-delivery stay in
// the hook, which feeds these functions pre-computed facts (an account id, "is
// a lua game", "is the schema cached", ...). Nothing here reads a global.
//
// Plain uint32_t stands in for AccountID_t / AppId_t (both are uint32 typedefs
// in Steam/Types.h) so the header needs nothing beyond <cstdint>.
namespace AchievementsLogic {

    // A GetUserAchievements reply that beats its app's schema is held (see
    // Hooks_NetPacket_UserStats::g_HeldUserAch) and released as-is if the schema
    // never shows, well before the client's own job would time out.
    inline constexpr std::chrono::seconds kHoldTimeout{ 8 };

    // Bound on what we keep alive: past this a reply is left untouched, not held.
    inline constexpr std::size_t kMaxHeldUserAch = 16;

    // The account a GetUserAchievements request is about. A request without a
    // steamid is the client asking about itself (-> activeAccount); otherwise it
    // is the low 32 bits of the 64-bit SteamID (the AccountID).
    [[nodiscard]] std::uint32_t RequestedAccount(bool requestHasSteamId,
                                                 std::uint64_t requestSteamId,
                                                 std::uint32_t activeAccount) noexcept;

    // True when Valve's reply must pass through untouched: the local stats .bin
    // only describes the logged-in user, so anyone else's page (a friend's) is
    // not reconstructed, and with no logged-in user (activeAccount == 0) there
    // is nobody to reconstruct for. 0 is never a valid match, so a request for
    // account 0 cannot pair with an unresolved active account.
    [[nodiscard]] bool ShouldSkipReconstruction(std::uint32_t requestAccount,
                                                std::uint32_t activeAccount) noexcept;

    // Recv side of the same decision: `jobAccount` is what the send hook stored
    // (the logged-in account when the request was for it, else 0). The reply is
    // rebuilt only for a lua game on the logged-in user's own page.
    [[nodiscard]] bool ShouldRebuildReply(std::uint32_t jobAccount, bool isLuaGame) noexcept;

    // A reply whose app has no cached schema yet is held (choke-and-queue) until
    // the schema lands -- unless kMaxHeldUserAch replies are already waiting, in
    // which case it is left alone. A reply whose schema IS cached is rebuilt
    // immediately, never held.
    [[nodiscard]] bool ShouldHoldReply(bool schemaCached, std::size_t heldCount) noexcept;

    // A held reply is released as-is once it has waited kHoldTimeout.
    [[nodiscard]] bool ShouldReleaseHeld(std::chrono::steady_clock::duration age) noexcept;

    // A GetGameAchievements reply with no achievements (hash_only) must not wipe
    // a schema we already cached for that app.
    [[nodiscard]] bool ShouldKeepCachedSchema(std::size_t newAchievementCount,
                                              bool haveCachedSchema) noexcept;

} // namespace AchievementsLogic
