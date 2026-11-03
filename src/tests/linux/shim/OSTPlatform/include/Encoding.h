#pragma once

// Linux-CI stand-in for src/OSTPlatform/include/Encoding.h (the real one is
// implemented over WideCharToMultiByte / MultiByteToWideChar in Encoding.cpp and
// cannot build off Windows).
//
// std::filesystem::path on Linux is already a UTF-8 byte string, so both
// conversions are the identity. That is exactly the property the Windows
// implementation exists to emulate, which makes this a faithful stand-in for the
// logic units that merely *call* it (CompatAliasLogic's non-ASCII Steam-path
// test). It does NOT test Encoding.cpp itself -- encoding_test.cpp stays
// Windows-only.
//
// Only reachable through tests/linux/CMakeLists.txt, which puts this directory
// ahead of src/ on the include path; the Windows build never sees it.

#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::Encoding {

    inline std::filesystem::path Utf8ToPath(std::string_view utf8) {
        return std::filesystem::path(std::string(utf8));
    }

    inline std::string PathToUtf8(const std::filesystem::path& path) {
        return path.string();
    }

} // namespace OSTPlatform::Encoding
