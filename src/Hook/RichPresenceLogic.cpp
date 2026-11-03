#include "Hook/RichPresenceLogic.h"

namespace RichPresenceLogic {

    std::uint32_t AppIdFromGameId(std::uint64_t gameId) noexcept
    {
        return static_cast<std::uint32_t>(gameId & UINT32_MAX);
    }

    bool ShouldTrackAppId(std::uint32_t appId, bool isLuaGame, bool isOnlineFix) noexcept
    {
        return appId != 0 && !isOnlineFix && isLuaGame;
    }

    TrackAction DecideTrackAction(std::uint32_t playingAppId,
                                  std::uint32_t newTracked,
                                  std::uint32_t topmost) noexcept
    {
        if (playingAppId == newTracked) return TrackAction::Unchanged;
        if (newTracked != 0)            return TrackAction::InjectGame;
        if (topmost == 0)               return TrackAction::InjectClear;
        return TrackAction::DeferToServer;
    }

    bool ShouldCaptureRpUpload(std::uint64_t localSteamId, std::uint32_t playingAppId) noexcept
    {
        return localSteamId != 0 && playingAppId != 0;
    }

    bool ShouldInjectRichPresence(std::uint32_t playingAppId) noexcept
    {
        return playingAppId != 0;
    }

    std::uint32_t WithRichPresenceFlag(std::uint32_t statusFlags,
                                       std::uint32_t appId,
                                       bool hasKvs) noexcept
    {
        const bool wanted = appId == 0 || hasKvs;
        return wanted ? (statusFlags | kStatusFlagRichPresence)
                      : (statusFlags & ~kStatusFlagRichPresence);
    }

    // Moved verbatim from Hooks_NetPacket_RichPresence::ExtractStringKVs.
    void ExtractStringKVs(const std::uint8_t* data, std::uint32_t size, KvList& out)
    {
        std::uint32_t pos = 0;
        int depth = 0;
        auto readCStr = [&](std::string& s) -> bool {
            std::uint32_t start = pos;
            while (pos < size && data[pos] != 0) ++pos;
            if (pos >= size) return false;
            s.assign(reinterpret_cast<const char*>(data + start), pos - start);
            ++pos;
            return true;
        };
        while (pos < size) {
            std::uint8_t type = data[pos++];
            if (type == 0x08) {
                if (depth > 0) { --depth; continue; }
                break;
            }
            if (type == 0x00) {
                std::string name;
                if (!readCStr(name)) return;
                ++depth;
            } else if (type == 0x01) {
                std::string key, value;
                if (!readCStr(key) || !readCStr(value)) return;
                out.emplace_back(std::move(key), std::move(value));
            } else {
                return;
            }
        }
    }

} // namespace RichPresenceLogic
