// Regression test for the manifest.opensteamtool.com download failure in
// AmethystTool v1.1.0: the DLL's User-Agent was renamed from "OpenSteamTool/1.0"
// to "AmethystTool/1.0", and the server's Cloudflare WAF allowlists only the
// former -- any other User-Agent gets a JS-challenge response that WinHTTP can
// never solve, so every manifest fetch fails with a connection error even
// though the game was already added to the library. Confirmed empirically:
//   curl -H "User-Agent: OpenSteamTool/1.0"  https://manifest.opensteamtool.com/440  -> 200
//   curl -H "User-Agent: AmethystTool/1.0"   https://manifest.opensteamtool.com/440  -> 403 (cf challenge)
//
// This pins OSTPlatform::Http::kDefaultUserAgent so a future rename can't
// silently reintroduce the same failure. It only includes Http.h (no WinHTTP
// dependency, see the header), so it needs no new link dependency in
// CMakeLists.txt -- consistent with this suite's WinHTTP/Detours-free scope.

#include "OSTPlatform/include/Http.h"

#include <gtest/gtest.h>

#include <string>

TEST(HttpUserAgent, DefaultMatchesManifestServerAllowlist) {
    EXPECT_EQ(std::wstring(OSTPlatform::Http::kDefaultUserAgent), L"OpenSteamTool/1.0");
}
