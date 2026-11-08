#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Compiled-in copy of the IPC method metadata that IPCLoader normally reads from
// the steam-monitor `ipc` TOML (v1.5.0). Header-only and free of Windows, Config
// and TOML so the table and its verification are unit-testable on their own.
//
// Why it is safe to embed: funcHash, fencepost and argc were identical across the
// three steamclient64.dll builds cached on the audit machine (2026-09-05 to
// 2026-10-23). What does change from build to build is wrapper_rva/vtable_rva,
// and the runtime reads neither. "Identical across 3 builds" is evidence, not a
// guarantee, so the table is never trusted blindly: Verify() must find every
// constant in the loaded steamclient `.text` before IPCLoader uses it. If Valve
// changes one, verification fails and the loader falls through to the mirrors,
// which is the pre-v1.5.0 behaviour.
//
// Scope: only the methods the functional handlers need (Hooks_IPC_ISteamUser.cpp,
// Hooks_IPC_ISteamUtils.cpp). The Debug-only IClientUGC diagnostics have no row
// on purpose -- they stay disabled unless a TOML lists them.
//
// Adding a handler means adding its row here; ipc_embedded_test.cpp fails
// otherwise. A row's constants come from a published `ipc` TOML for a build
// whose funcHash/fencepost you have checked with Verify().
namespace IPCEmbedded {

    struct Entry {
        std::string_view interfaceName;   // an `interface` in Steam/IPCMessages.steamd
        std::string_view methodName;      // a method of that interface
        std::uint32_t    funcHash;
        std::uint32_t    fencepost;
        std::uint32_t    argc;
    };

    inline constexpr std::array<Entry, 6> kEntries = {{
        { "IClientUser",  "GetSteamID",                        0xD6FC3200u, 0xD7058CA5u, 0 },
        { "IClientUser",  "GetAppOwnershipTicketExtendedData", 0xC7E71245u, 0xC8449840u, 2 },
        { "IClientUser",  "RequestEncryptedAppTicket",         0x25D6BB1Du, 0x2646B663u, 2 },
        { "IClientUser",  "GetEncryptedAppTicket",             0xE0468CB4u, 0xE0B80200u, 1 },
        { "IClientUtils", "GetAppID",                          0x09607EC4u, 0x0AFE7552u, 0 },
        { "IClientUtils", "GetAPICallResult",                  0x2D3D3947u, 0x2EDF5EE6u, 3 },
    }};

    // True when `value` occurs in `haystack` as the 4 little-endian bytes of an
    // x86-64 imm32/disp32, at any offset (instruction immediates are unaligned).
    [[nodiscard]] inline bool ContainsU32(std::span<const std::uint8_t> haystack,
                                          std::uint32_t value) noexcept
    {
        const std::array<std::uint8_t, 4> needle = {
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 24),
        };
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
    }

    struct Verification {
        std::size_t  checked = 0;                   // constants looked up (2 per entry)
        std::size_t  missing = 0;                   // of those, not found
        const Entry* firstMissing = nullptr;        // first entry with a missing constant
        const char*  firstMissingField = nullptr;   // "funcHash" or "fencepost"

        // An empty table verifies nothing, so it must not count as success.
        [[nodiscard]] bool ok() const noexcept { return checked != 0 && missing == 0; }
    };

    // Looks up every funcHash and fencepost of `entries` in `text` (the loaded
    // steamclient `.text` section). Presence only: it proves the constants belong
    // to this build, not that each is paired with the right method.
    [[nodiscard]] inline Verification Verify(std::span<const std::uint8_t> text,
                                             std::span<const Entry> entries = kEntries) noexcept
    {
        Verification v;
        for (const Entry& e : entries) {
            const struct { std::uint32_t value; const char* field; } constants[] = {
                { e.funcHash,  "funcHash"  },
                { e.fencepost, "fencepost" },
            };
            for (const auto& c : constants) {
                ++v.checked;
                if (ContainsU32(text, c.value)) continue;
                ++v.missing;
                if (!v.firstMissing) {
                    v.firstMissing = &e;
                    v.firstMissingField = c.field;
                }
            }
        }
        return v;
    }

} // namespace IPCEmbedded
