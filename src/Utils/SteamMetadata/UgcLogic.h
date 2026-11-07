#pragma once

#include <cstdint>
#include <optional>
#include <span>

// Pure decision logic behind the Workshop (UGC) EResult override in
// Hooks_IPC_ISteamUGC.cpp, split out so it compiles in the unit-test binary --
// on Windows and on the Linux CI -- without the IPC layer, LuaConfig or Steam
// headers. Same split as OnlineFixLogic / RichPresenceLogic.
//
// Context: for a Lua-managed app (no real license) steamclient completes
// Workshop API calls (SubscribeItem, ...) with an access/license EResult. The
// game reads that EResult from the callback it fetches through
// IClientUtils::GetAPICallResult. The hook rewrites it to k_EResultOK there.
//
// Only DECISIONS plus the in-place byte patch live here; the hook feeds this
// pre-computed facts. Nothing here reads a global.
//
// Plain std::uint32_t / std::int32_t stand in for AppId_t / EResult. The values
// below are pinned to Steam/Enums.h by static_assert in Hooks_IPC_ISteamUGC.cpp.
namespace UgcLogic {

    inline constexpr std::int32_t kEResultOK                    = 1;
    inline constexpr std::int32_t kEResultFail                  = 2;
    inline constexpr std::int32_t kEResultInvalidParam          = 8;
    inline constexpr std::int32_t kEResultInvalidState          = 11;
    inline constexpr std::int32_t kEResultAccessDenied          = 15;
    inline constexpr std::int32_t kEResultInsufficientPrivilege = 24;

    // Should a failed Workshop EResult be reported as k_EResultOK?
    //
    // True only for a Lua-managed app (resolved, non-zero AppID) AND an
    // access/license-style failure: Fail, InvalidParam, InvalidState,
    // AccessDenied, InsufficientPrivilege. Everything else is left alone
    // (fail-closed): k_EResultOK (nothing to fix), k_EResultNone, network and
    // timeout errors, not-found, and any value we do not recognise -- those
    // are real failures the game should see.
    [[nodiscard]] bool ShouldOverrideUgcResult(std::uint32_t appId, std::int32_t originalResult,
                                               bool isLuaManaged) noexcept;

    // Does this callback id name a Workshop / RemoteStorage API-call result whose
    // FIRST int32 is m_eResult? Allowlist on purpose: SteamUGCQueryCompleted_t
    // (3401) starts with a uint64 query handle, DownloadItemResult_t (3406) with
    // AppId/PublishedFileId, the 3407-3409 vote/favorite results with a file id,
    // RemoteStorageFileReadAsyncComplete_t (1332) with a handle. Writing "1" over
    // the first int32 of those would corrupt a handle or id, not fix a result.
    [[nodiscard]] bool IsEResultLeadingCallback(std::uint32_t callbackId) noexcept;

    // Rewrites callback[0..3] to k_EResultOK when ShouldOverrideUgcResult says so.
    // Returns the ORIGINAL EResult if it rewrote, std::nullopt if it touched
    // nothing. Never writes unless the callback id is allowlisted and the buffer
    // holds at least 4 bytes (fail-closed: unreadable => unchanged).
    [[nodiscard]] std::optional<std::int32_t> TryOverrideCallback(std::uint32_t appId, bool isLuaManaged,
                                                                  std::uint32_t callbackId,
                                                                  std::span<std::uint8_t> callback) noexcept;

} // namespace UgcLogic
