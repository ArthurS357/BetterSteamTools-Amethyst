// Unit tests for RemoteTomlLogic -- the mirror chain and fall-through policy
// behind RemoteToml::Fetch (see Utils/SteamMetadata/RemoteTomlLogic.h),
// extracted so they are testable without WinHTTP, Config or the filesystem.

#include "Utils/SteamMetadata/RemoteTomlLogic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using RemoteTomlLogic::BuildMirrorUrls;
using RemoteTomlLogic::ExpandTemplate;
using RemoteTomlLogic::IsMirrorHit;
using RemoteTomlLogic::IsValidTemplate;
using RemoteTomlLogic::kDefaultTemplates;

namespace {

    const std::vector<std::string> kChain{ kDefaultTemplates.begin(), kDefaultTemplates.end() };

    std::vector<std::string> DefaultUrls() {
        return BuildMirrorUrls(kChain, "pattern", "steamclient", "abc");
    }

    // Mirrors RemoteToml::Fetch's loop: walk the URLs in order, stop at the
    // first hit. Returns every URL attempted.
    std::vector<std::string> Attempted(const std::vector<std::string>& urls,
                                       const std::vector<uint32_t>& statusPerUrl) {
        std::vector<std::string> tried;
        for (size_t i = 0; i < urls.size(); ++i) {
            tried.push_back(urls[i]);
            if (IsMirrorHit(true, statusPerUrl[i])) break;
        }
        return tried;
    }

    // ── chain order ──────────────────────────────────────────────────

    TEST(RemoteTomlLogicTest, ActiveMirrorComesBeforeStaleOne) {
        ASSERT_EQ(kDefaultTemplates.size(), 5u);
        EXPECT_EQ(kDefaultTemplates[0].rfind("https://raw.githubusercontent.com/madoiscool/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[1].rfind("https://cdn.jsdelivr.net/gh/madoiscool/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[2].rfind("https://raw.githubusercontent.com/OpenSteam001/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[3].rfind("https://cdn.jsdelivr.net/gh/OpenSteam001/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[4].rfind("https://git.lua.tools/", 0), 0u);
    }

    TEST(RemoteTomlLogicTest, EveryBuiltInTemplateIsValid) {
        for (auto t : kDefaultTemplates)
            EXPECT_TRUE(IsValidTemplate(t)) << t;
    }

    TEST(RemoteTomlLogicTest, FirstUrlTriedIsActiveMirrorRaw) {
        const auto urls = DefaultUrls();
        ASSERT_EQ(urls.size(), kDefaultTemplates.size());
        EXPECT_EQ(urls[0],
                  "https://raw.githubusercontent.com/madoiscool/steam-monitor/pattern/steamclient/abc.toml");
    }

    // ── template validation / expansion ──────────────────────────────

    TEST(RemoteTomlLogicTest, RejectsTemplateMissingAnyPlaceholder) {
        EXPECT_FALSE(IsValidTemplate("https://x/{component}/{sha256}.toml"));
        EXPECT_FALSE(IsValidTemplate("https://x/{channel}/{sha256}.toml"));
        EXPECT_FALSE(IsValidTemplate("https://x/{channel}/{component}.toml"));
        EXPECT_FALSE(IsValidTemplate(""));
    }

    TEST(RemoteTomlLogicTest, ExpandsEveryPlaceholderOccurrence) {
        EXPECT_EQ(ExpandTemplate("{channel}/{component}/{sha256}/{sha256}", "pattern", "steamui", "ff"),
                  "pattern/steamui/ff/ff");
    }

    // ── fall-through policy ──────────────────────────────────────────

    TEST(RemoteTomlLogicTest, OnlyTransportOkPlus200IsAHit) {
        EXPECT_TRUE(IsMirrorHit(true, 200));
        EXPECT_FALSE(IsMirrorHit(true, 404));
        EXPECT_FALSE(IsMirrorHit(true, 503));
        EXPECT_FALSE(IsMirrorHit(true, 0));
        EXPECT_FALSE(IsMirrorHit(false, 200));   // stale status from a failed transport
    }

    TEST(RemoteTomlLogicTest, FallsThroughToStaleMirrorWhenActiveOneFails) {
        const auto urls = DefaultUrls();
        const auto tried = Attempted(urls, { 404, 404, 200, 200, 200 });
        ASSERT_EQ(tried.size(), 3u);
        EXPECT_NE(tried[2].find("/OpenSteam001/"), std::string::npos);
    }

    TEST(RemoteTomlLogicTest, TriesAllFiveMirrorsInOrderWhenAllFail) {
        const auto urls = DefaultUrls();
        const auto tried = Attempted(urls, { 404, 404, 404, 404, 404 });
        ASSERT_EQ(tried.size(), kDefaultTemplates.size());
        for (size_t i = 0; i < kChain.size(); ++i)
            EXPECT_EQ(tried[i], ExpandTemplate(kChain[i], "pattern", "steamclient", "abc"));
    }

    TEST(RemoteTomlLogicTest, EmptyChainYieldsNoUrls) {
        EXPECT_TRUE(BuildMirrorUrls({}, "pattern", "steamclient", "abc").empty());
    }

} // namespace
