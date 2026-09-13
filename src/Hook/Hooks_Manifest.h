#pragma once

#include "dllmain.h"

#include <cstdint>

namespace Hooks_Manifest {
    void Install();
    void Uninstall();

    // Look up the app id and manifest GID last seen for a depot.
    //
    // Populated from BuildDepotDependency, which is the only place a depot's
    // current GID passes through — Steam supplies it while working out what to
    // install, so a depot only appears here once its app has been installed or
    // updated in this session. The GID recorded is Steam's own, captured before
    // any manifest-override patch is applied.
    //
    // Returns false if the depot has not been seen.
    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid);
}
