#include "UgcLogic.h"

#include <cstring>

namespace UgcLogic {

    bool ShouldOverrideUgcResult(std::uint32_t appId, std::int32_t originalResult, bool isLuaManaged) noexcept {
        if (!isLuaManaged || appId == 0) return false;   // original games and unresolved AppID pass through

        switch (originalResult) {
            case kEResultFail:
            case kEResultInvalidParam:
            case kEResultInvalidState:
            case kEResultAccessDenied:
            case kEResultInsufficientPrivilege:
                return true;
            default:   // kEResultOK, network/timeout/not-found, unknown: leave untouched
                return false;
        }
    }

    bool IsEResultLeadingCallback(std::uint32_t callbackId) noexcept {
        switch (callbackId) {
            // k_iSteamRemoteStorageCallbacks (1300) family: Subscribe/Unsubscribe/Publish/...
            case 1307: case 1309: case 1311: case 1312: case 1313: case 1314: case 1315:
            case 1316: case 1317: case 1318: case 1319: case 1320:
            case 1324: case 1325: case 1326: case 1327: case 1328: case 1331:
            // k_iSteamUGCCallbacks (3400) family: Create/SubmitItemUpdate, playtime, dependencies, delete
            case 3403: case 3404: case 3410: case 3411: case 3412: case 3413:
            case 3414: case 3415: case 3416: case 3417:
                return true;
            default:
                return false;
        }
    }

    std::optional<std::int32_t> TryOverrideCallback(std::uint32_t appId, bool isLuaManaged,
                                                    std::uint32_t callbackId,
                                                    std::span<std::uint8_t> callback) noexcept {
        if (!IsEResultLeadingCallback(callbackId)) return std::nullopt;
        if (callback.size() < sizeof(std::int32_t)) return std::nullopt;   // cannot read EResult safely

        std::int32_t original = 0;
        std::memcpy(&original, callback.data(), sizeof(original));
        if (!ShouldOverrideUgcResult(appId, original, isLuaManaged)) return std::nullopt;

        const std::int32_t ok = kEResultOK;
        std::memcpy(callback.data(), &ok, sizeof(ok));
        return original;
    }

} // namespace UgcLogic
