#include "LocalAchievements.h"

#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <fstream>
#include <vector>

namespace LocalAchievements {
namespace {

    // Schema/stats .bin files are a few hundred KB at most; refuse anything
    // absurd rather than slurp an attacker-sized file into memory.
    constexpr std::streamoff kMaxBlobBytes = 16 * 1024 * 1024;

    // SteamInstallPath is UTF-8; go through Utf8ToPath, not the ANSI-codepage
    // path(std::string) conversion ifstream(std::string) would use.
    std::vector<uint8_t> ReadBlob(const std::string& utf8Path) {
        std::ifstream f(OSTPlatform::Encoding::Utf8ToPath(utf8Path), std::ios::binary | std::ios::ate);
        if (!f) return {};
        const std::streamoff size = f.tellg();
        if (size <= 0 || size > kMaxBlobBytes) return {};
        std::vector<uint8_t> blob(static_cast<size_t>(size));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(blob.data()), size)) return {};
        return blob;
    }

} // namespace

UnlockMap Read(AppId_t appId, AccountID_t accountId) {
    if (!SteamInstallPath[0] || accountId == 0) return {};

    // appId / accountId are integers, so neither can smuggle a path separator.
    const std::string dir = std::string(SteamInstallPath) + "\\appcache\\stats\\";
    const auto schema = ReadBlob(dir + "UserGameStatsSchema_" + std::to_string(appId) + ".bin");
    if (schema.empty()) {
        LOG_ACHIEVEMENT_DEBUG("LocalAchievements: no schema .bin for app={}", appId);
        return {};
    }
    const auto stats = ReadBlob(dir + "UserGameStats_" + std::to_string(accountId) +
                                "_" + std::to_string(appId) + ".bin");

    UnlockMap result = Join(schema, stats);
    LOG_ACHIEVEMENT_INFO("LocalAchievements: app={} -> {} achievement(s) in schema, stats file {}",
                         appId, result.size(), stats.empty() ? "missing" : "read");
    return result;
}

} // namespace LocalAchievements
