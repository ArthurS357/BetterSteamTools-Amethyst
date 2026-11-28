#pragma once

#include "Utils/SteamMetadata/PatternMigrateLogic.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Disk side of the shadow-mode recorder (v1.5.0, pillar 4). Reads a Steam DLL FROM DISK
// (why: PatternRecorderLogic.h), fingerprints the functions the hooks resolved, and
// returns the text of the fingerprint file. No logging, no Config, no Steam, no locks:
// the worker in PatternLoader.cpp owns those, and this unit can be tested on its own.
namespace PatternRecorder {

    // The whole DLL, read once, and only if its SHA-256 is still `sha256` (lower-case hex).
    // nullopt, with a reason in *error, when it cannot be read, is outside the size cap, or
    // was replaced since it was hashed. Shared by the recorder and the runtime migration.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadDllVerified(
        const std::filesystem::path& dll, const std::string& sha256, std::string* error);

    // `resolved` is (name, rva) of every function the hooks resolved in this DLL.
    // nullopt, with a reason in *error, when the DLL cannot be read or parsed, the
    // component/sha256 are not safe path segments, or nothing could be fingerprinted.
    [[nodiscard]] std::optional<std::string> BuildFingerprintFile(
        const std::filesystem::path& dll, const std::string& component, const std::string& sha256,
        const std::vector<PatternMigrateLogic::NamedRva>& resolved, std::string* error);

    // Puts the CALLING thread in the background mode (lower CPU and I/O priority) so a
    // recording running next to Steam's startup does not compete with it. Windows only;
    // a no-op elsewhere. Failure is ignored: it only makes the thread a bit less polite.
    void UseBackgroundPriority() noexcept;

} // namespace PatternRecorder
