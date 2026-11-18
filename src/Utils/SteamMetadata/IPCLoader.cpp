#include "IPCLoader.h"
#include "IPCMessages.gen.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/IPCEmbedded.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/RemoteTomlLogic.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <unordered_map>
#include <utility>

#include <toml++/toml.hpp>

namespace IPCLoader {

namespace {

    struct Registry {
        std::vector<Interface> interfaces;
        std::unordered_map<EIPCInterface, size_t> byID;
        std::unordered_map<std::string, size_t> byName;

        void Clear()
        {
            interfaces.clear();
            byID.clear();
            byName.clear();
        }

        void Add(Interface iface)
        {
            const size_t index = interfaces.size();
            byID[iface.id] = index;
            byName[iface.name] = index;
            interfaces.push_back(std::move(iface));
        }

        const Method* Find(EIPCInterface interfaceID, uint32_t funcHash) const
        {
            const auto it = byID.find(interfaceID);
            if (it == byID.end()) return nullptr;

            for (const auto& method : interfaces[it->second].methods) {
                if (method.funcHash == funcHash) return &method;
            }
            return nullptr;
        }

        const Method* Find(std::string_view interfaceName,
                           std::string_view methodName) const
        {
            const auto it = byName.find(std::string(interfaceName));
            if (it == byName.end()) return nullptr;

            for (const auto& method : interfaces[it->second].methods) {
                if (method.name == methodName) return &method;
            }
            return nullptr;
        }

        size_t MethodCount() const
        {
            size_t count = 0;
            for (const auto& iface : interfaces)
                count += iface.methods.size();
            return count;
        }
    };

    Registry g_registry;

    // ---- TOML helpers ----

    static bool ParseHexU32(std::string_view s, uint32_t& out)
    {
        const auto parsed = OSTPlatform::Numbers::ParseHexUInt32(s);
        if (!parsed) return false;
        out = *parsed;
        return true;
    }

    static bool ParseInterfaceTable(std::string_view name,
                                    const toml::table& tbl,
                                    Interface& out)
    {
        out.name = std::string(name);

        const auto expected = EIPCInterfaceFromName(name);
        if (!expected) {
            LOG_WARN("IPCLoader: [{}] is not declared in EIPCInterface", name);
            return false;
        }
        out.id = *expected;

        // Backward-compatible metadata: EIPCInterface is authoritative.
        if (auto v = tbl["interface_id"].value<int64_t>()) {
            if (*v < 0 || *v > 0xFF) {
                LOG_WARN("IPCLoader: [{}] interface_id out of range ({})", name, *v);
                return false;
            }
            if (static_cast<EIPCInterface>(*v) != out.id) {
                LOG_WARN("IPCLoader: [{}] interface_id {} disagrees with generated EIPCInterface value {}",
                         name, *v, static_cast<uint8_t>(out.id));
                return false;
            }
        }

        if (auto v = tbl["vtable_rva"].value<std::string>())
            ParseHexU32(*v, out.vtableRva);

        // Walk dotted sub-tables — each is a method.
        for (auto& [methodKey, methodVal] : tbl) {
            if (!methodVal.is_table()) continue;
            const auto& mtbl = *methodVal.as_table();

            Method m;
            m.interfaceID = out.id;
            m.name = std::string(methodKey.str());

            if (auto v = mtbl["funcHash"].value<std::string>()) {
                if (!ParseHexU32(*v, m.funcHash)) {
                    LOG_WARN("IPCLoader: [{}.{}] bad funcHash '{}'",
                             name, m.name, *v);
                    continue;
                }
            } else {
                LOG_WARN("IPCLoader: [{}.{}] missing funcHash", name, m.name);
                continue;
            }

            if (auto v = mtbl["fencepost"].value<std::string>())
                ParseHexU32(*v, m.fencepost);
            if (auto v = mtbl["argc"].value<int64_t>())
                m.argc = static_cast<uint32_t>(*v);
            out.methods.push_back(std::move(m));
        }
        return true;
    }

    static void ShowMissingPopup(const std::string& sha256)
    {
        SteamDiagnostics::ShowWarning(
            "AmethystTool - IPC spec missing",
            "AmethystTool: IPC spec file not found.\n\n"
            "IPC interception is disabled for this session; pattern-based "
            "hooks are unaffected.\n\n"
            "You can:\n"
            "  1. Wait for the next upstream publish and restart Steam.\n"
            "  2. Drop a matching TOML at:\n"
            "       <Steam>\\amethysttool\\ipc\\steamclient\\" + sha256 + ".toml\n"
            "  3. Check upstream:\n"
            "       https://github.com/madoiscool/steam-monitor/tree/ipc/steamclient");
    }

} // namespace

constexpr const char* kIPCChannel = "ipc";

namespace {

    // Fills g_registry from a TOML body. False (registry left empty) when the body
    // does not parse or lists no usable interface, so the caller can try the next
    // source instead of ending up with a registry that disables every handler.
    bool LoadFromToml(const RemoteToml::Result& r)
    {
        toml::table root;
        try {
            root = toml::parse(r.body);
        } catch ([[maybe_unused]] const toml::parse_error& e) {
            LOG_WARN("IPCLoader: TOML parse error: {}", e.description());
            return false;
        }

        for (auto& [key, val] : root) {
            if (!val.is_table()) continue;
            Interface iface;
            if (!ParseInterfaceTable(key.str(), *val.as_table(), iface)) continue;

            g_registry.Add(std::move(iface));
        }

        if (g_registry.interfaces.empty()) {
            LOG_WARN("IPCLoader: TOML lists no usable interface");
            return false;
        }
        LOG_INFO("IPCLoader: loaded {} methods across {} interfaces ({})",
                 MethodCount(), InterfaceCount(),
                 RemoteTomlLogic::DescribeLoadSource(r.fromCache, r.cacheFallback));
        return true;
    }

    // Registers the compiled-in table (IPCEmbedded.h) -- but only after every one of
    // its constants was found in this build's .text. Any doubt returns false and
    // leaves g_registry untouched, which sends the caller on to the mirrors.
    bool LoadEmbedded(OSTPlatform::DynamicLibrary::ModuleHandle steamclient)
    {
        const auto text = OSTPlatform::Memory::GetModuleSection(steamclient, ".text");
        if (!text) {
            LOG_WARN("IPCLoader: embedded IPC spec not used: no readable .text section in steamclient");
            return false;
        }

        const IPCEmbedded::Verification check = IPCEmbedded::Verify(
            std::span<const uint8_t>(text->base, text->size));
        if (!check.ok()) {
            LOG_WARN("IPCLoader: embedded IPC spec does not match this steamclient build "
                     "({} of {} constants not found, first: {}::{}.{}); trying the mirrors",
                     check.missing, check.checked,
                     check.firstMissing ? check.firstMissing->interfaceName : std::string_view{},
                     check.firstMissing ? check.firstMissing->methodName : std::string_view{},
                     check.firstMissingField ? check.firstMissingField : "?");
            return false;
        }

        std::vector<Interface> interfaces;
        for (const IPCEmbedded::Entry& e : IPCEmbedded::kEntries) {
            auto it = std::ranges::find(interfaces, e.interfaceName, &Interface::name);
            if (it == interfaces.end()) {
                const auto id = EIPCInterfaceFromName(e.interfaceName);
                if (!id) {
                    LOG_WARN("IPCLoader: embedded IPC spec names {}, which is not declared in EIPCInterface",
                             e.interfaceName);
                    return false;
                }
                Interface iface;
                iface.id = *id;
                iface.name = std::string(e.interfaceName);
                interfaces.push_back(std::move(iface));
                it = std::prev(interfaces.end());
            }

            Method m;
            m.interfaceID = it->id;
            m.name = std::string(e.methodName);
            m.funcHash = e.funcHash;
            m.fencepost = e.fencepost;
            m.argc = e.argc;
            it->methods.push_back(std::move(m));
        }

        for (Interface& iface : interfaces)
            g_registry.Add(std::move(iface));

        LOG_INFO("IPCLoader: loaded {} methods across {} interfaces (embedded, verified against steamclient .text)",
                 MethodCount(), InterfaceCount());
        return true;
    }

} // namespace

bool Load(const std::string& steamclientPath, OSTPlatform::DynamicLibrary::ModuleHandle steamclient)
{
    g_registry.Clear();

    const RemoteToml::Request request{ kIPCChannel, "steamclient", steamclientPath };

    // 1. Cache for this exact build.
    const RemoteToml::Result cached = RemoteToml::LoadCached(request);
    if (cached.ok) {
        if (LoadFromToml(cached)) return true;
        g_registry.Clear();
        LOG_WARN("IPCLoader: cached spec for {} is unusable; trying the embedded table", cached.sha256);
    }

    // 2. Compiled-in table. Needs no SHA, so it also covers a DLL that could not be hashed.
    if (LoadEmbedded(steamclient)) return true;
    g_registry.Clear();

    // 3. Mirrors. They also rewrite the cache, which replaces an unusable cached copy.
    if (cached.sha256.empty()) {
        ShowMissingPopup("(hash failed)");
        return false;
    }
    const RemoteToml::Result fetched = RemoteToml::FetchOnMiss(request, cached.sha256);
    if (fetched.ok && LoadFromToml(fetched)) return true;

    ShowMissingPopup(cached.sha256);
    return false;
}

const Method* Find(EIPCInterface interfaceID, uint32_t funcHash)
{
    return g_registry.Find(interfaceID, funcHash);
}

const Method* Find(std::string_view ifaceName, std::string_view methodName)
{
    return g_registry.Find(ifaceName, methodName);
}

size_t InterfaceCount() 
{ 
    return g_registry.interfaces.size();
}

size_t MethodCount() {
    return g_registry.MethodCount();
}

} // namespace IPCLoader
