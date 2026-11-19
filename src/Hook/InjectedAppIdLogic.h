#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

// Pure part of the AppIdVec edits in Hooks_Package.cpp: the injected package 0's AppIdVec has to
// stay in ascending order.
//
// Measured on a real session (2026-11-19, Debug trace): of 636 ids, Steam's own 196 were in
// perfect ascending order and the 440 lua ids appended after them (in unordered_map order) held
// 176 inversions; the old removal (CUtlVector::FindAndFastRemove) swaps the LAST element into the
// hole and breaks order by itself. Steam keeping its part sorted suggests it looks ids up with a
// binary search, which would miss ids that sit out of place. That last step is the hypothesis
// this change tests; it is not proven.
//
// All of it works on a raw (pointer, size) range because the real vector lives in Steam's memory
// and is edited in place, never replaced. Nothing here allocates the vector or moves its buffer.
namespace InjectedAppIdLogic {

    // The ids of `wanted` that `cur[0..n)` does not hold yet: unique, ascending. Appending only
    // these keeps duplicates out of the vector (an id Steam already lists is not added twice).
    [[nodiscard]] inline std::vector<uint32_t> MissingIds(const uint32_t* cur, size_t n,
                                                         const std::vector<uint32_t>& wanted) {
        const std::unordered_set<uint32_t> have(cur, cur + n);
        std::vector<uint32_t> out;
        out.reserve(wanted.size());
        for (const uint32_t id : wanted)
            if (have.find(id) == have.end()) out.push_back(id);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Ascending, in place. Equal ids stay (a duplicate is harmless to a binary search).
    inline void SortInPlace(uint32_t* p, size_t n) {
        std::sort(p, p + n);
    }

    // Removes the first `id` and shifts the tail down by one, so the relative order of everything
    // else is untouched (unlike a swap-with-last). Shrinks `n` only; the buffer is not touched
    // beyond the shift. False when `id` is absent.
    inline bool RemoveKeepingOrder(uint32_t* p, size_t& n, uint32_t id) {
        const uint32_t* const end = p + n;
        const uint32_t* const hit = std::find(static_cast<const uint32_t*>(p), end, id);
        if (hit == end) return false;
        const size_t i = static_cast<size_t>(hit - p);
        std::move(p + i + 1, p + n, p + i);
        --n;
        return true;
    }

}  // namespace InjectedAppIdLogic
