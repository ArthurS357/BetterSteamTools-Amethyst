#pragma once
#include "Steam/Structs.h"

namespace Hooks_IPC_ISteamUGC {
    // Registers log-only post-handlers for IClientUGC (Debug builds only).
    void Register();

    // Debug-only raw trace of every IClientUGC / IClientRemoteStorage call for
    // Lua-managed apps: funcHash + request/response bytes. Needs no IPC spec entry,
    // so it works even when the upstream TOML has no IClientUGC methods.
    // Call after the original IPCProcessMessage; both buffers are read-only here.
    void TraceCall(CUtlBuffer* pRead, CUtlBuffer* pWrite, bool processed);
}
