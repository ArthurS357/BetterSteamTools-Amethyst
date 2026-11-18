cmake_minimum_required(VERSION 3.20)
project(AmethystToolTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

add_executable(ipc_codegen
    ipc_codegen/ipc_codegen.cpp
)
target_compile_features(ipc_codegen PRIVATE cxx_std_20)


add_executable(extract_tickets
    extract_tickets/extract_tickets.cpp
)
target_compile_features(extract_tickets PRIVATE cxx_std_20)


# version_pin: offline game version pinning (v1.5.0). Reads appmanifest_<appid>.acf and
# writes setManifestid calls for the installed depots. No injection, no hook, no network;
# the logic is Utils/SteamMetadata/VersionPinLogic.h (unit-tested), and the atomic file
# write is the one CompatAliasLogic already has.
add_executable(version_pin
    version_pin/version_pin.cpp
    ../src/Utils/Config/CompatAliasLogic.cpp
)
target_include_directories(version_pin PRIVATE ../src)
target_compile_features(version_pin PRIVATE cxx_std_20)
if(MSVC)
    # /w14062: a switch over an enum must name every case (the same flag src/CMakeLists.txt uses).
    target_compile_options(version_pin PRIVATE /W4 /w14062 /permissive-)
endif()
