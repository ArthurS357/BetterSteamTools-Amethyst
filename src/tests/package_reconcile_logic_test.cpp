#include "Hook/PackageReconcileLogic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
    using PackageReconcileLogic::Plan;
    using Ids = std::vector<AppId_t>;

    bool Has(const Ids& ids, AppId_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    Plan Decide(const Ids& queuedRemovals, const Ids& queuedAdditions, const Ids& config, const Ids& vec) {
        return PackageReconcileLogic::Reconcile(
            queuedRemovals, queuedAdditions,
            [&](AppId_t id) { return Has(config, id); },
            [&](AppId_t id) { return Has(vec, id); });
    }
}

TEST(PackageReconcileLogic, NewIdIsAddedAndDroppedIdIsRemoved) {
    const Plan plan = Decide({2}, {1}, /*config*/ {1}, /*vec*/ {2});
    EXPECT_EQ(plan.additions, Ids{1});
    EXPECT_EQ(plan.removals, Ids{2});
}

// The common reload: the file's ids are in both queues. The vector must not be touched.
TEST(PackageReconcileLogic, IdReloadedUnchangedIsLeftAlone) {
    const Plan plan = Decide({7}, {7}, /*config*/ {7}, /*vec*/ {7});
    EXPECT_TRUE(plan.additions.empty());
    EXPECT_TRUE(plan.removals.empty());
}

// Reloaded twice before the fill, whose snapshot already holds it. Replaying the
// queues would remove it and then insert it twice.
TEST(PackageReconcileLogic, RepeatedReloadsBeforeTheFillDoNotDuplicate) {
    const Plan plan = Decide({9, 9}, {9, 9}, /*config*/ {9}, /*vec*/ {9});
    EXPECT_TRUE(plan.additions.empty());
    EXPECT_TRUE(plan.removals.empty());
}

// Added and deleted before anything drained. Replaying would leave it in the package
// with no later removal to clear it.
TEST(PackageReconcileLogic, IdAddedAndDeletedInOneBatchLeavesNoGhost) {
    const Plan plan = Decide({5}, {5}, /*config*/ {}, /*vec*/ {});
    EXPECT_TRUE(plan.additions.empty());
    EXPECT_TRUE(plan.removals.empty());
}

TEST(PackageReconcileLogic, IdDeletedAfterItsRemovalWasCancelledStillGoes) {
    const Plan plan = Decide({3, 3}, {3}, /*config*/ {}, /*vec*/ {3});
    EXPECT_TRUE(plan.additions.empty());
    EXPECT_EQ(plan.removals, Ids{3});
}
