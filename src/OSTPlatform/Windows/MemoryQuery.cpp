#include "include/Memory.h"

#include "include/Log.h"

#include <windows.h>

#include <cstdint>

// Ported from upstream BetterSteamTools 1d15f39 (Memory.cpp there). Split into
// its own TU here -- see the note on IsReadable in include/Memory.h.

namespace OSTPlatform::Memory {

namespace {

    // PAGE_* protections that permit a read. PAGE_NOACCESS and bare
    // PAGE_EXECUTE (execute-only) are absent on purpose: neither can be read.
    bool IsReadableProtect(DWORD protect) {
        // Strip the modifier bits before comparing -- they combine with the
        // base protection rather than replacing it.
        const DWORD base = protect & ~static_cast<DWORD>(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
        switch (base) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

} // namespace

bool IsReadable(const void* addr, size_t bytes) {
    if (!addr || bytes == 0) return false;

    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (start > UINTPTR_MAX - bytes) return false;   // wraps
    const uintptr_t end = start + bytes;

    // The cursor stays a pointer (advanced by region sizes) so VirtualQuery
    // never receives an integer-to-pointer cast; comparisons use its address.
    const auto* cursor = static_cast<const uint8_t*>(addr);
    for (uintptr_t at = start; at < end; at = reinterpret_cast<uintptr_t>(cursor)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            OSTP_LOG_TRACE("IsReadable({}, {}): VirtualQuery failed at 0x{:X} (error={})",
                           addr, bytes, at, GetLastError());
            return false;
        }
        // A guard page faults once on first touch, so reading it is not safe
        // even though its base protection says otherwise.
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !IsReadableProtect(mbi.Protect)) {
            OSTP_LOG_TRACE("IsReadable({}, {}): 0x{:X} not readable (state=0x{:X} protect=0x{:X})",
                           addr, bytes, at, mbi.State, mbi.Protect);
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= at) return false;   // no forward progress; refuse to spin
        cursor += regionEnd - at;
    }
    return true;
}

std::optional<ModuleImage> GetModuleSection(DynamicLibrary::ModuleHandle module, std::string_view name) {
    if (!module || name.empty() || name.size() > IMAGE_SIZEOF_SHORT_NAME) return std::nullopt;

    auto* const base = static_cast<uint8_t*>(module);
    if (!IsReadable(base, sizeof(IMAGE_DOS_HEADER))) return std::nullopt;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return std::nullopt;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadable(nt, sizeof(IMAGE_NT_HEADERS)) || nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;
    // IMAGE_NT_HEADERS is the layout of THIS build's architecture (x64 PE32+). The
    // section table's position depends on SizeOfOptionalHeader, but SizeOfImage is read
    // from the optional header below, so refuse an image that is not that layout.
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return std::nullopt;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const size_t count = nt->FileHeader.NumberOfSections;
    if (!IsReadable(sections, count * sizeof(IMAGE_SECTION_HEADER))) return std::nullopt;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    for (size_t i = 0; i < count; ++i) {
        const IMAGE_SECTION_HEADER& s = sections[i];
        // The 8-byte name is not NUL-terminated when it is exactly 8 characters.
        const std::string_view padded(reinterpret_cast<const char*>(s.Name), IMAGE_SIZEOF_SHORT_NAME);
        if (padded.substr(0, padded.find('\0')) != name) continue;

        const size_t offset = s.VirtualAddress;
        const size_t size = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
        if (size == 0 || offset > imageSize || size > imageSize - offset) return std::nullopt;
        if (!IsReadable(base + offset, size)) return std::nullopt;
        return ModuleImage{ base + offset, size };
    }
    return std::nullopt;
}

} // namespace OSTPlatform::Memory
