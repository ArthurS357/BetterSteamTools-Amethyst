#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pure part of the PICS response diagnostics (Hooks_NetPacket.cpp, Hooks_NetPacket_AccessToken).
// Observability only: nothing here changes a request or a reply.
//
// Why it exists: a lua app (Hubcap exports `addappid(id, 1, "<depot key>")` and no `addtoken`)
// is asked from PICS with access_token 0. For an app the account does not own, PICS answers
// such a request with `missing_token` and no appinfo buffer, which would leave the app with 0
// depots and make BuildDepotDependency's empty-list guard fail the install ("Invalid content
// configuration"). This header turns the reply into the facts needed to confirm or refute that.
namespace PicsDiagnosticsLogic {

    struct AppSummary {
        uint32_t              appId        = 0;
        bool                  luaApp       = false;   // declared by a .lua (LuaConfig::HasDepot)
        bool                  tokenSent    = false;   // the request that asked for it carried a token != 0
        bool                  missingToken = false;   // reply flag: server wanted a token
        size_t                bufferBytes  = 0;       // appinfo payload size in the reply
        std::optional<uint32_t> depots;               // nullopt: buffer absent or not text VDF
    };

    // A lua app whose reply carries no usable depot list: the case to flag.
    // `depots` unknown (binary or absent buffer) counts only when the buffer is empty or the
    // server said missing_token, so an unreadable-but-present buffer is never reported as empty.
    [[nodiscard]] inline bool LuaAppCameBackEmpty(const AppSummary& s) {
        if (!s.luaApp) return false;
        if (s.missingToken || s.bufferBytes == 0) return true;
        return s.depots.has_value() && *s.depots == 0;
    }

    namespace detail {
        inline bool AllDigits(std::string_view s) {
            if (s.empty()) return false;
            for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            return true;
        }
        constexpr size_t kMaxDepth = 32;   // deeper than any real appinfo; bounds the key stack
    }

    // Counts the numeric-keyed sections directly under appinfo/depots in a TEXT VDF buffer:
    //   "appinfo" { "depots" { "1868141" { ... } "branches" { ... } } }
    // Returns nullopt for an empty buffer or one that does not start like text VDF (a binary
    // buffer is not parsed). A text buffer with no depots section is a valid 0. Tolerant of a
    // trailing NUL, unterminated strings and unbalanced braces; linear time, bounded depth.
    [[nodiscard]] inline std::optional<uint32_t> CountAppInfoDepots(std::string_view buf) {
        while (!buf.empty() && buf.back() == '\0') buf.remove_suffix(1);
        size_t i = 0;
        while (i < buf.size() && std::isspace(static_cast<unsigned char>(buf[i]))) ++i;
        if (i >= buf.size() || buf[i] != '"') return std::nullopt;

        std::vector<std::string> path;      // keys of the sections currently open
        std::string pending;                // last string token, a key until proven a value
        bool        havePending = false;
        uint32_t    count = 0;

        auto readString = [&](std::string& out) {
            out.clear();
            ++i;  // opening quote
            while (i < buf.size() && buf[i] != '"') {
                if (buf[i] == '\\' && i + 1 < buf.size()) ++i;
                out.push_back(buf[i++]);
            }
            if (i < buf.size()) ++i;  // closing quote
        };

        std::string tok;
        while (i < buf.size()) {
            const char c = buf[i];
            if (c == '"') {
                readString(tok);
                if (havePending) {          // key + value pair: both consumed
                    havePending = false;
                } else {
                    pending     = tok;
                    havePending = true;
                }
            } else if (c == '{') {
                ++i;
                if (path.size() >= detail::kMaxDepth) return count;
                const std::string key = havePending ? pending : std::string();
                havePending = false;
                path.push_back(key);
                if (path.size() == 3 && path[1] == "depots" && detail::AllDigits(key)) ++count;
            } else if (c == '}') {
                ++i;
                havePending = false;
                if (!path.empty()) path.pop_back();
            } else {
                ++i;                        // whitespace, comments and stray bytes
            }
        }
        return count;
    }

}  // namespace PicsDiagnosticsLogic
