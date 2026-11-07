cmake_minimum_required(VERSION 3.20)
project(AmethystToolLinuxLogicTests LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Pure-logic unit tests on Linux (CI: .github/workflows/linux-logic-tests.yml).
#
# AmethystTool itself is Windows-only (Detours, WinHTTP, MSVC /MT), but a large
# part of its decision logic was deliberately split into dependency-light
# translation units (see src/tests/CMakeLists.txt). This project builds the
# subset of those that needs nothing but the C++ standard library + GoogleTest, so
# a push can be checked without MSVC. It does NOT replace the Windows build/tests:
#
#   built here                          not built here (and why)
#   ----------------------------------  ---------------------------------------
#   *Logic.cpp / *Logic.h units         encoding_test, hash_test      <windows.h>
#   the tests that exercise them        manifest_cache_logic_test     <windows.h>
#                                       net_packet_logic_test         protobuf + Win MemoryQuery
#                                       license_list_logic_test       generated protobuf
#                                       donate_config_logic_test      toml++ (FetchContent on Windows)
#                                       numbers_test                  links OSTPlatform Windows units
#
# Usage:
#   cmake -S src/tests/linux -B build-linux-tests -G Ninja -DLINUX_TESTS_SANITIZE=ON
#   cmake --build build-linux-tests && build-linux-tests/LinuxLogicTests
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LINUX_TESTS_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)

get_filename_component(_src   "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)   # <repo>/src
get_filename_component(_tests "${CMAKE_CURRENT_SOURCE_DIR}/.."    ABSOLUTE)   # <repo>/src/tests

# --- GoogleTest ------------------------------------------------------------
find_package(GTest QUIET)
if(NOT GTest_FOUND AND EXISTS "/usr/src/googletest/CMakeLists.txt")
    # Older distros ship only the sources (libgtest-dev without prebuilt libs).
    add_subdirectory(/usr/src/googletest "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
    if(NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
elseif(NOT GTest_FOUND)
    message(FATAL_ERROR "GoogleTest not found. On Debian/Ubuntu: sudo apt-get install libgtest-dev")
endif()

# --- Tests and the logic units they exercise -------------------------------
# Every file below was verified to build and pass on Linux. Add a new pure-logic
# test here when it only needs <cstdint>/<filesystem>/... -- if it needs
# <windows.h>, leave it to the Windows suite in src/tests/CMakeLists.txt.
set(LOGIC_TESTS
    # new in v1.3.5 (-Werror, see below)
    achievements_logic_test.cpp
    rich_presence_logic_test.cpp
    compat_alias_logic_test.cpp
    # new in v1.4.0 (-Werror, see below)
    ugc_logic_test.cpp
    package_reconcile_logic_test.cpp
    # header-only logic
    pattern_refresh_logic_test.cpp
    pattern_loader_logic_test.cpp
    remote_toml_logic_test.cpp
    remote_toml_load_source_test.cpp
    forge_base_cache_test.cpp
    manifest_hook_logic_test.cpp
    manifest_probe_logic_test.cpp
    fnvhash_test.cpp
    http_user_agent_test.cpp
    # logic with a .cpp unit
    config_migration_test.cpp
    online_fix_logic_test.cpp
    local_achievements_logic_test.cpp
    manifest_url_logic_test.cpp
    manifest_donor_logic_test.cpp
    ticket_parsing_test.cpp
    tokeer_parsing_test.cpp
)

set(LOGIC_UNITS
    Hook/AchievementsLogic.cpp
    Hook/RichPresenceLogic.cpp
    Hook/OnlineFixLogic.cpp
    Utils/SteamMetadata/UgcLogic.cpp
    Utils/Config/CompatAliasLogic.cpp
    Utils/Config/ConfigMigration.cpp
    Utils/Achievements/LocalAchievementsLogic.cpp
    Utils/SteamMetadata/ManifestUrlLogic.cpp
    Utils/SteamMetadata/ManifestDonorLogic.cpp
    Utils/Tickets/TicketParsing.cpp
    Utils/Tokeer/TokeerParsing.cpp
)

set(_test_files "")
foreach(t IN LISTS LOGIC_TESTS)
    list(APPEND _test_files "${_tests}/${t}")
endforeach()
set(_unit_files "")
foreach(u IN LISTS LOGIC_UNITS)
    list(APPEND _unit_files "${_src}/${u}")
endforeach()

add_executable(LinuxLogicTests ${_test_files} ${_unit_files})

# The shim dir comes FIRST so "OSTPlatform/include/Encoding.h" resolves to the
# Linux stand-in; every other header resolves against <repo>/src as on Windows.
target_include_directories(LinuxLogicTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    "${_src}"
)
find_package(Threads REQUIRED)
target_link_libraries(LinuxLogicTests PRIVATE GTest::gtest_main Threads::Threads)

# -Wall -Wextra everywhere. -Werror only for the files written for this suite:
# the older tests were authored against MSVC and may trip GCC-only warnings that
# say nothing about the logic under test.
target_compile_options(LinuxLogicTests PRIVATE -Wall -Wextra)
foreach(f IN ITEMS
        "${_tests}/achievements_logic_test.cpp"
        "${_tests}/rich_presence_logic_test.cpp"
        "${_tests}/compat_alias_logic_test.cpp"
        "${_tests}/ugc_logic_test.cpp"
        "${_tests}/package_reconcile_logic_test.cpp"
        "${_src}/Hook/AchievementsLogic.cpp"
        "${_src}/Hook/RichPresenceLogic.cpp"
        "${_src}/Utils/Config/CompatAliasLogic.cpp"
        "${_src}/Utils/SteamMetadata/UgcLogic.cpp")
    set_source_files_properties("${f}" PROPERTIES COMPILE_OPTIONS "-Werror")
endforeach()

if(LINUX_TESTS_SANITIZE)
    target_compile_options(LinuxLogicTests PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(LinuxLogicTests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME LinuxLogicTests COMMAND LinuxLogicTests)
