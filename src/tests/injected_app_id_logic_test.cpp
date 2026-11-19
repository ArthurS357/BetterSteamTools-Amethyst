// Unit tests for InjectedAppIdLogic (Hook/InjectedAppIdLogic.h): keeping the injected package 0's
// AppIdVec in ascending order through the initial injection and later removals/additions.

#include "Hook/AppIdVecOrderLogic.h"
#include "Hook/InjectedAppIdLogic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using AppIdVecOrderLogic::AnalyzeOrder;
using InjectedAppIdLogic::MissingIds;
using InjectedAppIdLogic::RemoveKeepingOrder;
using InjectedAppIdLogic::SortInPlace;

namespace {

    bool Sorted(const std::vector<uint32_t>& v) { return AnalyzeOrder(v.data(), v.size()).Sorted(); }

    // A plain binary search, not std::binary_search: the MSVC debug STL asserts on unordered input,
    // and the whole point of one test below is to run it over an unordered vector.
    bool ManualBinarySearch(const std::vector<uint32_t>& v, uint32_t id) {
        size_t lo = 0, hi = v.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (v[mid] == id) return true;
            if (v[mid] < id) lo = mid + 1; else hi = mid;
        }
        return false;
    }

    // The hook's sequence: append what is missing, then sort the whole range in place.
    void Inject(std::vector<uint32_t>& vec, const std::vector<uint32_t>& wanted) {
        const auto add = MissingIds(vec.data(), vec.size(), wanted);
        vec.insert(vec.end(), add.begin(), add.end());
        SortInPlace(vec.data(), vec.size());
    }

    // The measured shape: Steam's own ids ascending, lua ids arriving in container order.
    TEST(InjectedAppIdLogicTest, InjectionLeavesTheVectorAscending) {
        std::vector<uint32_t> vec = {7, 70, 700, 7000, 70000};
        Inject(vec, {391541, 5072431, 391540, 228989, 1868141, 391544, 3592041});

        EXPECT_TRUE(Sorted(vec));
        EXPECT_EQ(vec.size(), 12u);
        // Steam's own ids are all still there.
        for (uint32_t id : {7u, 70u, 700u, 7000u, 70000u})
            EXPECT_TRUE(std::binary_search(vec.begin(), vec.end(), id)) << id;
    }

    // What a binary search needs: every injected id is found.
    TEST(InjectedAppIdLogicTest, EveryInjectedIdIsFoundByBinarySearch) {
        std::vector<uint32_t> vec = {10, 20, 30};
        const std::vector<uint32_t> wanted = {391542, 5, 391541, 99999999, 391544, 25};
        Inject(vec, wanted);
        for (uint32_t id : wanted)
            EXPECT_TRUE(std::binary_search(vec.begin(), vec.end(), id)) << id;
    }

    // Before the change a binary search over the appended-in-container-order vector missed ids.
    TEST(InjectedAppIdLogicTest, TheOldAppendOrderDefeatsABinarySearch) {
        std::vector<uint32_t> vec = {10, 20, 30};
        const std::vector<uint32_t> wanted = {900, 100, 800, 200, 700, 300, 600, 400, 500};
        std::vector<uint32_t> old = vec;
        old.insert(old.end(), wanted.begin(), wanted.end());   // what the hook used to do

        ASSERT_FALSE(Sorted(old));
        size_t missed = 0;
        for (uint32_t id : wanted)
            if (!ManualBinarySearch(old, id)) ++missed;
        EXPECT_GT(missed, 0u);

        Inject(vec, wanted);
        for (uint32_t id : wanted)
            EXPECT_TRUE(ManualBinarySearch(vec, id)) << id;
    }

    // Duplicates: an id Steam already lists is not added again; a duplicate in `wanted` is added once.
    TEST(InjectedAppIdLogicTest, DuplicatesAreNotAppended) {
        std::vector<uint32_t> vec = {5, 50, 500};
        Inject(vec, {50, 60, 60, 500, 70});
        EXPECT_EQ(vec, (std::vector<uint32_t>{5, 50, 60, 70, 500}));
    }

    TEST(InjectedAppIdLogicTest, MissingIdsIsUniqueAndAscending) {
        const std::vector<uint32_t> cur = {1, 3, 5};
        EXPECT_EQ(MissingIds(cur.data(), cur.size(), {9, 3, 7, 7, 2, 1}),
                  (std::vector<uint32_t>{2, 7, 9}));
        EXPECT_TRUE(MissingIds(cur.data(), cur.size(), {}).empty());
        EXPECT_EQ(MissingIds(nullptr, 0, {4, 2}), (std::vector<uint32_t>{2, 4}));
    }

    // An already-sorted-with-duplicates vector (Steam's own part may hold a repeat) stays valid.
    TEST(InjectedAppIdLogicTest, ExistingDuplicatesInSteamsPartSurvive) {
        std::vector<uint32_t> vec = {1, 2, 2, 3};
        Inject(vec, {2, 4});
        EXPECT_EQ(vec, (std::vector<uint32_t>{1, 2, 2, 3, 4}));
    }

    // ── removal ──────────────────────────────────────────────────────────

    TEST(InjectedAppIdLogicTest, RemovalKeepsTheOrderOfEverythingElse) {
        std::vector<uint32_t> vec = {1, 2, 3, 4, 5};
        size_t n = vec.size();
        ASSERT_TRUE(RemoveKeepingOrder(vec.data(), n, 2));
        vec.resize(n);

        EXPECT_EQ(vec, (std::vector<uint32_t>{1, 3, 4, 5}));
        EXPECT_TRUE(Sorted(vec));
    }

    // The case that motivated it: swap-with-last unsorts {1,2,3,4,5} when removing 2.
    TEST(InjectedAppIdLogicTest, RemovalDoesNotDoWhatFastRemoveDid) {
        std::vector<uint32_t> fast = {1, 2, 3, 4, 5};
        *std::find(fast.begin(), fast.end(), 2u) = fast.back();
        fast.pop_back();
        EXPECT_FALSE(Sorted(fast));   // {1, 5, 3, 4}

        std::vector<uint32_t> kept = {1, 2, 3, 4, 5};
        size_t n = kept.size();
        RemoveKeepingOrder(kept.data(), n, 2);
        kept.resize(n);
        EXPECT_TRUE(Sorted(kept));
    }

    TEST(InjectedAppIdLogicTest, RemovingFirstLastAndOnlyElements) {
        std::vector<uint32_t> vec = {1, 2, 3};
        size_t n = vec.size();
        EXPECT_TRUE(RemoveKeepingOrder(vec.data(), n, 1));   // first
        EXPECT_TRUE(RemoveKeepingOrder(vec.data(), n, 3));   // last
        EXPECT_EQ(n, 1u);
        EXPECT_EQ(vec[0], 2u);
        EXPECT_TRUE(RemoveKeepingOrder(vec.data(), n, 2));   // only
        EXPECT_EQ(n, 0u);
    }

    TEST(InjectedAppIdLogicTest, RemovingAnAbsentIdChangesNothing) {
        std::vector<uint32_t> vec = {1, 2, 3};
        size_t n = vec.size();
        EXPECT_FALSE(RemoveKeepingOrder(vec.data(), n, 9));
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(vec, (std::vector<uint32_t>{1, 2, 3}));
        size_t zero = 0;
        EXPECT_FALSE(RemoveKeepingOrder(nullptr, zero, 1));
    }

    // With a duplicate present only the first match goes, and the order still holds.
    TEST(InjectedAppIdLogicTest, RemovingADuplicateRemovesOneCopy) {
        std::vector<uint32_t> vec = {1, 2, 2, 3};
        size_t n = vec.size();
        EXPECT_TRUE(RemoveKeepingOrder(vec.data(), n, 2));
        vec.resize(n);
        EXPECT_EQ(vec, (std::vector<uint32_t>{1, 2, 3}));
    }

    // Reconcile in the hook: remove some, append some, sort. Stays ascending throughout.
    TEST(InjectedAppIdLogicTest, RemoveThenAddThenSortStaysAscending) {
        std::vector<uint32_t> vec = {10, 20, 30};
        Inject(vec, {500, 100, 300, 200, 400});
        ASSERT_TRUE(Sorted(vec));

        size_t n = vec.size();
        for (uint32_t id : {100u, 400u, 20u}) {
            ASSERT_TRUE(RemoveKeepingOrder(vec.data(), n, id));
            EXPECT_TRUE(AnalyzeOrder(vec.data(), n).Sorted()) << "after removing " << id;
        }
        vec.resize(n);
        Inject(vec, {250, 5, 600});

        EXPECT_TRUE(Sorted(vec));
        EXPECT_EQ(vec, (std::vector<uint32_t>{5, 10, 30, 200, 250, 300, 500, 600}));
    }

}  // namespace
