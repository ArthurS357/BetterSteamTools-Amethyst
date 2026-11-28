#include "Hook/GateSkipLogic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>

namespace {
    struct FakeAppData {
        bool bSkipFlag = false;
        bool emptySha  = true;
        bool HasEmptyAppInfoSha() const { return emptySha; }
    };
    using Marks = GateSkipLogic::Marks<FakeAppData>;

    // Steam's appinfo cache as Restore sees it: id -> current entry, nullptr when absent.
    struct FakeCache {
        std::map<std::uint32_t, FakeAppData*> entries;
        int lookups = 0;
        auto Lookup() {
            return [this](std::uint32_t id) -> FakeAppData* {
                ++lookups;
                const auto it = entries.find(id);
                return it == entries.end() ? nullptr : it->second;
            };
        }
    };

    const auto kYes = [] { return true; };
}

TEST(GateSkipLogic, RaisesOnlyAnUnresolvedInjectedEntry) {
    Marks marks;
    FakeAppData blank;
    EXPECT_TRUE(marks.Raise(1, &blank, kYes));
    EXPECT_TRUE(blank.bSkipFlag);
    EXPECT_EQ(marks.Count(), 1u);

    FakeAppData resolved{ false, false };
    EXPECT_FALSE(marks.Raise(2, &resolved, kYes));
    EXPECT_FALSE(resolved.bSkipFlag);

    FakeAppData notInjected;   // empty sha but not one of ours: Steam's own business
    EXPECT_FALSE(marks.Raise(3, &notInjected, [] { return false; }));
    EXPECT_FALSE(notInjected.bSkipFlag);

    EXPECT_FALSE(marks.Raise(4, nullptr, kYes));
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
    marks.Raise(1, &resolved, injected);
    marks.Raise(2, &alreadyFlagged, injected);
    EXPECT_EQ(lookups, 0);

    FakeAppData blank;
    marks.Raise(3, &blank, injected);
    EXPECT_EQ(lookups, 1);
}

// A flag Steam set itself (its PICS path) must survive: Restore only undoes our own.
TEST(GateSkipLogic, RestorePutsBackOnlyWhatWasRaised) {
    Marks marks;
    FakeAppData ours;
    FakeAppData steams{ true, true };
    FakeCache cache;
    cache.entries = { { 10, &ours }, { 20, &steams } };
    marks.Raise(10, &ours, kYes);
    marks.Raise(20, &steams, kYes);

    EXPECT_EQ(marks.Restore(cache.Lookup()), 1u);
    EXPECT_FALSE(ours.bSkipFlag);
    EXPECT_TRUE(steams.bSkipFlag);
    EXPECT_EQ(marks.Count(), 0u);

    EXPECT_EQ(marks.Restore(cache.Lookup()), 0u);   // a second restore is a no-op
    EXPECT_TRUE(steams.bSkipFlag);
}

// One entry looked up twice in a pass is raised once and restored once.
TEST(GateSkipLogic, AnEntryQueriedTwiceIsRaisedOnce) {
    Marks marks;
    FakeAppData blank;
    FakeCache cache;
    cache.entries = { { 7, &blank } };
    EXPECT_TRUE(marks.Raise(7, &blank, kYes));
    EXPECT_FALSE(marks.Raise(7, &blank, kYes));
    EXPECT_EQ(marks.Count(), 1u);
    EXPECT_EQ(marks.Restore(cache.Lookup()), 1u);
    EXPECT_FALSE(blank.bSkipFlag);
}

// M2 (v1.6.0): Restore goes by id. Each raised id is looked up exactly once.
TEST(GateSkipLogic, RestoreLooksEveryRaisedIdUpAgain) {
    Marks marks;
    FakeAppData a, b, c;
    FakeCache cache;
    cache.entries = { { 1, &a }, { 2, &b }, { 3, &c } };
    marks.Raise(1, &a, kYes);
    marks.Raise(2, &b, kYes);
    marks.Raise(3, &c, kYes);

    EXPECT_EQ(marks.Restore(cache.Lookup()), 3u);
    EXPECT_EQ(cache.lookups, 3);
    EXPECT_FALSE(a.bSkipFlag || b.bSkipFlag || c.bSkipFlag);
}

// An entry Steam dropped during the pass: the lookup finds nothing, and the stale pointer
// is never written through. The freed object is simulated by a sentinel that would show a
// write.
TEST(GateSkipLogic, AnEntryGoneFromTheCacheIsNotTouched) {
    Marks marks;
    auto freed = std::make_unique<FakeAppData>();
    FakeCache cache;
    cache.entries = { { 5, freed.get() } };
    ASSERT_TRUE(marks.Raise(5, freed.get(), kYes));

    cache.entries.erase(5);   // Steam purged it
    EXPECT_EQ(marks.Restore(cache.Lookup()), 0u);
    EXPECT_TRUE(freed->bSkipFlag) << "a pointer the cache no longer returns must not be written";
    EXPECT_EQ(marks.Count(), 0u);
}

// An entry Steam replaced with a new object for the same id: the new one was never raised
// by us, so whatever flag it carries is Steam's and stays.
TEST(GateSkipLogic, AReplacedEntryKeepsSteamsFlag) {
    Marks marks;
    FakeAppData old;
    FakeAppData replacement{ true, true };   // Steam's own "known unknown"
    FakeCache cache;
    cache.entries = { { 9, &old } };
    ASSERT_TRUE(marks.Raise(9, &old, kYes));

    cache.entries[9] = &replacement;
    EXPECT_EQ(marks.Restore(cache.Lookup()), 0u);
    EXPECT_TRUE(replacement.bSkipFlag);
    EXPECT_TRUE(old.bSkipFlag) << "the old object is not written either";
}

// Many ids, some dropped, some replaced, the rest intact: only the intact ones are restored.
TEST(GateSkipLogic, MixedCacheChangesRestoreOnlyTheIntactEntries) {
    constexpr std::uint32_t kN = 300;
    std::vector<std::unique_ptr<FakeAppData>> raised, replacements;
    FakeCache cache;
    Marks marks;
    for (std::uint32_t id = 0; id < kN; ++id) {
        raised.push_back(std::make_unique<FakeAppData>());
        cache.entries[id] = raised.back().get();
        ASSERT_TRUE(marks.Raise(id, raised.back().get(), kYes));
    }
    for (std::uint32_t id = 0; id < kN; ++id) {
        if (id % 3 == 1) cache.entries.erase(id);
        if (id % 3 == 2) {
            replacements.push_back(std::make_unique<FakeAppData>());
            cache.entries[id] = replacements.back().get();
        }
    }

    EXPECT_EQ(marks.Restore(cache.Lookup()), kN / 3);
    for (std::uint32_t id = 0; id < kN; ++id)
        EXPECT_EQ(raised[id]->bSkipFlag, id % 3 != 0) << id;
    for (const auto& r : replacements) EXPECT_FALSE(r->bSkipFlag);
}
