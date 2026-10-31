#pragma once
#include "dllmain.h"

namespace Hooks_Decryption {
    // LoadDepotDecryptionKey hook: serves user-provided decryption keys for
    // depots configured via Lua.
    void Install();
    void Uninstall();

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId);

    // AppIds of apptickets observed being read through the local user
    // ConfigStore (populated by the ConfigStoreGetBinary hook). The forge
    // fallback uses this to search for any available source ticket when the
    // primary (appid 7) and first fallback (appid 228980) tickets are missing.
    // Returned by value: the hook inserts from Steam's threads, so handing out
    // a reference to the live set would race with the caller's iteration.
    std::vector<AppId_t> GetSeenAppTicketIds();
}
