// Unit tests for IPCEmbedded.h -- the compiled-in IPC method table and the check
// that proves it matches a loaded steamclient (v1.5.0).
//
// Two kinds of test:
//   * Verify()/ContainsU32() on synthetic .text buffers, so the "every constant
//     must be present, byte for byte, in little-endian order" rule is pinned.
//   * Source cross-checks: the table must stay in step with the handlers the hook
//     code registers and with the IDL the interface/method names come from. They
//     read the real .cpp/.steamd files (AMETHYST_SRC_DIR), because the handler
//     tables live in Windows-only translation units that cannot be linked here.

#include "Utils/SteamMetadata/IPCEmbedded.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef AMETHYST_SRC_DIR
#error "AMETHYST_SRC_DIR must point at <repo>/src (set in src/tests/CMakeLists.txt)"
#endif

using IPCEmbedded::Entry;
using IPCEmbedded::kEntries;
using IPCEmbedded::Verify;

namespace {

    // Filler that cannot form any of the constants by accident (0xCC repeated).
    constexpr std::uint8_t kFill = 0xCC;

    void AppendLE(std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<std::uint8_t>(v >> shift));
    }

    // A fake .text: each constant at an odd offset (instruction immediates are
    // unaligned), separated by filler.
    std::vector<std::uint8_t> PackVector(const std::vector<std::uint32_t>& constants) {
        std::vector<std::uint8_t> text(13, kFill);
        for (std::uint32_t c : constants) {
            AppendLE(text, c);
            text.insert(text.end(), 7, kFill);
        }
        return text;
    }

    std::vector<std::uint8_t> Pack(std::initializer_list<std::uint32_t> constants) {
        return PackVector(std::vector<std::uint32_t>(constants));
    }

    std::vector<std::uint32_t> AllConstants() {
        std::vector<std::uint32_t> all;
        for (const Entry& e : kEntries) {
            all.push_back(e.funcHash);
            all.push_back(e.fencepost);
        }
        return all;
    }

    std::string ReadSource(const char* relative) {
        const std::filesystem::path path = std::filesystem::path(AMETHYST_SRC_DIR) / relative;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            ADD_FAILURE() << "cannot open " << path.string();
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    using Handler = std::pair<std::string, std::string>;   // {interface, method}

    // Every ADD_IPC_{PRE,POST,BOTH}_HANDLER(Interface, Method) not in a // comment.
    std::set<Handler> RegisteredHandlers(const std::string& source) {
        static const std::regex re(R"(ADD_IPC_(?:PRE|POST|BOTH)_HANDLER\(\s*(\w+)\s*,\s*(\w+)\s*\))");
        std::set<Handler> found;
        std::istringstream lines(source);
        for (std::string line; std::getline(lines, line);) {
            if (const auto comment = line.find("//"); comment != std::string::npos) line.resize(comment);
            for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it)
                found.emplace((*it)[1].str(), (*it)[2].str());
        }
        return found;
    }

    std::set<Handler> TableRows(std::span<const Entry> entries) {
        std::set<Handler> rows;
        for (const Entry& e : entries) rows.emplace(std::string(e.interfaceName), std::string(e.methodName));
        return rows;
    }

    std::set<Handler> Difference(const std::set<Handler>& a, const std::set<Handler>& b) {
        std::set<Handler> out;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::inserter(out, out.end()));
        return out;
    }

    // Functional handlers only. Hooks_IPC_ISteamUGC.cpp is deliberately absent: its
    // handlers are Debug-only log traces that run without a spec (see its header).
    std::set<Handler> FunctionalHandlers() {
        std::set<Handler> all = RegisteredHandlers(ReadSource("Hook/Hooks_IPC_ISteamUser.cpp"));
        const std::set<Handler> utils = RegisteredHandlers(ReadSource("Hook/Hooks_IPC_ISteamUtils.cpp"));
        all.insert(utils.begin(), utils.end());
        return all;
    }

    // The text between the braces of `interface <name> { ... }` in the IDL.
    std::string InterfaceBody(const std::string& idl, const std::string& name) {
        const std::regex head("\\binterface\\s+" + name + "\\s*\\{");
        std::smatch m;
        if (!std::regex_search(idl, m, head)) return {};
        const std::size_t start = static_cast<std::size_t>(m.position(0) + m.length(0));
        std::size_t i = start;
        for (int depth = 1; i < idl.size() && depth > 0; ++i) {
            if (idl[i] == '{') ++depth;
            else if (idl[i] == '}') --depth;
        }
        return idl.substr(start, i - start);
    }

} // namespace

// ── Verify / ContainsU32 ────────────────────────────────────────

TEST(IPCEmbeddedVerify, AcceptsATextThatHoldsEveryConstant) {
    const auto text = PackVector(AllConstants());
    const auto v = Verify(text);
    EXPECT_TRUE(v.ok());
    EXPECT_EQ(v.checked, kEntries.size() * 2);
    EXPECT_EQ(v.missing, 0u);
    EXPECT_EQ(v.firstMissing, nullptr);
}

TEST(IPCEmbeddedVerify, OneMissingConstantRejectsTheWholeTable) {
    auto constants = AllConstants();
    constants.erase(constants.begin() + 5);   // entry 2's fencepost
    const auto v = Verify(PackVector(constants));
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.missing, 1u);
    ASSERT_NE(v.firstMissing, nullptr);
    EXPECT_EQ(v.firstMissing, &kEntries[2]);
    EXPECT_STREQ(v.firstMissingField, "fencepost");
}

TEST(IPCEmbeddedVerify, ReportsTheFirstMissingEntryInTableOrder) {
    auto constants = AllConstants();
    constants.erase(constants.begin() + 6);   // entry 3's funcHash
    constants.erase(constants.begin() + 1);   // entry 0's fencepost
    const auto v = Verify(PackVector(constants));
    EXPECT_EQ(v.missing, 2u);
    EXPECT_EQ(v.firstMissing, &kEntries[0]);
    EXPECT_STREQ(v.firstMissingField, "fencepost");
}

TEST(IPCEmbeddedVerify, EmptyTextVerifiesNothing) {
    const std::vector<std::uint8_t> empty;
    const auto v = Verify(empty);
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.missing, kEntries.size() * 2);
}

TEST(IPCEmbeddedVerify, AnEmptyTableIsNeverSuccess) {
    const auto text = PackVector(AllConstants());
    const auto v = Verify(text, std::span<const Entry>{});
    EXPECT_EQ(v.checked, 0u);
    EXPECT_FALSE(v.ok());   // vacuous truth must not unlock the embedded path
}

TEST(IPCEmbeddedVerify, FindsAConstantFlushAgainstTheEndOfTheSection) {
    std::vector<std::uint8_t> text(9, kFill);
    AppendLE(text, 0x11223344u);   // last four bytes of the buffer
    EXPECT_TRUE(IPCEmbedded::ContainsU32(text, 0x11223344u));
    text.pop_back();               // one byte short of the whole constant
    EXPECT_FALSE(IPCEmbedded::ContainsU32(text, 0x11223344u));
}

TEST(IPCEmbeddedVerify, ConstantsAreLittleEndianInTheCode) {
    std::vector<std::uint8_t> text(9, kFill);
    const std::uint8_t bigEndian[] = { 0x11, 0x22, 0x33, 0x44 };
    text.insert(text.end(), std::begin(bigEndian), std::end(bigEndian));
    EXPECT_FALSE(IPCEmbedded::ContainsU32(text, 0x11223344u));
    EXPECT_TRUE(IPCEmbedded::ContainsU32(Pack({0x11223344u}), 0x11223344u));
}

TEST(IPCEmbeddedVerify, ABrokenUpConstantIsNotFound) {
    std::vector<std::uint8_t> text(9, kFill);
    text.insert(text.end(), { 0x44, 0x33 });
    text.push_back(kFill);
    text.insert(text.end(), { 0x22, 0x11 });
    EXPECT_FALSE(IPCEmbedded::ContainsU32(text, 0x11223344u));
}

TEST(IPCEmbeddedVerify, UsesACallerSuppliedTable) {
    static constexpr Entry kOne[] = { { "IClientTest", "Probe", 0x11223344u, 0x55667788u, 1 } };
    EXPECT_TRUE(Verify(Pack({0x11223344u, 0x55667788u}), kOne).ok());
    EXPECT_FALSE(Verify(Pack({0x11223344u}), kOne).ok());
}

// ── The table itself ────────────────────────────────────────────

TEST(IPCEmbeddedTable, RowsAreUniqueAndLookPlausible) {
    std::set<Handler> names;
    std::set<std::uint32_t> constants;
    for (const Entry& e : kEntries) {
        EXPECT_TRUE(names.emplace(std::string(e.interfaceName), std::string(e.methodName)).second)
            << "duplicate row " << e.interfaceName << "::" << e.methodName;
        EXPECT_NE(e.funcHash, 0u) << e.methodName;
        EXPECT_NE(e.fencepost, 0u) << e.methodName;
        EXPECT_NE(e.funcHash, e.fencepost) << e.methodName;
        EXPECT_LE(e.argc, 16u) << e.methodName;
        // A repeated constant would let one occurrence in the binary vouch for two rows.
        EXPECT_TRUE(constants.insert(e.funcHash).second) << e.methodName << " funcHash repeats";
        EXPECT_TRUE(constants.insert(e.fencepost).second) << e.methodName << " fencepost repeats";
    }
}

TEST(IPCEmbeddedTable, InterfaceAndMethodNamesExistInTheIdl) {
    const std::string idl = ReadSource("Steam/IPCMessages.steamd");
    ASSERT_FALSE(idl.empty());
    for (const Entry& e : kEntries) {
        const std::string body = InterfaceBody(idl, std::string(e.interfaceName));
        ASSERT_FALSE(body.empty()) << "interface " << e.interfaceName << " not found in IPCMessages.steamd";
        const std::regex method("\\b" + std::string(e.methodName) + "\\s*\\(");
        EXPECT_TRUE(std::regex_search(body, method))
            << e.interfaceName << "::" << e.methodName << " not found in the IDL";
    }
}

TEST(IPCEmbeddedTable, EveryFunctionalHandlerHasARow) {
    const std::set<Handler> handlers = FunctionalHandlers();
    ASSERT_FALSE(handlers.empty()) << "the handler scan matched nothing; the regex or the files moved";
    const std::set<Handler> uncovered = Difference(handlers, TableRows(kEntries));
    for (const Handler& h : uncovered)
        ADD_FAILURE() << h.first << "::" << h.second << " has a handler but no row in IPCEmbedded::kEntries"
                      << " -- it would be disabled whenever the embedded table is used";
}

TEST(IPCEmbeddedTable, EveryRowHasAFunctionalHandler) {
    const std::set<Handler> handlers = FunctionalHandlers();
    ASSERT_FALSE(handlers.empty());
    const std::set<Handler> stale = Difference(TableRows(kEntries), handlers);
    for (const Handler& h : stale)
        ADD_FAILURE() << h.first << "::" << h.second << " has a row but no handler in "
                      << "Hooks_IPC_ISteamUser.cpp / Hooks_IPC_ISteamUtils.cpp";
}

TEST(IPCEmbeddedTable, TheHandlerScanSeesAnUncoveredHandler) {
    // Proves the checks above can fail: a handler absent from the table is reported.
    const std::string source =
        "ADD_IPC_POST_HANDLER(IClientUser, GetSteamID),\n"
        "ADD_IPC_PRE_HANDLER(IClientUser, BrandNewMethod),\n"
        "// ADD_IPC_POST_HANDLER(IClientUser, CommentedOut),\n";
    const std::set<Handler> uncovered = Difference(RegisteredHandlers(source), TableRows(kEntries));
    ASSERT_EQ(uncovered.size(), 1u);
    EXPECT_EQ(uncovered.begin()->second, "BrandNewMethod");
}
