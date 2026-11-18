#pragma once

// Shared by the pattern_* tests (v1.5.0, pillar 4): hand-assembled "functions", a tiny
// .text/.pdata image builder and a synthetic PE32+ file around it. Nothing here touches
// Windows or a real Steam DLL.

#include "Utils/SteamMetadata/PatternMigrateLogic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SyntheticImage {

    using Bytes = std::vector<std::uint8_t>;

    inline constexpr std::uint32_t kTextRva = 0x1000;

    inline void Put16(Bytes& b, std::size_t off, std::uint32_t v) {
        b[off] = static_cast<std::uint8_t>(v);
        b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    }

    inline void Put32(Bytes& b, std::size_t off, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i)
            b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }

    // ── a synthetic function ─────────────────────────────────────────
    //
    //   0  48 89 5C 24 08        mov [rsp+8], rbx
    //   5  48 89 74 24 10        mov [rsp+10h], rsi
    //  10  57                    push rdi
    //  11  48 83 EC 20           sub rsp, 20h
    //  15  B8 <seed>             mov eax, seed          (imm32 at 16, fixed)
    //  20  E8 <rel32>            call ...               (rel32 at 21, wildcard)
    //  25  48 8B 05 <disp32>     mov rax, [rip+disp]    (disp32 at 28, wildcard)
    //  32  85 C0                 test eax, eax
    //  34  74 02                 jz +2
    //  36  90 x30                nop padding            -> head = 66 bytes
    //  66  90 x(size-67), C3     tail                   -> size = 66 + tail + 1
    //
    // The first 64 bytes are the fingerprint prefix; 8 of them are wildcards.
    inline constexpr std::size_t kCallOff = 20;
    inline constexpr std::size_t kRipDispOff = 28;
    inline constexpr std::size_t kHeadLen = 66;

    inline Bytes Func(std::uint32_t seed, std::size_t size) {
        Bytes b = { 0x48, 0x89, 0x5C, 0x24, 0x08,
                    0x48, 0x89, 0x74, 0x24, 0x10,
                    0x57,
                    0x48, 0x83, 0xEC, 0x20,
                    0xB8, 0, 0, 0, 0,
                    0xE8, 0, 0, 0, 0,
                    0x48, 0x8B, 0x05, 0, 0, 0, 0,
                    0x85, 0xC0,
                    0x74, 0x02 };
        Put32(b, 16, seed);
        b.resize(size, 0x90);
        b[size - 1] = 0xC3;
        return b;
    }

    struct Img {
        Bytes text;
        std::vector<PatternMigrateLogic::RuntimeFunction> pdata;

        // Places `code` at the next 16-byte boundary (int3 padding between functions)
        // and, unless told otherwise, gives it a .pdata entry covering exactly it.
        std::uint32_t Add(const Bytes& code, bool inPdata = true) {
            while (text.size() % 16 != 0) text.push_back(0xCC);
            const std::uint32_t rva = kTextRva + static_cast<std::uint32_t>(text.size());
            text.insert(text.end(), code.begin(), code.end());
            if (inPdata)
                pdata.push_back({ rva, rva + static_cast<std::uint32_t>(code.size()) });
            return rva;
        }

        void PatchCall(std::uint32_t funcRva, std::uint32_t target) {
            const std::size_t off = funcRva - kTextRva + kCallOff + 1;
            Put32(text, off, target - (funcRva + static_cast<std::uint32_t>(kCallOff) + 5));
        }

        void PatchRip(std::uint32_t funcRva, std::uint32_t disp) {
            Put32(text, funcRva - kTextRva + kRipDispOff, disp);
        }

        PatternMigrateLogic::ImageView View() const {
            PatternMigrateLogic::ImageView v;
            v.textRva = kTextRva;
            v.text = std::span<const std::uint8_t>(text);
            v.pdata = pdata;
            return v;
        }
    };

    // ── a synthetic PE32+ x64 file around an Img ─────────────────────
    //
    //   0x000  DOS header, e_lfanew = 0x80
    //   0x080  "PE\0\0", COFF header (2 sections, optional header of 240 bytes)
    //   0x098  optional header: magic 0x20B, 16 data directories, [3] = exception (.pdata)
    //   0x188  section table: .text, .pdata
    //   0x400  .text raw data, then .pdata raw data (each padded to 0x200)
    inline constexpr std::size_t kPeOffset = 0x80;
    inline constexpr std::size_t kOptOffset = kPeOffset + 24;
    inline constexpr std::size_t kOptSize = 112 + 16 * 8;
    inline constexpr std::size_t kSectionTable = kOptOffset + kOptSize;
    inline constexpr std::size_t kHeadersEnd = kSectionTable + 2 * 40;
    inline constexpr std::size_t kRawStart = 0x400;

    inline std::size_t AlignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

    inline std::uint32_t PdataRvaOf(const Img& img) {
        return kTextRva + static_cast<std::uint32_t>(AlignUp(img.text.size(), 0x1000));
    }

    inline Bytes PdataBytes(const Img& img) {
        Bytes pd(img.pdata.size() * 12, 0);
        for (std::size_t i = 0; i < img.pdata.size(); ++i) {
            Put32(pd, i * 12, img.pdata[i].begin);
            Put32(pd, i * 12 + 4, img.pdata[i].end);
            Put32(pd, i * 12 + 8, 0x5000);          // unwind info: not read
        }
        return pd;
    }

    inline void PutSection(Bytes& f, std::size_t at, const char* name, std::uint32_t vsize, std::uint32_t va,
                           std::uint32_t rawSize, std::uint32_t rawOff) {
        for (std::size_t i = 0; i < 8 && name[i] != 0; ++i) f[at + i] = static_cast<std::uint8_t>(name[i]);
        Put32(f, at + 8, vsize);
        Put32(f, at + 12, va);
        Put32(f, at + 16, rawSize);
        Put32(f, at + 20, rawOff);
    }

    inline Bytes BuildPeFile(const Img& img) {
        const Bytes pd = PdataBytes(img);
        const std::size_t textRaw = AlignUp(img.text.size(), 0x200);
        const std::size_t pdataRaw = AlignUp(pd.size(), 0x200);

        Bytes f(kRawStart + textRaw + pdataRaw, 0);
        f[0] = 'M';
        f[1] = 'Z';
        Put32(f, 0x3C, static_cast<std::uint32_t>(kPeOffset));
        Put32(f, kPeOffset, 0x00004550);                    // "PE\0\0"
        Put16(f, kPeOffset + 4, 0x8664);                    // x64
        Put16(f, kPeOffset + 6, 2);                         // sections
        Put16(f, kPeOffset + 20, static_cast<std::uint32_t>(kOptSize));
        Put16(f, kOptOffset, 0x20B);                        // PE32+
        Put32(f, kOptOffset + 108, 16);                     // data directories
        Put32(f, kOptOffset + 112 + 3 * 8, PdataRvaOf(img));
        Put32(f, kOptOffset + 112 + 3 * 8 + 4, static_cast<std::uint32_t>(pd.size()));
        PutSection(f, kSectionTable, ".text", static_cast<std::uint32_t>(img.text.size()), kTextRva,
                   static_cast<std::uint32_t>(textRaw), static_cast<std::uint32_t>(kRawStart));
        PutSection(f, kSectionTable + 40, ".pdata", static_cast<std::uint32_t>(pd.size()), PdataRvaOf(img),
                   static_cast<std::uint32_t>(pdataRaw), static_cast<std::uint32_t>(kRawStart + textRaw));
        std::copy(img.text.begin(), img.text.end(), f.begin() + static_cast<std::ptrdiff_t>(kRawStart));
        std::copy(pd.begin(), pd.end(), f.begin() + static_cast<std::ptrdiff_t>(kRawStart + textRaw));
        return f;
    }

} // namespace SyntheticImage
