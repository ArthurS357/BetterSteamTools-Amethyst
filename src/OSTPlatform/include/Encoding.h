#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::Encoding {

    std::string WideToUtf8(std::wstring_view value);
    std::wstring Utf8ToWide(std::string_view value);

    // Builds a std::filesystem::path from a string known to hold UTF-8 bytes
    // (e.g. the output of WideToUtf8, or TOML content -- TOML is UTF-8 by
    // spec). std::filesystem::path(std::string) decodes via the host
    // process's ANSI codepage on MSVC, NOT UTF-8, so feeding it UTF-8 bytes
    // directly mangles any non-ASCII path (wrong even when every character
    // is representable in the ACP, since UTF-8's multi-byte sequences are
    // not ACP byte sequences). This goes through the C++20 char8_t
    // constructor instead, which is required to interpret its input as
    // UTF-8 regardless of host codepage.
    std::filesystem::path Utf8ToPath(std::string_view utf8);

    // Inverse of Utf8ToPath: renders a path back to a UTF-8 std::string.
    // Unlike std::filesystem::path::string() (ANSI codepage on MSVC), this
    // round-trips losslessly with Utf8ToPath for any valid path. Only use
    // this for a std::string that will be re-decoded as UTF-8 downstream
    // (another Utf8ToPath call, a log message, a UTF-8-aware API) -- not for
    // a narrow string an ANSI-only WinAPI call (MessageBoxA, fopen-style
    // WinAPI, ...) will open directly; for that, keep path.string().
    std::string PathToUtf8(const std::filesystem::path& path);

} // namespace OSTPlatform::Encoding
