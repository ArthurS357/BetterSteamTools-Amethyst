// Regression tests for LuaConfig's concurrency contract, run against the real
// LuaConfig.cpp and the real Lua interpreter. Only the units it would reach the
// network or the credential store through are faked, at link time, below.
//
// The fake HTTP call parks a script mid-http_get on a latch, which makes the
// interesting interleavings deterministic: "a reload is in flight" and "another
// thread enters Lua meanwhile" happen every run, not once in a thousand.
//
// The stress test only proves anything under a race detector. MSVC has no
// ThreadSanitizer, so src/tests/linux/luaconfig builds this file under TSan and
// ASan+UBSan in CI, with Lua itself instrumented.
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/Tickets/AppTicket.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::string_view kBlockingUrl = "test://block";
    constexpr auto kLatchTimeout = 10s;

    std::mutex g_httpMutex;
    std::condition_variable g_httpCv;
    bool g_httpEntered = false;
    bool g_httpReleased = false;
    std::wstring g_httpLastHeaders;
}

// ── Link-time fakes ─────────────────────────────────────────────
namespace OSTPlatform::Http {
    Result Execute(const wchar_t*, const char* url, const void*, uint32_t, const wchar_t* headers,
                   uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
        if (headers) {
            std::scoped_lock lock(g_httpMutex);
            g_httpLastHeaders = headers;
        }
        if (kBlockingUrl == url) {
            std::unique_lock lock(g_httpMutex);
            g_httpEntered = true;
            g_httpCv.notify_all();
            g_httpCv.wait_for(lock, kLatchTimeout, [] { return g_httpReleased; });
        }
        Result result;
        result.body = "777";
        result.status = 200;
        result.ok = true;
        return result;
    }
}

namespace StatsClient {
    bool FetchStatSteamId(AppId_t, uint64_t*) { return false; }
}

namespace AppTicket {
    bool WriteAppOwnershipTicket(AppId_t, const std::vector<uint8_t>&) { return true; }
    bool WriteEncryptedTicket(AppId_t, const std::vector<uint8_t>&) { return true; }
}

#ifndef _WIN32
// Numbers.cpp's wide-string overloads reference this. LuaConfig never calls them,
// and the Linux Encoding.h stand-in has no wide API.
namespace OSTPlatform::Encoding {
    std::string WideToUtf8(std::wstring_view) { return {}; }
}
#endif

namespace {
    namespace fs = std::filesystem;

    // Runs ParseFile on its own thread; the constructor returns once the script is
    // parked inside http_get on kBlockingUrl.
    class ParseParkedInHttp {
    public:
        explicit ParseParkedInHttp(std::string file)
            : thread_([file = std::move(file)] { LuaConfig::ParseFile(file); }) {
            std::unique_lock lock(g_httpMutex);
            parked_ = g_httpCv.wait_for(lock, kLatchTimeout, [] { return g_httpEntered; });
        }
        ~ParseParkedInHttp() { Finish(); }

        [[nodiscard]] bool Parked() const { return parked_; }

        void Finish() {
            {
                std::scoped_lock lock(g_httpMutex);
                g_httpReleased = true;
            }
            g_httpCv.notify_all();
            if (thread_.joinable()) thread_.join();
        }

    private:
        std::thread thread_;
        bool parked_ = false;
    };

    class LuaConfigConcurrency : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir_ = fs::temp_directory_path() / (std::string("amethyst_luaconfig_") + info->name());
            fs::remove_all(dir_);
            fs::create_directories(dir_);

            std::scoped_lock lock(g_httpMutex);
            g_httpEntered = false;
            g_httpReleased = false;
        }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }

        std::string WriteLua(const std::string& name, const std::string& content) {
            const fs::path path = dir_ / name;
            std::ofstream(path, std::ios::trunc) << content;
            return OSTPlatform::Encoding::PathToUtf8(path);
        }

        fs::path dir_;
    };
}

// Risk 3: the reload gap. While a reload's script is blocked in HTTP (seconds, in
// production), the previous version must stay fully visible and the new one must
// not leak out partially.
TEST_F(LuaConfigConcurrency, ReloadSwapsAtomicallyWithNoGapWhileScriptIsBlocked) {
    const std::string file = WriteLua("swap.lua", "addappid(1001)\naddappid(1003)\n");
    LuaConfig::ParseFile(file);
    ASSERT_TRUE(LuaConfig::HasDepot(1001, false));
    ASSERT_TRUE(LuaConfig::HasDepot(1003, false));

    WriteLua("swap.lua", "addappid(1001)\nhttp_get('test://block')\naddappid(1002)\n");
    ParseParkedInHttp parse(file);
    ASSERT_TRUE(parse.Parked());

    EXPECT_TRUE(LuaConfig::HasDepot(1001, false)) << "kept id vanished during the reload";
    EXPECT_TRUE(LuaConfig::HasDepot(1003, false)) << "dropped id must stay until the commit";
    EXPECT_FALSE(LuaConfig::HasDepot(1002, false)) << "new id leaked before the commit";

    parse.Finish();
    EXPECT_TRUE(LuaConfig::HasDepot(1001, false));
    EXPECT_TRUE(LuaConfig::HasDepot(1002, false));
    EXPECT_FALSE(LuaConfig::HasDepot(1003, false));
}

TEST_F(LuaConfigConcurrency, TruncatedReloadKeepsThePreviousVersion) {
    const std::string file = WriteLua("truncated.lua", "addappid(2001)\n");
    LuaConfig::ParseFile(file);

    WriteLua("truncated.lua", "addappid(2002)\naddappid(");
    LuaConfig::ParseFile(file);

    EXPECT_TRUE(LuaConfig::HasDepot(2001, false));
    EXPECT_FALSE(LuaConfig::HasDepot(2002, false));
}

TEST_F(LuaConfigConcurrency, UnopenableReloadKeepsThePreviousVersionUntilUnload) {
    const std::string file = WriteLua("gone.lua", "addappid(2101)\n");
    LuaConfig::ParseFile(file);

    fs::remove(dir_ / "gone.lua");
    LuaConfig::ParseFile(file);
    EXPECT_TRUE(LuaConfig::HasDepot(2101, false));

    LuaConfig::UnloadFile(file);
    EXPECT_FALSE(LuaConfig::HasDepot(2101, false));
}

// Truncation is the first step of a non-atomic save, so a file read at zero bytes is
// far more likely to be mid-write than deliberately emptied.
TEST_F(LuaConfigConcurrency, ZeroByteReloadKeepsThePreviousVersion) {
    const std::string file = WriteLua("emptied.lua", "addappid(2301)\n");
    LuaConfig::ParseFile(file);

    WriteLua("emptied.lua", "");
    LuaConfig::ParseFile(file);

    EXPECT_TRUE(LuaConfig::HasDepot(2301, false));
}

// With nothing loaded yet there is no good version to protect, so a file's complete
// statements still apply, as they always have.
TEST_F(LuaConfigConcurrency, TruncatedFirstLoadStillAppliesCompleteStatements) {
    const std::string file = WriteLua("fresh.lua", "addappid(2201)\naddappid(");
    LuaConfig::ParseFile(file);
    EXPECT_TRUE(LuaConfig::HasDepot(2201, false));
}

// A failing statement is skipped, not fatal: community files carry stray calls, and
// treating that as a failed parse would freeze such a file at its first version.
TEST_F(LuaConfigConcurrency, FailingStatementDoesNotBlockAReload) {
    const std::string file = WriteLua("badline.lua", "addappid(3001)\n");
    LuaConfig::ParseFile(file);

    WriteLua("badline.lua", "no_such_function()\naddappid(3002)\n");
    LuaConfig::ParseFile(file);

    EXPECT_FALSE(LuaConfig::HasDepot(3001, false));
    EXPECT_TRUE(LuaConfig::HasDepot(3002, false));
}

// Risk 1: another thread can run Lua while a script sits in HTTP, on its own stack,
// and its config mutations are not attributed to the file being parsed.
//
// The blocker first grows its stack and returns, so the callback's full GC cycle
// finds a parked coroutine with a stack worth shrinking: Lua reallocates a stack out
// from under its owner here, which ASan would catch if the owner kept a pointer.
TEST_F(LuaConfigConcurrency, ManifestCallbackRunsWhileAParseIsBlockedInHttp) {
    LuaConfig::ParseFile(WriteLua("manifest.lua",
        "function fetch_manifest_code(gid) collectgarbage() addappid(5009) return \"777\" end\n"));
    ASSERT_TRUE(LuaConfig::HasManifestCodeFunc());

    const std::string blocker = WriteLua("blocker.lua",
        "local function deep(n) if n == 0 then return 0 end return 1 + deep(n - 1) end "
        "deep(10000) http_get('test://block')\n"
        "addappid(5001)\n");
    ParseParkedInHttp parse(blocker);
    ASSERT_TRUE(parse.Parked());

    auto call = std::async(std::launch::async, [] {
        uint64_t code = 0;
        return LuaConfig::CallManifestFetchCode(42, &code) ? code : 0;
    });
    EXPECT_EQ(call.wait_for(5s), std::future_status::ready)
        << "g_luaMutex was held across http_get";
    EXPECT_TRUE(LuaConfig::HasDepot(5009, false)) << "callback's addappid was staged into the parse";

    parse.Finish();
    EXPECT_EQ(call.get(), 777u);
    EXPECT_TRUE(LuaConfig::HasDepot(5001, false));

    LuaConfig::UnloadFile(blocker);
    EXPECT_FALSE(LuaConfig::HasDepot(5001, false));
    EXPECT_TRUE(LuaConfig::HasDepot(5009, false)) << "callback's addappid was attributed to the parse";
}

// A table value used to reach std::string as a null pointer and take Steam down.
TEST_F(LuaConfigConcurrency, MalformedHttpHeadersAreSkippedAndValidOnesKept) {
    const std::string file = WriteLua("headers.lua",
        "http_get('test://ok', {Accept='json', ['X-Count']=5, nested={}, flag=true, [1]='array'})\n"
        "addappid(7001)\n");
    LuaConfig::ParseFile(file);

    EXPECT_TRUE(LuaConfig::HasDepot(7001, false)) << "the script did not get past http_get";

    std::wstring headers;
    {
        std::scoped_lock lock(g_httpMutex);
        headers = g_httpLastHeaders;
    }
    EXPECT_NE(headers.find(L"Accept: json\r\n"), std::wstring::npos);
    EXPECT_NE(headers.find(L"X-Count: 5\r\n"), std::wstring::npos);
    EXPECT_EQ(headers.find(L"nested"), std::wstring::npos);
    EXPECT_EQ(headers.find(L"flag"), std::wstring::npos);
    EXPECT_EQ(headers.find(L"array"), std::wstring::npos);
}

// A raising __index on _G used to reach the lookups made outside lua_pcall, which
// aborts the process through Lua's panic function. __index only fires for a missing
// name, so this makes each of the four lookups find its name missing. No ASSERT: the
// metatable must be restored at the end, because leaving it would break the
// case-insensitive lookup later tests rely on when they share this process.
TEST_F(LuaConfigConcurrency, GlobalLookupsSurviveARaisingIndexMetamethod) {
    uint64_t code = 0;

    // ParseFile's lookup of the undefined fetch_manifest_code_ex.
    LuaConfig::ParseFile(WriteLua("strict.lua",
        "saved_global_mt = getmetatable(_G)\n"
        "function fetch_manifest_code(gid) fetch_manifest_code = nil return '777' end\n"
        "setmetatable(_G, {__index = function(t, k) error('strict: undefined ' .. k) end})\n"));
    EXPECT_FALSE(LuaConfig::HasManifestCodeFuncEx());
    EXPECT_TRUE(LuaConfig::HasManifestCodeFunc());

    // The callback removes itself while the flag still says it exists: the second
    // call's lookup misses, and a missing function must read as a failed call.
    EXPECT_TRUE(LuaConfig::CallManifestFetchCode(42, &code));
    EXPECT_EQ(code, 777u);
    EXPECT_FALSE(LuaConfig::CallManifestFetchCode(42, &code));

    // ParseFile's lookup of the now-removed fetch_manifest_code, then the same
    // self-removal through the _ex entry point.
    LuaConfig::ParseFile(WriteLua("strict_ex.lua",
        "function fetch_manifest_code_ex(app, depot, gid) fetch_manifest_code_ex = nil return '778' end\n"));
    EXPECT_FALSE(LuaConfig::HasManifestCodeFunc());
    EXPECT_TRUE(LuaConfig::HasManifestCodeFuncEx());
    EXPECT_TRUE(LuaConfig::CallManifestFetchCodeEx(1, 2, 42, &code));
    EXPECT_EQ(code, 778u);
    EXPECT_FALSE(LuaConfig::CallManifestFetchCodeEx(1, 2, 42, &code));

    LuaConfig::ParseFile(WriteLua("unstrict.lua", "setmetatable(_G, saved_global_mt)\n"));
}

// Readers, reloaders and manifest callers all at once. Besides surviving, it asserts
// the two things corruption would break: the stably-provided id never disappears and
// every callback returns the right value off its Lua stack.
TEST_F(LuaConfigConcurrency, StressReadersReloadersAndManifestCalls) {
    constexpr AppId_t kStableId = 6000;
    constexpr AppId_t kReloadedId = 6001;
    const std::string key(64, 'a');

    const std::string stable = WriteLua("stable.lua", "addappid(6000)\nhttp_get('test://ok')\n");
    const std::string reloaded = WriteLua("reloaded.lua",
        "function fetch_manifest_code(gid) return http_get('test://ok') end\n"
        "addappid(6001, 1, '" + key + "')\n"
        "addtoken(6001, '1234')\n"
        "setManifestid(6001, '99')\n"
        "http_post('test://ok', 'payload')\n"
        "addappid(6002)\n");
    LuaConfig::ParseFile(stable);
    LuaConfig::ParseFile(reloaded);
    ASSERT_TRUE(LuaConfig::HasManifestCodeFunc());

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> stableMissing{0}, wrongCodes{0}, reloads{0}, manifestCalls{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            while (!stop) {
                if (!LuaConfig::HasDepot(kStableId, false)) ++stableMissing;
                (void)LuaConfig::GetAllDepotIds();
                (void)LuaConfig::GetDecryptionKey(kReloadedId);
                (void)LuaConfig::GetAccessToken(kReloadedId);
                (void)LuaConfig::GetManifestOverrides();
                (void)LuaConfig::GetPurchaseTime(kReloadedId);
                (void)LuaConfig::GetEticketUrl();
                if (i == 0) {
                    (void)LuaConfig::TakePendingRemovals();
                    (void)LuaConfig::TakePendingAdditions();
                }
            }
        });
    }
    for (const std::string* file : {&stable, &reloaded}) {
        threads.emplace_back([&, file] {
            while (!stop) {
                LuaConfig::ParseFile(*file);
                ++reloads;
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] {
            while (!stop) {
                uint64_t code = 0;
                if (!LuaConfig::CallManifestFetchCode(42, &code) || code != 777) ++wrongCodes;
                ++manifestCalls;
            }
        });
    }

    std::this_thread::sleep_for(2s);
    stop = true;
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(stableMissing.load(), 0u);
    EXPECT_EQ(wrongCodes.load(), 0u);
    EXPECT_GT(reloads.load(), 0u);
    EXPECT_GT(manifestCalls.load(), 0u);
}
