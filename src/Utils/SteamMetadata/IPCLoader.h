#pragma once
#include "IPCMessages.gen.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace IPCLoader {

    struct Method {
        EIPCInterface interfaceID = static_cast<EIPCInterface>(0);
        std::string   name;
        uint32_t      funcHash  = 0;
        uint32_t      fencepost = 0;
        uint32_t      argc      = 0;
    };

    struct Interface {
        EIPCInterface       id = static_cast<EIPCInterface>(0);
        std::string         name;
        uint32_t            vtableRva = 0;
        std::vector<Method> methods;
    };

    // Load the IPC method metadata before installing IPC hooks. Sources, in order:
    //   1. the cached TOML for this steamclient's exact SHA-256;
    //   2. the table compiled into the DLL (IPCEmbedded.h), but only if every one of
    //      its constants is found in `steamclient`'s .text -- no request is made;
    //   3. the mirror chain (RemoteToml::FetchOnMiss).
    // `steamclient` is the loaded module behind `steamclientPath`. Returns false
    // (after a popup) only when none of the three produced a usable spec.
    bool Load(const std::string& steamclientPath, OSTPlatform::DynamicLibrary::ModuleHandle steamclient);

    // Return nullptr when metadata is unavailable for a method.
    const Method* Find(EIPCInterface interfaceID, uint32_t funcHash);
    const Method* Find(std::string_view ifaceName, std::string_view methodName);

    size_t InterfaceCount();
    size_t MethodCount();

} // namespace IPCLoader
