#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// Pure part of the AppIdVec order diagnostics (Hooks_Package.cpp). Observation only.
//
// The injection appends the lua ids to the AppIdVec of Steam's package 0 in whatever order
// LuaConfig::GetAllDepotIds returns them (an unordered_map walk), and a later reconcile can
// FastRemove (swap with the last element) or append again. If Steam looks ids up in that vector
// with a binary search, an unsorted vector would make a present id come back "not found"
// depending on where it sits. These helpers only measure order; they never reorder anything.
namespace AppIdVecOrderLogic {

    struct OrderReport {
        size_t   size       = 0;
        size_t   inversions = 0;   // adjacent pairs with v[i] > v[i+1]
        size_t   firstAt    = 0;   // index i of the first such pair (valid when inversions > 0)
        uint32_t firstPrev  = 0;   // v[firstAt]
        uint32_t firstNext  = 0;   // v[firstAt + 1]
        [[nodiscard]] bool Sorted() const { return inversions == 0; }
    };

    // Ascending order with duplicates allowed, the contract of std::is_sorted. n may be 0.
    [[nodiscard]] inline OrderReport AnalyzeOrder(const uint32_t* v, size_t n) {
        OrderReport r;
        r.size = n;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (v[i] > v[i + 1]) {
                if (r.inversions == 0) { r.firstAt = i; r.firstPrev = v[i]; r.firstNext = v[i + 1]; }
                ++r.inversions;
            }
        }
        return r;
    }

    // Linear position of `id`, as the hook's own ContainsAppId finds it (first match).
    [[nodiscard]] inline std::optional<size_t> PositionOf(const uint32_t* v, size_t n, uint32_t id) {
        for (size_t i = 0; i < n; ++i)
            if (v[i] == id) return i;
        return std::nullopt;
    }

}  // namespace AppIdVecOrderLogic
