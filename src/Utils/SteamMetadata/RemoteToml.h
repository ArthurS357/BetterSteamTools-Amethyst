#pragma once
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

} // namespace RemoteToml
