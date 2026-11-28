#pragma once

#include <cstddef>
#include <cstdint>
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
//
// Restore (v1.6.0) never dereferences a pointer kept from Raise: it looks each id up again
// and clears the flag only when Steam still hands back the very entry that was raised. An
// entry Steam freed or replaced during the pass is not ours any more and is left alone.
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
        bool Raise(std::uint32_t id, Data* data, Injected injected) {
            if (!data || data->bSkipFlag || !data->HasEmptyAppInfoSha() || !injected()) return false;
            raised_.push_back({ id, data });   // before the flag: a bad_alloc must not leave it raised
            data->bSkipFlag = true;
            return true;
        }

        [[nodiscard]] std::size_t Count() const noexcept { return raised_.size(); }

        // `lookup(id)` returns Steam's current entry for id (nullptr when it has none) and
        // must not create one. Puts back only what Raise changed; a flag Steam itself set
        // stays set. Returns how many flags were put back.
        template <class Lookup>
        std::size_t Restore(Lookup lookup) noexcept {
            std::size_t restored = 0;
            for (const Mark& m : raised_) {
                Data* now = lookup(m.id);
                if (now && now == m.data) {   // compared, never dereferenced before this check
                    now->bSkipFlag = false;
                    ++restored;
                }
            }
            raised_.clear();
            return restored;
        }

    private:
        struct Mark {
            std::uint32_t id;
            Data*         data;   // identity only
        };
        std::vector<Mark> raised_;
    };

} // namespace GateSkipLogic
