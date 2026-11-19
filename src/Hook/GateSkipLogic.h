#pragma once

#include <cstddef>
#include <vector>

// Steam's ProcessPendingLicenseUpdates (the pass that rebuilds the ownership index) stops
// at the first app of a licensed package whose CAppData has an empty SHA and skip_flag == 0:
// it waits for that app's appinfo. Ids in the injected package that are really depot ids
// never get appinfo, so unless Steam's PICS path flagged them as "known unknown" the pass
// returns early on every run and the index is never rebuilt.
//
// The flag is raised only while that pass runs and put back right after it, entry by entry:
//   - raised for good, a Lua game added a moment ago (appinfo not fetched yet) would read as
//     "known unknown" and might never be requested;
//   - raised once and left, MarkLicenseAsChanged clears it again for every app of the package.
namespace GateSkipLogic {

    // `Data` is Steam's CAppData: bSkipFlag, HasEmptyAppInfoSha().
    template <class Data>
    class Marks {
    public:
        // Cheapest checks first: this runs for every app lookup made inside the pass.
        // `injected` (a config lookup) is only reached by unresolved entries.
        // Room for the ~250 injected depot ids up front: Raise runs inside Steam's
        // cache lock and must not allocate in the common case.
        Marks() { raised_.reserve(512); }

        template <class Injected>
        bool Raise(Data* data, Injected injected) {
            if (!data || data->bSkipFlag || !data->HasEmptyAppInfoSha() || !injected()) return false;
            raised_.push_back(data);   // before the flag: a bad_alloc must not leave it raised
            data->bSkipFlag = true;
            return true;
        }

        [[nodiscard]] std::size_t Count() const noexcept { return raised_.size(); }

        // Puts back only what Raise changed; a flag Steam itself set stays set.
        void Restore() noexcept {
            for (Data* d : raised_) d->bSkipFlag = false;
            raised_.clear();
        }

    private:
        std::vector<Data*> raised_;
    };

} // namespace GateSkipLogic
