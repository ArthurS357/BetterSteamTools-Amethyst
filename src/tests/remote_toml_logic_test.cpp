// Unit tests for RemoteTomlLogic -- the mirror chain and fall-through policy
// behind RemoteToml::Fetch (see Utils/SteamMetadata/RemoteTomlLogic.h),
// extracted so they are testable without WinHTTP, Config or the filesystem.

#include "Utils/SteamMetadata/RemoteTomlLogic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
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
        EXPECT_EQ(kDefaultTemplates[0].rfind("https://cdn.jsdelivr.net/gh/madoiscool/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[1].rfind("https://git.lua.tools/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[2].rfind("https://raw.githubusercontent.com/madoiscool/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[3].rfind("https://cdn.jsdelivr.net/gh/OpenSteam001/", 0), 0u);
        EXPECT_EQ(kDefaultTemplates[4].rfind("https://raw.githubusercontent.com/OpenSteam001/", 0), 0u);
    }

    // v1.5.0 (upstream v1.0.9): for every repo, its jsDelivr copy is tried before
    // its GitHub raw copy, so a network that cannot reach GitHub raw never pays
    // the connect timeout before the CDN that answers.
    TEST(RemoteTomlLogicTest, JsDelivrPrecedesGithubRawForEachRepo) {
        for (const char* repo : { "madoiscool", "OpenSteam001" }) {
            size_t cdn = kDefaultTemplates.size(), raw = kDefaultTemplates.size();
            for (size_t i = 0; i < kDefaultTemplates.size(); ++i) {
                const std::string t(kDefaultTemplates[i]);
                if (t.find(std::string("cdn.jsdelivr.net/gh/") + repo + "/") != std::string::npos) cdn = i;
                if (t.find(std::string("raw.githubusercontent.com/") + repo + "/") != std::string::npos) raw = i;
            }
            ASSERT_LT(cdn, kDefaultTemplates.size()) << repo;
            ASSERT_LT(raw, kDefaultTemplates.size()) << repo;
            EXPECT_LT(cdn, raw) << repo;
        }
    }

    TEST(RemoteTomlLogicTest, EveryBuiltInTemplateIsValid) {
        for (auto t : kDefaultTemplates)
            EXPECT_TRUE(IsValidTemplate(t)) << t;
    }

    TEST(RemoteTomlLogicTest, FirstUrlTriedIsActiveMirrorJsDelivr) {
        const auto urls = DefaultUrls();
        ASSERT_EQ(urls.size(), kDefaultTemplates.size());
        EXPECT_EQ(urls[0],
                  "https://cdn.jsdelivr.net/gh/madoiscool/steam-monitor@pattern/steamclient/abc.toml");
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
        const auto tried = Attempted(urls, { 404, 404, 404, 200, 200 });
        ASSERT_EQ(tried.size(), 4u);
        EXPECT_NE(tried[3].find("/OpenSteam001/"), std::string::npos);
    }

    // ── forced cache miss (WalkMissMirrors with a fake network) ──────
    //
    // Regression for upstream v1.0.9 / "invalid content configuration": the
    // miss walk runs on the init thread before hooks install. The fake network
    // charges a dead host (HTTP=0) its connect timeout, as WinHTTP does, and
    // records the timeouts every request was given.

    struct FakeResult {
        bool     ok     = false;
        uint32_t status = 0;
    };

    struct FakeCall {
        std::string url;
        uint32_t resolveMs, connectMs, sendMs, recvMs;
    };

    // Rules are matched by substring in order; the first match decides.
    // status 0 = unreachable host (transport failure).
    struct FakeNet {
        std::vector<std::pair<std::string, uint32_t>> rules;
        std::vector<FakeCall> calls;
        uint32_t elapsedMs = 0;

        FakeResult operator()(const std::string& url, uint32_t resolveMs, uint32_t connectMs,
                              uint32_t sendMs, uint32_t recvMs) {
            calls.push_back({ url, resolveMs, connectMs, sendMs, recvMs });
            for (const auto& [needle, status] : rules) {
                if (url.find(needle) == std::string::npos) continue;
                if (status == 0) { elapsedMs += connectMs; return {}; }
                return { true, status };
            }
            return { true, 404 };
        }
    };

    FakeResult Walk(FakeNet& net, std::string& lastUrl) {
        return RemoteTomlLogic::WalkMissMirrors<FakeResult>(
            DefaultUrls(),
            [&](const std::string& u, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return net(u, a, b, c, d); },
            lastUrl);
    }

    void ExpectMissTimeouts(const FakeNet& net) {
        for (const FakeCall& c : net.calls) {
            EXPECT_EQ(c.resolveMs, RemoteTomlLogic::kMissTimeoutMs) << c.url;
            EXPECT_EQ(c.connectMs, RemoteTomlLogic::kMissTimeoutMs) << c.url;
            EXPECT_EQ(c.sendMs, RemoteTomlLogic::kMissRecvTimeoutMs) << c.url;
            EXPECT_EQ(c.recvMs, RemoteTomlLogic::kMissRecvTimeoutMs) << c.url;
        }
    }

    TEST(RemoteTomlMissWalk, BlockedGithubRawCostsNothingWhenJsDelivrHasTheFile) {
        FakeNet net{ { { "raw.githubusercontent.com", 0 }, { "cdn.jsdelivr.net", 200 } } };
        std::string lastUrl;
        const FakeResult r = Walk(net, lastUrl);
        EXPECT_TRUE(IsMirrorHit(r.ok, r.status));
        ASSERT_EQ(net.calls.size(), 1u);
        EXPECT_EQ(net.elapsedMs, 0u);
        EXPECT_EQ(lastUrl, "https://cdn.jsdelivr.net/gh/madoiscool/steam-monitor@pattern/steamclient/abc.toml");
        ExpectMissTimeouts(net);
    }

    TEST(RemoteTomlMissWalk, BlockedGithubRawCostsOneShortTimeoutWhenFallingBackToOtherRepo) {
        // madoiscool lacks this hash (jsDelivr and lua.tools 404, its raw dead);
        // OpenSteam001's jsDelivr copy has it.
        FakeNet net{ { { "raw.githubusercontent.com", 0 },
                       { "cdn.jsdelivr.net/gh/OpenSteam001", 200 },
                       { "cdn.jsdelivr.net", 404 },
                       { "git.lua.tools", 404 } } };
        std::string lastUrl;
        const FakeResult r = Walk(net, lastUrl);
        EXPECT_TRUE(IsMirrorHit(r.ok, r.status));
        ASSERT_EQ(net.calls.size(), 4u);
        EXPECT_NE(lastUrl.find("cdn.jsdelivr.net/gh/OpenSteam001/"), std::string::npos);
        EXPECT_EQ(net.elapsedMs, RemoteTomlLogic::kMissTimeoutMs);   // one dead raw host
        ExpectMissTimeouts(net);
    }

    TEST(RemoteTomlMissWalk, EverythingDeadWalksTheWholeChainWithinShortTimeouts) {
        FakeNet net{ { { "https://", 0 } } };
        std::string lastUrl;
        const FakeResult r = Walk(net, lastUrl);
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(net.calls.size(), kDefaultTemplates.size());
        EXPECT_EQ(net.elapsedMs, kDefaultTemplates.size() * RemoteTomlLogic::kMissTimeoutMs);
        ExpectMissTimeouts(net);
    }

    TEST(RemoteTomlMissWalk, EmptyChainMakesNoRequest) {
        FakeNet net;
        std::string lastUrl;
        const FakeResult r = RemoteTomlLogic::WalkMissMirrors<FakeResult>(
            {}, [&](const std::string& u, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return net(u, a, b, c, d); },
            lastUrl);
        EXPECT_FALSE(r.ok);
        EXPECT_TRUE(net.calls.empty());
        EXPECT_TRUE(lastUrl.empty());
    }

    TEST(RemoteTomlLogicTest, MissTimeoutsAreShortAndBounded) {
        static_assert(RemoteTomlLogic::kMissTimeoutMs == 2500);
        static_assert(RemoteTomlLogic::kMissRecvTimeoutMs == 5000);
        static_assert(RemoteTomlLogic::kMissTimeoutMs < 5000);       // OSTPlatform::Http default
        static_assert(RemoteTomlLogic::kMissRecvTimeoutMs < 10000);  // OSTPlatform::Http default
        SUCCEED();
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
