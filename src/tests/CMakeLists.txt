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
# Ticket/URI/config PARSERS (AppTicket, TokeerBridge, Config::Load) are TU-coupled
# to Lua, the hook engine, WinHTTP and MessageBox and are NOT unit-testable without
# an interface extraction — tracked as pending in the report, not forced here.
# Config::MigrateLegacyConfig is the exception: it depends only on <filesystem>, so
# it is extracted into its own TU (ConfigMigration.cpp) and covered here directly.
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

    # Units under test, compiled directly (no OSTPlatform lib link) — see header.
    "${_ostp}/Numbers.cpp"
    "${_ostp}/Encoding.cpp"
    "${_ostp}/Windows/Hash.cpp"
    # ConfigMigration depends only on <filesystem>; compiled directly so the test
    # stays free of the Lua/toml/WinHTTP coupling of the rest of Utils/Config.
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils/Config/ConfigMigration.cpp"
)

# src/ root for production-style header spellings ("Utils/Support/FnvHash.h",
# "OSTPlatform/include/Numbers.h"); OSTPlatform dir for the units' own
# "include/..." spelling (e.g. Numbers.cpp -> #include "include/Encoding.h").
target_include_directories(AmethystToolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${_ostp}
)

# BCrypt backs Hash::Sha256OfBuffer; no other platform lib is needed because the
# compiled units, with logging disabled, reference nothing from Detours/WinHTTP.
target_link_libraries(AmethystToolTests PRIVATE
    GTest::gtest_main
    GTest::gmock
    Bcrypt
)

# Register every TEST() with CTest. PRE_TEST defers enumeration to test time,
# which suits Ninja Multi-Config.
gtest_discover_tests(AmethystToolTests
    DISCOVERY_MODE PRE_TEST
)
