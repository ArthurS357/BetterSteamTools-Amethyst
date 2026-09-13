#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <future>

namespace Hooks_NetPacket {
    void Install();
    void Uninstall();

    // Originates a ContentServerDirectory.GetManifestRequestCode#1 call for
    // (appId, depotId, gid) instead of waiting for Steam to ask -- see
    // Hooks_NetPacket_ManifestProbe in Hooks_NetPacket.cpp. Only meaningful
    // once Steam has sent at least one binary frame this session (the probe
    // needs a live websocket + a real header template); a future holding 0
    // means the request failed to send, was refused by Steam, or timed out.
    //
    // Intended caller: ManifestDonor's mint cycle, which itself checks
    // Config::GetDonateSettings().enabled before ever reaching here -- this
    // function does not gate on [donate] itself.
    std::future<uint64_t> RequestManifestCode(AppId_t appId, uint32_t depotId, uint64_t gid);
}
