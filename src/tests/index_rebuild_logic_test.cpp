#include "Hook/IndexRebuildLogic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using IndexRebuildLogic::AppInfoUpdateMsg;

// Steam's own license-change path marks the package first, then raises the flags.
TEST(IndexRebuildLogic, MarksPackageBeforeRaisingTheFlag) {
    std::vector<std::string> calls;
    int user = 0;
    const bool ok = IndexRebuildLogic::MarkAndRequestRebuild(&user,
        [&](void*) { calls.push_back("mark"); },
        [&](void*, AppInfoUpdateMsg*) { calls.push_back("trigger"); });
    EXPECT_TRUE(ok);
    EXPECT_EQ(calls, (std::vector<std::string>{"mark", "trigger"}));
}

// The handler sets the rebuild flag only when the update reports at least one app.
TEST(IndexRebuildLogic, TriggerGetsTheUserAndAMessageWithOneAppUpdated) {
    int user = 0;
    void* seenUser = nullptr;
    AppInfoUpdateMsg seen{};
    IndexRebuildLogic::MarkAndRequestRebuild(&user,
        [](void*) {},
        [&](void* u, AppInfoUpdateMsg* m) { seenUser = u; seen = *m; });
    EXPECT_EQ(seenUser, &user);
    EXPECT_EQ(seen.result, 1u);
    EXPECT_EQ(seen.appsUpdated, 1u);
    EXPECT_EQ(seen.packagesUpdated, 0u);
}

TEST(IndexRebuildLogic, NoUserCallsNothing) {
    int calls = 0;
    const bool ok = IndexRebuildLogic::MarkAndRequestRebuild(nullptr,
        [&](void*) { ++calls; },
        [&](void*, AppInfoUpdateMsg*) { ++calls; });
    EXPECT_FALSE(ok);
    EXPECT_EQ(calls, 0);
}
