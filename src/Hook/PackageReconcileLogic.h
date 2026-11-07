#pragma once

#include "Steam/Types.h"

#include <unordered_set>
#include <vector>

// What to change in the injected package's AppIdVec after a batch of Lua reloads,
// split out of Hooks_Package.cpp so it can be tested without Steam structures.
//
// LuaConfig's pending queues record history, and replaying history gets it wrong:
// the two queues lose their relative order, and they can span several batches when
// reloads land before the initial fill. So they are used only to learn WHICH ids may
// have changed; what to do with each one comes from comparing the current config to
// the vector. An id reloaded unchanged ends up in neither list, so the vector (which
// Steam reads without any lock we can take) is not touched for it.
namespace PackageReconcileLogic {

    struct Plan {
        std::vector<AppId_t> additions;
        std::vector<AppId_t> removals;
    };

    // `wanted(id)`: the config currently provides id. `present(id)`: the vector holds it.
    template <class Wanted, class Present>
    [[nodiscard]] Plan Reconcile(const std::vector<AppId_t>& queuedRemovals,
                                 const std::vector<AppId_t>& queuedAdditions,
                                 Wanted wanted, Present present) {
        std::unordered_set<AppId_t> touched(queuedRemovals.begin(), queuedRemovals.end());
        touched.insert(queuedAdditions.begin(), queuedAdditions.end());

        Plan plan;
        for (AppId_t id : touched) {
            const bool want = wanted(id);
            if (want != present(id)) {
                (want ? plan.additions : plan.removals).push_back(id);
            }
        }
        return plan;
    }

} // namespace PackageReconcileLogic
