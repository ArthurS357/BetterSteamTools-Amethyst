// Unit tests for AppIdVecOrderLogic (Hook/AppIdVecOrderLogic.h): the order measurement behind
// the AppIdVec diagnostics in Hooks_Package.cpp.

#include "Hook/AppIdVecOrderLogic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using AppIdVecOrderLogic::AnalyzeOrder;
using AppIdVecOrderLogic::PositionOf;

namespace {

    TEST(AppIdVecOrderLogicTest, EmptyAndSingleAreSorted) {
        EXPECT_TRUE(AnalyzeOrder(nullptr, 0).Sorted());
        const uint32_t one[] = {42};
        const auto r = AnalyzeOrder(one, 1);
        EXPECT_TRUE(r.Sorted());
        EXPECT_EQ(r.size, 1u);
    }

    TEST(AppIdVecOrderLogicTest, AscendingWithDuplicatesIsSorted) {
        const std::vector<uint32_t> v = {1, 2, 2, 5, 9, 9, 10};
        EXPECT_TRUE(AnalyzeOrder(v.data(), v.size()).Sorted());
    }

    TEST(AppIdVecOrderLogicTest, ReportsTheFirstInversionAndCountsAll) {
        const std::vector<uint32_t> v = {10, 20, 15, 30, 25, 40};
        const auto r = AnalyzeOrder(v.data(), v.size());
        EXPECT_FALSE(r.Sorted());
        EXPECT_EQ(r.inversions, 2u);   // 20>15 and 30>25
        EXPECT_EQ(r.firstAt, 1u);
        EXPECT_EQ(r.firstPrev, 20u);
        EXPECT_EQ(r.firstNext, 15u);
        EXPECT_EQ(r.size, 6u);
    }

    TEST(AppIdVecOrderLogicTest, DescendingCountsEveryPair) {
        const std::vector<uint32_t> v = {5, 4, 3, 2, 1};
        EXPECT_EQ(AnalyzeOrder(v.data(), v.size()).inversions, 4u);
    }

    // The shape the hook produces: a sorted pre-existing part, then lua ids appended in
    // container order. The whole vector is unsorted even though each part may be sorted.
    TEST(AppIdVecOrderLogicTest, SortedPrefixPlusAppendedUnorderedIdsIsUnsorted) {
        std::vector<uint32_t> v = {7, 70, 700, 7000};
        const size_t pre = v.size();
        for (uint32_t id : {391541u, 391540u, 1868141u, 228989u}) v.push_back(id);

        EXPECT_TRUE(AnalyzeOrder(v.data(), pre).Sorted());
        EXPECT_FALSE(AnalyzeOrder(v.data() + pre, v.size() - pre).Sorted());
        EXPECT_FALSE(AnalyzeOrder(v.data(), v.size()).Sorted());
    }

    // FastRemove swaps the last element into the hole: removing from a sorted vector can
    // unsort it.
    TEST(AppIdVecOrderLogicTest, FastRemoveOfAnInnerElementUnsortsASortedVector) {
        std::vector<uint32_t> v = {1, 2, 3, 4, 5};
        ASSERT_TRUE(AnalyzeOrder(v.data(), v.size()).Sorted());

        const auto it = std::find(v.begin(), v.end(), 2u);   // what CUtlVector::FindAndFastRemove does
        *it = v.back();
        v.pop_back();

        EXPECT_FALSE(AnalyzeOrder(v.data(), v.size()).Sorted());   // {1, 5, 3, 4}
    }

    TEST(AppIdVecOrderLogicTest, PositionOfFindsTheFirstMatchOrNothing) {
        const std::vector<uint32_t> v = {9, 4, 9, 1};
        EXPECT_EQ(PositionOf(v.data(), v.size(), 9).value(), 0u);
        EXPECT_EQ(PositionOf(v.data(), v.size(), 1).value(), 3u);
        EXPECT_FALSE(PositionOf(v.data(), v.size(), 7).has_value());
        EXPECT_FALSE(PositionOf(nullptr, 0, 7).has_value());
    }

}  // namespace
