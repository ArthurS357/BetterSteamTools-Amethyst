#pragma once

#include "LocalAchievementsLogic.h"
#include "Steam/Types.h"

// Reads the Steam client's own local achievement state for a game off disk:
//   <steam>/appcache/stats/UserGameStatsSchema_<app>.bin   (names <-> stat/bit)
//   <steam>/appcache/stats/UserGameStats_<acct>_<app>.bin   (per-bit unlocks)
// and joins them into api_name -> unlock state. Used to rebuild the new
// Player.GetUserAchievements#1 response (which Valve returns empty for unowned
// games), keyed later by internal_key via the GetGameAchievements schema.
// The parsing itself lives in LocalAchievementsLogic (pure, unit-tested).
namespace LocalAchievements {

    // Empty on any read/parse failure (missing files, oversized files, etc).
    UnlockMap Read(AppId_t appId, AccountID_t accountId);

} // namespace LocalAchievements
