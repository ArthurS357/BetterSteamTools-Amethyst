#pragma once

#include <cstdint>

// Pure predicate behind Hooks_NetPacket_ManifestProbe (see Hooks_NetPacket.cpp)
// -- which jobid range this fork's originated ("probe") requests use, and how
// to tell a probe's own reply apart from a genuine Steam-initiated exchange.
// Extracted (header-only, constexpr) so the boundary is unit-testable without
// the hook engine, protobuf messages, or a live websocket -- everything else
// in ManifestProbe needs at least one of those.
namespace ManifestProbeLogic {

    // Well clear of Steam's own job ids, which count up from small values.
    inline constexpr uint64_t kJobIdBase = 0x7E51'0000'0000'0000ull;

    // True if `jobId` falls in the range Hooks_NetPacket_ManifestProbe::Request
    // hands out (g_NextJobId starts at kJobIdBase and only ever increments) --
    // a cheap, allocation-free reject before locking a mutex or looking the id
    // up in the pending-request map.
    [[nodiscard]] constexpr bool IsSyntheticJobId(uint64_t jobId) noexcept {
        return jobId >= kJobIdBase;
    }

}
