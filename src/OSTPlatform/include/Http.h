#pragma once

#include <cstdint>
#include <string>

namespace OSTPlatform::Http {

    // The WinHttpOpen User-Agent every Execute() call uses unless overridden via
    // SetUserAgent(). This exact string is what manifest.opensteamtool.com's
    // Cloudflare WAF allowlists -- any other value (observed: a plain rename to
    // "AmethystTool/1.0") gets a JS-challenge response WinHTTP can never solve,
    // which surfaces as "download fails, HTTP request never succeeds". Header-only
    // so it stays testable without linking WinHTTP (see http_user_agent_test.cpp).
    inline constexpr const wchar_t* kDefaultUserAgent = L"OpenSteamTool/1.0";

    // Overrides the User-Agent sent by Execute(). Pass nullptr or an empty string
    // to restore kDefaultUserAgent. Thread-safe; takes effect on the next Execute()
    // call. Driven from [http] user_agent in amethysttool.toml (see Config.cpp) so
    // a future server-side allowlist change doesn't require a rebuild.
    void SetUserAgent(const wchar_t* userAgent);

    // Returns the User-Agent currently in effect.
    std::wstring ActiveUserAgent();

    struct Result {
        std::string body;
        uint32_t status = 0;
        // True once WinHTTP produced a response; callers must still check status for 2xx.
        bool ok = false;
    };

    Result Execute(const wchar_t* method,
                   const char* url,
                   const void* reqBody = nullptr,
                   uint32_t reqBodyLen = 0,
                   const wchar_t* headers = nullptr,
                   uint32_t timeoutResolve = 5000,
                   uint32_t timeoutConnect = 5000,
                   uint32_t timeoutSend = 10000,
                   uint32_t timeoutRecv = 10000,
                   // Upper bound on the response body read into memory. The default
                   // suits small metadata (TOML/JSON); callers fetching larger payloads
                   // (e.g. a DLL) must raise it. Reading stops once the cap is reached.
                   uint32_t maxBodyBytes = 256 * 1024);

} // namespace OSTPlatform::Http
