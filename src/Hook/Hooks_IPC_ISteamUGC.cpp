// Hooks_IPC_ISteamUGC - Workshop (UGC) support for Lua-managed apps (v1.4.0).
//
//   * OverrideAPICallResult(): ACTIVE IN ALL BUILDS. Rewrites access/license EResults of
//     Workshop / RemoteStorage API-call results to k_EResultOK (decision logic and the
//     byte patch live in Utils/SteamMetadata/UgcLogic, which is unit-tested).
//   * Everything below the #ifdef is DIAGNOSTIC ONLY: observes IClientUGC /
//     IClientRemoteStorage traffic and logs it. It never changes a request or a
//     response, and is compiled out of Release builds together with the LOG_IPC_*
//     macros.
//       - Register(): typed post-handlers. Only active when the IPC spec TOML for this
//         steamclient build lists the IClientUGC methods (see "[Handler Disabled]" in ipc.log).
//       - TraceCall(): raw dump keyed by interface id, independent of the spec.
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUGC.h"
#include "Hooks_Misc.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/UgcLogic.h"

// UgcLogic is Steam-header-free so it can build on the Linux CI; keep its EResult
// copies honest against the real enum.
static_assert(UgcLogic::kEResultOK == k_EResultOK);
static_assert(UgcLogic::kEResultFail == k_EResultFail);
static_assert(UgcLogic::kEResultInvalidParam == k_EResultInvalidParam);
static_assert(UgcLogic::kEResultInvalidState == k_EResultInvalidState);
static_assert(UgcLogic::kEResultAccessDenied == k_EResultAccessDenied);
static_assert(UgcLogic::kEResultInsufficientPrivilege == k_EResultInsufficientPrivilege);

namespace Hooks_IPC_ISteamUGC {

    void OverrideAPICallResult(CUtlBuffer* pWrite, uint32 iCallbackExpected, uint32 cubCallback) {
        // Cheap early-out before touching the response.
        if (!UgcLogic::IsEResultLeadingCallback(iCallbackExpected)) return;

        IPCMessages::IClientUtils::GetAPICallResultResp resp{pWrite, cubCallback};
        // Response shorter than declared, call not completed, or the call itself failed:
        // the callback bytes mean nothing -> leave the response alone.
        if (!resp.ok() || !resp.returnValue() || resp.pbFailed()) return;

        // Pipe-bound AppID, deliberately NOT ResolveAppId(): while -onlinefix is active that
        // returns the global real AppID for EVERY pipe, which would make an unrelated pipe
        // (a licensed game, the Steam UI) look Lua-managed and mask its real Workshop errors.
        // 0 (not a game pipe / engine not captured yet) => nothing is touched.
        const AppId_t appId = Hooks_Misc::GetAppIDForCurrentPipeWrap();
        const auto original = UgcLogic::TryOverrideCallback(appId, LuaConfig::HasDepot(appId),
                                                            iCallbackExpected, resp.pCallback());
        if (original) {
            LOG_IPC_DEBUG("UGC override: AppId={} callback={} result {} -> OK", appId, iCallbackExpected, *original);
        }
    }

} // namespace Hooks_IPC_ISteamUGC

#ifdef OPENSTEAMTOOL_LOGGING_ENABLED

#include "Utils/SteamMetadata/IPCLoader.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {
    using namespace IPCMessages::IClientUGC;

    constexpr size_t kMaxDumpBytes = 128;

    std::string Hex(std::span<const uint8> bytes) {
        std::ostringstream os;
        IPCMessages::detail::AppendBytes(os, "", bytes.first((std::min)(bytes.size(), kMaxDumpBytes)));
        return os.str();
    }

    // Workshop-related callback id ranges (steam_api callback families):
    //   k_iSteamRemoteStorageCallbacks = 1300, k_iSteamUGCCallbacks = 3400.
    constexpr bool IsWorkshopCallback(uint32 id) {
        return (id >= 1300 && id < 1400) || (id >= 3400 && id < 3500);
    }

    template <class Req, class Resp>
    void LogCall(const char* method, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        const Req req{pRead};
        const Resp resp{pWrite};
        LOG_IPC_TRACE("IClientUGC::{} AppId={} req={} resp={}", method, Hooks_Misc::ResolveAppId(),
                      req.ok() ? req.DebugString() : std::string("<layout mismatch>"),
                      resp.ok() ? resp.DebugString() : std::string("<layout mismatch>"));
    }

    // SubscribeItem/UnsubscribeItem are async: resp.returnValue is the SteamAPICall_t.
    // The EResult arrives later via IClientUtils::GetAPICallResult (see TraceCall).
    void HandlerPost_IClientUGC_SubscribeItem(CPipeClient*, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        LogCall<SubscribeItemReq, SubscribeItemResp>("SubscribeItem", pRead, pWrite);
    }
    void HandlerPost_IClientUGC_UnsubscribeItem(CPipeClient*, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        LogCall<UnsubscribeItemReq, UnsubscribeItemResp>("UnsubscribeItem", pRead, pWrite);
    }
    void HandlerPost_IClientUGC_GetItemState(CPipeClient*, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        LogCall<GetItemStateReq, GetItemStateResp>("GetItemState", pRead, pWrite);
    }
    void HandlerPost_IClientUGC_GetItemDownloadInfo(CPipeClient*, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        LogCall<GetItemDownloadInfoReq, GetItemDownloadInfoResp>("GetItemDownloadInfo", pRead, pWrite);
    }
    void HandlerPost_IClientUGC_GetItemInstallInfo(CPipeClient*, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        const GetItemInstallInfoReq req{pRead};
        if (!req.ok()) {
            LOG_IPC_TRACE("IClientUGC::GetItemInstallInfo AppId={} req=<layout mismatch>", Hooks_Misc::ResolveAppId());
            return;
        }
        const GetItemInstallInfoResp resp{pWrite, req.cchFolderSize()};
        LOG_IPC_TRACE("IClientUGC::GetItemInstallInfo AppId={} req={} resp={}", Hooks_Misc::ResolveAppId(),
                      req.DebugString(),
                      resp.ok() ? resp.DebugString() : std::string("<layout mismatch>"));
    }

    // GetAPICallResult for a workshop callback: this is where the EResult of
    // SubscribeItem & co. shows up. m_eResult is the first int32 of these callbacks.
    void TraceAPICallResult(const IPCMessages::IPCInterfaceCall& call, CUtlBuffer* pRead, CUtlBuffer* pWrite) {
        using namespace IPCMessages::IClientUtils;
        const auto* method = IPCLoader::Find("IClientUtils", "GetAPICallResult");
        if (!method || call.funcHash() != method->funcHash) return;

        const GetAPICallResultReq req{pRead};
        if (!req.ok() || !IsWorkshopCallback(req.iCallbackExpected())) return;
        const AppId_t appId = Hooks_Misc::ResolveAppId();
        if (!LuaConfig::HasDepot(appId)) return;

        const GetAPICallResultResp resp{pWrite, req.cubCallback()};
        if (!resp.ok()) {
            LOG_IPC_TRACE("IClientUtils::GetAPICallResult[workshop] AppId={} {} resp=<short/failed>",
                          appId, req.DebugString());
            return;
        }
        const auto callback = resp.pCallback();
        int32 firstInt = 0;
        if (callback.size() >= sizeof(firstInt)) std::memcpy(&firstInt, callback.data(), sizeof(firstInt));
        LOG_IPC_TRACE("IClientUtils::GetAPICallResult[workshop] AppId={} {} returnValue={} pbFailed={} callback.firstInt32(EResult?)={} bytes={}",
                      appId, req.DebugString(), resp.returnValue(), resp.pbFailed(), firstInt, Hex(callback));
    }

} // namespace

namespace Hooks_IPC_ISteamUGC {

    void Register() {
        IPCHandlerEntry UGCEntries[] = {
            ADD_IPC_POST_HANDLER(IClientUGC, SubscribeItem),
            ADD_IPC_POST_HANDLER(IClientUGC, UnsubscribeItem),
            ADD_IPC_POST_HANDLER(IClientUGC, GetItemState),
            ADD_IPC_POST_HANDLER(IClientUGC, GetItemDownloadInfo),
            ADD_IPC_POST_HANDLER(IClientUGC, GetItemInstallInfo),
        };
        Hooks_IPC::RegisterHandlers(UGCEntries);
    }

    void TraceCall(CUtlBuffer* pRead, CUtlBuffer* pWrite, bool processed) {
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok() || request.command() != EIPCCommand::InterfaceCall) return;
        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return;

        const EIPCInterface iface = call.interfaceID();
        if (iface == EIPCInterface::IClientUtils) {
            TraceAPICallResult(call, pRead, pWrite);
            return;
        }
        if (iface != EIPCInterface::IClientUGC && iface != EIPCInterface::IClientRemoteStorage) return;

        const AppId_t appId = Hooks_Misc::ResolveAppId();
        if (!LuaConfig::HasDepot(appId)) return;

        const IPCMessages::IPCResponse response{pWrite};
        const std::span<const uint8> reqBody = call.body();
        const std::span<const uint8> respBody = response.ok() ? response.body() : std::span<const uint8>{};
        LOG_IPC_TRACE("RAW {} funcHash=0x{:08X} AppId={} processed={} ipcResult={} reqBody={}B respBody={}B req={} resp={}",
                      EIPCInterfaceName(iface), call.funcHash(), appId, processed,
                      response.ok() ? EIPCResultName(response.result()) : "n/a",
                      reqBody.size(), respBody.size(), Hex(reqBody), Hex(respBody));
    }

} // namespace Hooks_IPC_ISteamUGC

#else // Release: diagnostics compiled out, no behavior change.

namespace Hooks_IPC_ISteamUGC {
    void Register() {}
    void TraceCall(CUtlBuffer*, CUtlBuffer*, bool) {}
}

#endif
