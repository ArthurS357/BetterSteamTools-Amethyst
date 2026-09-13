# ---------------------------------------------------------------------------
# AmethystToolTests — GoogleTest unit suite (A-3).
#
# Scope is deliberately narrow: PURE functions with a clean, dependency-light
# surface that can be tested without refactoring production code — the
# OSTPlatform parsing/encoding/hash utilities plus the header-only FNV hash.
#
# The three OSTPlatform sources under test are compiled DIRECTLY into the test
# binary (a technique cpp-testing endorses) instead of linking the OSTPlatform
# static library. Two reasons:
#   1. Isolation — the unit under test is pulled in without the whole platform
#      layer (Detours, remote-process, injection primitives) riding along.
#   2. It keeps the hooking/injection object code OUT of the test executable, so
#      an on-access AV heuristic doesn't quarantine the freshly linked binary
#      (observed with Kaspersky quarantining the Debug exe when the full
#      OSTPlatform lib — Detours included — was linked in).
#
# OPENSTEAMTOOL_LOGGING_ENABLED is intentionally NOT defined here: it makes
# OSTP_LOG_* no-ops (see Log.h), which drops the Log/spdlog dependency entirely.
# Logging is diagnostic only and does not affect the parse/encode/hash results
# these tests assert, so behaviour under test is identical.
#
# Config::MigrateLegacyConfig, AppTicket::ExtractSteamIdFromTicketBytes, the
# TokeerBridge hex/JSON/URI helpers, and the online-fix AppID-flip decision are
# the exceptions: each was extracted into its own dependency-light TU
# (ConfigMigration.cpp, TicketParsing.cpp, TokeerParsing.cpp, OnlineFixLogic.cpp)
# specifically so it compiles here without pulling in Lua, the hook engine,
# WinHTTP, or MessageBox. The rest of AppTicket/TokeerBridge (credential-store
# I/O, HTTP requests, dialogs), Config::Load itself, and the rest of
# Hooks_Misc.cpp (VEH capture, Detours) remain TU-coupled and are not
# unit-testable without further extraction — tracked as pending in the report,
# not forced here.
#
# http_user_agent_test.cpp only includes OSTPlatform/include/Http.h (not
# Http.cpp): kDefaultUserAgent is a header-only constexpr precisely so this
# regression test needs no WinHTTP link dependency either.
#
# LicenseListLogic.cpp (CMsgClientLicenseList parsing, split out of
# Hooks_NetPacket.cpp the same way OnlineFixLogic.cpp was) is the one unit here
# that genuinely needs protobuf -- it parses a real Steam wire message. That
# pulls the generated steam_messages.pb.cc plus libprotobuf/libprotobuf-lite
# into this executable, same config split as the AmethystTool target itself.
# This is a deliberate, scoped exception to the "no heavy deps" framing above:
# protobuf is a normal, widely-used serialization library (not the
# hooking/injection surface the AV-heuristic note below is about), and testing
# a wire-format parser without constructing real wire bytes would not be a
# meaningful test.
#
# ManifestCacheLogic.cpp (the file/negative-cache/validation logic behind
# ManifestCache, split out of ManifestCache.cpp the same way) needs
# <windows.h> for MoveFileExW/GetCurrentThreadId only -- both are base
# kernel32 exports every Windows executable already links, not part of the
# WinHTTP/Detours surface this file avoids. The archive fetch itself is
# injected (ManifestCacheLogic::Fetcher), so no network call is possible from
# this test binary.
# ---------------------------------------------------------------------------

include(GTestDep)   # our recipe: GTest::gtest_main / GTest::gmock (/MT)
include(GoogleTest) # CMake builtin: gtest_discover_tests()

set(_ostp "${CMAKE_CURRENT_SOURCE_DIR}/../OSTPlatform")

add_executable(AmethystToolTests
    # Test translation units
    numbers_test.cpp
    encoding_test.cpp
    hash_test.cpp
    fnvhash_test.cpp
    config_migration_test.cpp
    ticket_parsing_test.cpp
    tokeer_parsing_test.cpp
    http_user_agent_test.cpp
    online_fix_logic_test.cpp
    license_list_logic_test.cpp
    manifest_cache_logic_test.cpp

    # Units under test, compiled directly (no OSTPlatform lib link) — see header.
    "${_ostp}/Numbers.cpp"
    "${_ostp}/Encoding.cpp"
    "${_ostp}/Windows/Hash.cpp"
    # ConfigMigration depends only on <filesystem>; compiled directly so the test
    # stays free of the Lua/toml/WinHTTP coupling of the rest of Utils/Config.
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils/Config/ConfigMigration.cpp"
    # Pure parsing logic split out of AppTicket.cpp / TokeerBridge.cpp precisely
    # so it compiles here without Hooks_Decryption/LuaConfig/Http/Dialog.
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils/Tickets/TicketParsing.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils/Tokeer/TokeerParsing.cpp"
    # Online-fix AppID-flip decision split out of Hooks_Misc.cpp so it compiles
    # here without HookMacros.h's Detour/PatternLoader dependency.
    "${CMAKE_CURRENT_SOURCE_DIR}/../Hook/OnlineFixLogic.cpp"
    # CMsgClientLicenseList parsing split out of Hooks_NetPacket.cpp; needs the
    # generated protobuf message code alongside it (see header comment above).
    "${CMAKE_CURRENT_SOURCE_DIR}/../Hook/LicenseListLogic.cpp"
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
    # ManifestCache's file/negative-cache/validation logic, split out so it
    # compiles here without OSTPlatform::Http/DynamicLibrary or Config.
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils/SteamMetadata/ManifestCacheLogic.cpp"
)

# src/ root for production-style header spellings ("Utils/Support/FnvHash.h",
# "OSTPlatform/include/Numbers.h"); OSTPlatform dir for the units' own
# "include/..." spelling (e.g. Numbers.cpp -> #include "include/Encoding.h");
# PROTO_GEN_DIR for the generated steam_messages.pb.h LicenseListLogic.cpp and
# the test both include.
target_include_directories(AmethystToolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${_ostp}
    "${PROTO_GEN_DIR}"
)

# Both license_list_logic_test.cpp and this build's copy of LicenseListLogic.cpp
# only #include steam_messages.pb.h; neither is itself an OUTPUT of the proto
# custom command, so without this, their first-ever compile can race ahead of
# this config's pb.h regeneration (see GenerateProtoHeaders in ../CMakeLists.txt
# for why OBJECT_DEPENDS was not used here instead).
add_dependencies(AmethystToolTests GenerateProtoHeaders)

# BCrypt backs Hash::Sha256OfBuffer. libprotobuf(-lite) backs the generated
# steam_messages.pb.cc that LicenseListLogic.cpp depends on -- same Debug/
# Release split as the AmethystTool target, so the test binary always matches
# whichever variant (full Message vs. lite MessageLite) that config generated.
target_link_libraries(AmethystToolTests PRIVATE
    GTest::gtest_main
    GTest::gmock
    Bcrypt
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
)

# Register every TEST() with CTest. PRE_TEST defers enumeration to test time,
# which suits Ninja Multi-Config.
gtest_discover_tests(AmethystToolTests
    DISCOVERY_MODE PRE_TEST
)
