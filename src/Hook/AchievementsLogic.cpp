#include "Hook/AchievementsLogic.h"

namespace AchievementsLogic {

    std::uint32_t RequestedAccount(bool requestHasSteamId,
                                   std::uint64_t requestSteamId,
                                   std::uint32_t activeAccount) noexcept
    {
        return requestHasSteamId
            ? static_cast<std::uint32_t>(requestSteamId & 0xFFFFFFFFull)
            : activeAccount;
    }

    bool ShouldSkipReconstruction(std::uint32_t requestAccount,
                                  std::uint32_t activeAccount) noexcept
    {
        return activeAccount == 0 || requestAccount != activeAccount;
    }

    bool ShouldRebuildReply(std::uint32_t jobAccount, bool isLuaGame) noexcept
    {
        return jobAccount != 0 && isLuaGame;
    }

    bool ShouldHoldReply(bool schemaCached, std::size_t heldCount) noexcept
    {
        return !schemaCached && heldCount < kMaxHeldUserAch;
    }

    bool ShouldReleaseHeld(std::chrono::steady_clock::duration age) noexcept
    {
        return age >= kHoldTimeout;
    }

    bool ShouldKeepCachedSchema(std::size_t newAchievementCount,
                                bool haveCachedSchema) noexcept
    {
        return newAchievementCount == 0 && haveCachedSchema;
    }

} // namespace AchievementsLogic
