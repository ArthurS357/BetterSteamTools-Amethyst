#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pure parsing helpers for the amethysttool:// URI bridge and the code
// server's flat JSON/hex wire format. No Http/Dialog/SteamCredentialStore
// dependencies, so this pair can be compiled directly into the test binary
// (see src/tests/CMakeLists.txt) instead of linking all of TokeerBridge.cpp.
namespace TokeerBridge::Parsing {

    inline constexpr std::string_view kUriScheme = "amethysttool";

    // Decodes a hex string (even length, [0-9a-fA-F] only) into bytes.
    // nullopt on empty input, odd length, or a non-hex character.
    [[nodiscard]] std::optional<std::vector<uint8_t>> HexToBytes(std::string_view hex);

    // Minimal "key":"value" string extraction from a flat (non-nested) JSON
    // object. Returns false if the key is absent or its value isn't a
    // quoted string (e.g. it's null/number, or the JSON is malformed).
    // Not [[nodiscard]]: callers legitimately probe for an optional field and
    // rely on `out` staying untouched on failure (see TokeerBridge::Redeem).
    bool JsonString(std::string_view body, std::string_view key, std::string& out);

    // True if `key`'s value in the flat JSON object is the literal `true`.
    [[nodiscard]] bool JsonTrue(std::string_view body, std::string_view key);

    struct ParsedUri {
        // The URL after whitespace trim and de-quoting, regardless of match outcome
        // (used for diagnostics even when matchedScheme is false).
        std::string cleaned;
        bool matchedScheme = false;
        std::string action;
        std::string arg;   // empty if no "/arg" segment was present
    };

    // Trims whitespace, strips one layer of wrapping quotes (rundll32 leaves the
    // literal quote characters from the registry command's "%1" in argv), checks
    // the amethysttool:// scheme, strips trailing slashes/whitespace, and splits
    // the remainder into action/arg on the first '/'.
    [[nodiscard]] ParsedUri ParseUri(std::string_view rawUrl);

} // namespace TokeerBridge::Parsing
