#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <string_view>

// Pure decision logic behind ManifestDonor (see ManifestDonor.h/.cpp),
// extracted so each network-gating condition is unit-testable without
// OSTPlatform::Http, OSTPlatform::Thread, or a live Steam session.
namespace ManifestDonorLogic {

    // Whether ManifestDonor::Start() should actually spawn the worker
    // thread. Trivially mirrors Config::DonateSettings::enabled -- kept as
    // its own named predicate (rather than inlining cfg.enabled at the call
    // site) so the ONE condition gating all donor network activity
    // (wanted-list GET, HEAD probes, mint calls, submit POST) has a single
    // name a test can pin down, instead of being an implicit consequence of
    // reading a struct field correctly.
    [[nodiscard]] constexpr bool ShouldStartDonor(bool donateEnabled) noexcept {
        return donateEnabled;
    }

    // Whether Hooks_NetPacket_Manifest::HandleSend should register this
    // outgoing GetManifestRequestCode in the passive-capture map. Both
    // conditions are required: a jobid to correlate the eventual reply
    // (haveJob), and [donate] actually enabled -- a request tracked while
    // donation is off would never be submitted anyway (SubmitCapturedCode
    // re-checks enabled independently), so tracking it would only be a
    // pointless map entry.
    [[nodiscard]] constexpr bool ShouldTrackSentRequest(bool haveJob, bool donateEnabled) noexcept {
        return haveJob && donateEnabled;
    }

    // Whether ManifestDonor::SubmitCapturedCode should proceed at all.
    // Every identifier being non-zero is required -- a zero anywhere means
    // "nothing genuine was captured" (see ManifestDonor.h) -- and [donate]
    // must still be enabled at submit time: it may have been toggled off in
    // the interval between the outgoing request (HandleSend, when tracking
    // was gated on the same setting) and this reply (HandleRecv).
    [[nodiscard]] constexpr bool ShouldSubmitCapturedCode(
        uint32_t depotId, uint64_t gid, uint64_t code, bool donateEnabled) noexcept {
        return depotId != 0 && gid != 0 && code != 0 && donateEnabled;
    }

    // One "app_id:depot_id:gid" line from the /manifestwanted response.
    struct WantedEntry {
        AppId_t  appId   = 0;   // may legitimately be 0 -- see upstream API.md
        uint32_t depotId = 0;
        uint64_t gid     = 0;
    };

    // Parses one line. Returns false on anything unexpected (missing field,
    // non-numeric, zero depot/gid) so a single malformed row can never
    // silently become a request for depot 0.
    [[nodiscard]] bool ParseWantedLine(std::string_view line, WantedEntry& out);

}
