#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

// Pure join of Steam's local achievement schema and user-stats blobs (both
// binary KeyValues). No IO, no logging, no Steam globals -- split out of
// LocalAchievements.cpp so it compiles into the unit-test binary on its own
// (see src/tests/CMakeLists.txt), same pattern as TicketParsing.cpp.
namespace LocalAchievements {

    struct Unlock {
        bool     unlocked = false;
        uint32_t unlockTime = 0;   // unix seconds, 0 if none recorded
        float    progress = 0.0f;  // current value of the schema's progress stat
    };

    // api_name (schema "name" == GetGameAchievements internal_name) -> unlock.
    using UnlockMap = std::unordered_map<std::string, Unlock>;

    // schema = UserGameStatsSchema_<app>.bin bytes, stats = UserGameStats_<acct>_<app>.bin bytes.
    // Fails closed: an empty map when the schema is missing, malformed or lists
    // no achievements. A missing or malformed stats blob yields every schema
    // achievement locked (a valid, complete list).
    UnlockMap Join(std::span<const uint8_t> schema, std::span<const uint8_t> stats);

} // namespace LocalAchievements
