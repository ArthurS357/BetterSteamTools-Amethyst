#pragma once

#include <cstddef>
#include <cstdint>

// CheckAppOwnership does not read the injected package's AppIdVec. It reads a
// separate appId -> license index in CUser that Steam rebuilds only from its own
// RunFrame, and only when CUser's "rebuild index" flag is set. MarkLicenseAsChanged
// never sets that flag; Steam's handler for its "appinfo update finished" callback
// does, whenever the update reports at least one app. Calling that handler with
// appsUpdated = 1 is the whole trigger: it stores the flags and returns, and the
// rebuild then runs on Steam's own thread. RE notes: steamclient64 sha 669fa08e,
// handler 0xA10BC0, flag word CUser+0x22A0 = 0x0101 (22A0 run, 22A1 rebuild).
namespace IndexRebuildLogic {

    // Payload of that callback as the handler reads it (rdx+0 / +4 / +8).
    struct AppInfoUpdateMsg {
        uint32_t result;
        uint32_t appsUpdated;
        uint32_t packagesUpdated;
    };
    static_assert(sizeof(AppInfoUpdateMsg) == 12);
    static_assert(offsetof(AppInfoUpdateMsg, appsUpdated) == 4);
    static_assert(offsetof(AppInfoUpdateMsg, packagesUpdated) == 8);

    // result = OK, one app updated: the branch that sets both flags.
    inline constexpr AppInfoUpdateMsg kRebuildMsg{1, 1, 0};

    // Steam's own license-change order: mark the package, then raise the flags.
    template <class Mark, class Trigger>
    bool MarkAndRequestRebuild(void* user, Mark mark, Trigger trigger) {
        if (!user) return false;
        mark(user);
        AppInfoUpdateMsg msg = kRebuildMsg;
        trigger(user, &msg);
        return true;
    }

} // namespace IndexRebuildLogic
