#pragma once

#include "include/DynamicLibrary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace OSTPlatform::Memory {

    struct ModuleImage {
        uint8_t* base = nullptr;
        size_t size = 0;
    };

    std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module);
    bool WriteExecutableByte(void* target, uint8_t value);

    // True when [addr, addr + bytes) is entirely committed and readable in this
    // process. For probing a pointer of unknown provenance before dereferencing
    // it -- a false return is an ordinary answer, not an error, so this never
    // logs above trace.
    //
    // Walks every region the range spans: a single VirtualQuery only describes
    // the region containing addr, so a range straddling into an uncommitted or
    // guard neighbour would otherwise look readable.
    //
    // Inherently TOCTOU -- another thread can unmap the range immediately after
    // this returns. Only use it where that race is acceptable (a probe against
    // memory that is live for the duration of the call).
    //
    // Lives in Windows/MemoryQuery.cpp, not Memory.cpp, so the unit-test binary
    // can compile it without WriteExecutableByte's RWX patching path.
    bool IsReadable(const void* addr, size_t bytes);

    // The in-memory range of one PE section of a loaded module (e.g. ".text"),
    // from the section table in its headers. nullopt when the module is null, the
    // headers are not a readable PE image, the section does not exist, or its
    // range falls outside the image or is not readable -- all ordinary answers.
    // `name` is at most 8 characters (IMAGE_SIZEOF_SHORT_NAME).
    //
    // Same TU as IsReadable (Windows/MemoryQuery.cpp) and for the same reason: the
    // unit-test binary can compile it, and test it against its own image.
    std::optional<ModuleImage> GetModuleSection(DynamicLibrary::ModuleHandle module, std::string_view name);

} // namespace OSTPlatform::Memory
