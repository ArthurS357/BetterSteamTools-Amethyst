#include "Hook/GateSkipLogic.h"

#include <gtest/gtest.h>

namespace {
    struct FakeAppData {
        bool bSkipFlag = false;
        bool emptySha  = true;
        bool HasEmptyAppInfoSha() const { return emptySha; }
    };
    using Marks = GateSkipLogic::Marks<FakeAppData>;
}

TEST(GateSkipLogic, RaisesOnlyAnUnresolvedInjectedEntry) {
    Marks marks;
    FakeAppData blank;
    EXPECT_TRUE(marks.Raise(&blank, [] { return true; }));
    EXPECT_TRUE(blank.bSkipFlag);
    EXPECT_EQ(marks.Count(), 1u);

    FakeAppData resolved{ false, false };
    EXPECT_FALSE(marks.Raise(&resolved, [] { return true; }));
    EXPECT_FALSE(resolved.bSkipFlag);

    FakeAppData notInjected;   // empty sha but not one of ours: Steam's own business
    EXPECT_FALSE(marks.Raise(&notInjected, [] { return false; }));
    EXPECT_FALSE(notInjected.bSkipFlag);

    EXPECT_FALSE(marks.Raise(nullptr, [] { return true; }));
    EXPECT_EQ(marks.Count(), 1u);
}

// The config lookup takes a lock and the pass asks about hundreds of apps: it must only
// run for entries that could be raised.
TEST(GateSkipLogic, TheConfigLookupIsNotReachedByResolvedOrAlreadyFlaggedEntries) {
    Marks marks;
    int lookups = 0;
    const auto injected = [&] { ++lookups; return true; };

    FakeAppData resolved{ false, false };
    FakeAppData alreadyFlagged{ true, true };
    marks.Raise(&resolved, injected);
    marks.Raise(&alreadyFlagged, injected);
    EXPECT_EQ(lookups, 0);

    FakeAppData blank;
    marks.Raise(&blank, injected);
    EXPECT_EQ(lookups, 1);
}

// A flag Steam set itself (its PICS path) must survive: Restore only undoes our own.
TEST(GateSkipLogic, RestorePutsBackOnlyWhatWasRaised) {
    Marks marks;
    FakeAppData ours;
    FakeAppData steams{ true, true };
    marks.Raise(&ours, [] { return true; });
    marks.Raise(&steams, [] { return true; });

    marks.Restore();
    EXPECT_FALSE(ours.bSkipFlag);
    EXPECT_TRUE(steams.bSkipFlag);
    EXPECT_EQ(marks.Count(), 0u);

    marks.Restore();   // a second restore is a no-op
    EXPECT_TRUE(steams.bSkipFlag);
}

// One entry looked up twice in a pass is raised once and restored once.
TEST(GateSkipLogic, AnEntryQueriedTwiceIsRaisedOnce) {
    Marks marks;
    FakeAppData blank;
    EXPECT_TRUE(marks.Raise(&blank, [] { return true; }));
    EXPECT_FALSE(marks.Raise(&blank, [] { return true; }));
    EXPECT_EQ(marks.Count(), 1u);
    marks.Restore();
    EXPECT_FALSE(blank.bSkipFlag);
}
