#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        bool        cacheFallback = false; // true only when the network failed and the cache was the plan B
        std::string body;
        std::string sha256;
    };

    // Read from local cache first; fetch remote only if missing.
    Result Fetch(const Request& request);

    // <Steam>\amethysttool\<channel>\<component>\<sha256>.toml for this request.
    std::filesystem::path CachePathFor(const Request& request, const std::string& sha256);

    struct RemoteBody {
        bool        ok = false;
        std::string body;
        std::string url;   // mirror that answered (or the last one tried)
    };

    // Walks the mirror chain for (request, sha256) WITHOUT reading or writing the
    // cache, with caller-chosen (short) timeouts. Used by PatternLoader's
    // background revalidation; never call it on Steam's loader path.
    // requireHttps=true skips (and logs) every mirror URL that is not https://.
    // [[nodiscard]]: the body is the whole point -- dropping it would silently
    // turn a revalidation / corrupt-cache refetch into a wasted network request.
    [[nodiscard]] RemoteBody FetchRemoteOnly(const Request& request, const std::string& sha256,
                                             uint32_t timeoutMs, uint32_t recvTimeoutMs,
                                             bool requireHttps = false);

    // Replaces cachePath atomically (temp file + MoveFileExW REPLACE_EXISTING |
    // WRITE_THROUGH). On failure the previous file is left untouched and false is
    // returned.
    [[nodiscard]] bool ReplaceCacheFile(const std::filesystem::path& cachePath, const std::string& body);

} // namespace RemoteToml
