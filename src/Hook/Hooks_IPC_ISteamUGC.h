#pragma once
#include "Steam/Structs.h"

namespace Hooks_IPC_ISteamUGC {
    // Workshop support for Lua-managed apps. ACTIVE IN ALL BUILDS (Release too).
    // Called from the IClientUtils::GetAPICallResult post-handler with the response
    // steamclient just wrote: if the callback is a Workshop/RemoteStorage result whose
    // m_eResult is an access/license failure, rewrites it to k_EResultOK in place.
    // Fail-closed: touches nothing unless the call completed, the callback id is
    // allowlisted and the callback buffer holds >= 4 bytes (see UgcLogic).
    void OverrideAPICallResult(CUtlBuffer* pWrite, uint32 iCallbackExpected, uint32 cubCallback);

    // Registers log-only post-handlers for IClientUGC (Debug builds only).
    void Register();

    // Debug-only raw trace of every IClientUGC / IClientRemoteStorage call for
    // Lua-managed apps: funcHash + request/response bytes. Needs no IPC spec entry,
    // so it works even when the upstream TOML has no IClientUGC methods.
    // Call after the original IPCProcessMessage; both buffers are read-only here.
    // It runs after the post-handlers, so a workshop GetAPICallResult is logged
    // AS REWRITTEN by OverrideAPICallResult (the original EResult is in the
    // "UGC override" debug line).
    void TraceCall(CUtlBuffer* pRead, CUtlBuffer* pWrite, bool processed);
}
