#include "Utils/Tokeer/TokeerParsing.h"

namespace TokeerBridge::Parsing {

namespace {

    constexpr int kHexLetterDigitOffset = 10; // 'a'/'A' represents hex digit value 10

    int HexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + kHexLetterDigitOffset;
        if (c >= 'A' && c <= 'F') return c - 'A' + kHexLetterDigitOffset;
        return -1;
    }

} // namespace

std::optional<std::vector<uint8_t>> HexToBytes(std::string_view hex) {
    if (hex.empty() || (hex.size() % 2) != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool JsonString(std::string_view body, std::string_view key, std::string& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t k = body.find(needle);
    if (k == std::string_view::npos) return false;
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t q1 = body.find('"', colon + 1);
    if (q1 == std::string_view::npos) return false;
    size_t delim = body.find_first_of(",}", colon + 1);
    if (delim != std::string_view::npos && q1 > delim) return false;  // value was null/number
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return false;
    out = std::string(body.substr(q1 + 1, q2 - q1 - 1));
    return true;
}

bool JsonTrue(std::string_view body, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t k = body.find(needle);
    if (k == std::string_view::npos) return false;
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string_view::npos) return false;
    return body.find("true", colon + 1) == body.find_first_not_of(" \t", colon + 1);
}

ParsedUri ParseUri(std::string_view rawUrl) {
    ParsedUri result;

    std::string url(rawUrl);
    size_t begin = url.find_first_not_of(" \t\r\n");
    size_t end = url.find_last_not_of(" \t\r\n");
    url = (begin == std::string::npos) ? "" : url.substr(begin, end - begin + 1);
    if (url.size() >= 2 &&
        ((url.front() == '"' && url.back() == '"') ||
         (url.front() == '\'' && url.back() == '\'')))
        url = url.substr(1, url.size() - 2);
    result.cleaned = url;

    const std::string prefix = std::string(kUriScheme) + "://";
    if (url.rfind(prefix, 0) != 0) {
        result.matchedScheme = false;
        return result;
    }
    result.matchedScheme = true;

    std::string rest = url.substr(prefix.size());
    while (!rest.empty() && (rest.back() == '/' || rest.back() == '\r' ||
                             rest.back() == '\n' || rest.back() == ' '))
        rest.pop_back();

    const size_t slash = rest.find('/');
    result.action = rest.substr(0, slash);
    result.arg = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
    return result;
}

} // namespace TokeerBridge::Parsing
