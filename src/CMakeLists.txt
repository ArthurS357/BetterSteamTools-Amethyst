cmake_minimum_required(VERSION 3.20)
project(AmethystTool VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(OPENSTEAMTOOL_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded into AmethystTool.dll")

# Allow CMAKE_MSVC_RUNTIME_LIBRARY to control runtime selection for all targets,
# including dependencies pulled in via FetchContent.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Static MSVC runtime everywhere, so the resulting DLL has no extra runtime
# dependencies. Must be set BEFORE FetchContent_MakeAvailable so the fetched
# deps (Lua, Detours, spdlog) inherit it.
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)


if(NOT WIN32)
    message(STATUS "AmethystTool: no targets on non-Windows platforms; skipping.")
    return()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenSteamToolBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/OpenSteamToolBuildInfo.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependency recipes (FetchContent-backed, cached at <repo>/.deps).
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(OPENSTEAMTOOL_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
include(Lua)
include(Spdlog)
include(Protobuf)
include(Tomlplusplus)
include(LogMacros)

# Build the host code generators (ipc_codegen) in their own binary subtree so
# their executables never land in the shipped output directory. EXCLUDE_FROM_ALL
# means only the tools actually depended on (ipc_codegen) get built here.
add_subdirectory("${OPENSTEAMTOOL_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
include("${OPENSTEAMTOOL_TOOLS_DIR}/cmake/IPCCodegen.cmake")

set(IPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd")
# Per-config output dir, mirroring the protobuf recipe below: a Multi-Config
# build invokes the generator once per config, and a single shared output file
# would ping-pong between the Release/Debug graphs and force needless rebuilds.
opensteamtool_add_ipc_codegen(IPC_GEN
    IDL "${IPC_IDL}"
    CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Protobuf code generation — one variant per config, from the same .proto:
#
#   Debug   → full Message     (protoc --cpp_out)       → links libprotobuf
#   Release → lite MessageLite (protoc --cpp_out=lite:) → links libprotobuf-lite
#
# Output lands in the per-config generated/ dir (same root as the IPC header),
# so each config owns its own files. A shared output path would ping-pong
# between the Release/Debug graphs (the command embeds the config-specific
# protoc path) and force a regen + recompile on every config switch.
# ---------------------------------------------------------------------------
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf C++ sources"
    VERBATIM
)

# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns the OS-specific backends.
# Its build is defined in its own module CMakeLists so this top-level file stays
# focused on host-level products (the DLLs) and code generation.
# ---------------------------------------------------------------------------
add_subdirectory(OSTPlatform)

# ---------------------------------------------------------------------------
# Windows host artifacts: the injected hook DLL and the two loader-hijack DLLs.
# All Windows-only by construction (PE DLL hijacking + Win32 hooking).
# ---------------------------------------------------------------------------

# AmethystTool — the hook DLL injected into Steam (always 64-bit).
add_library(AmethystTool SHARED
    dllmain.cpp

    # Shared utilities
    Utils/Tickets/AppTicket.cpp
    Utils/Tickets/LegacyCDKey.cpp
    Utils/Tickets/EticketClient.cpp
    Utils/Config/Config.cpp
    Utils/Config/ConfigMigration.cpp
    Utils/Config/ConfigFileWatcher.cpp
    Utils/Config/LuaConfig.cpp
    Utils/Config/LuaFileWatcher.cpp
    Utils/CloudRedirect/CloudRedirectHost.cpp
    Utils/HookSupport/VehCommon.cpp
    Utils/Logging/Log.cpp
    Utils/SteamMetadata/IPCLoader.cpp
    Utils/SteamMetadata/ManifestClient.cpp
    Utils/SteamMetadata/Mirror.cpp
    Utils/SteamMetadata/PatternLoader.cpp
    Utils/SteamMetadata/RemoteToml.cpp
    Utils/SteamMetadata/StatsClient.cpp
    Utils/SteamMetadata/SteamDiagnostics.cpp
    Utils/Tokeer/TokeerBridge.cpp
    Utils/Tokeer/TokeerUri.cpp
    Utils/Update/AppUpdater.cpp

    # Pipe subsystem: generic handshake registry + ProcessInspector core, plus
    # self-contained features that own their primitives.
    Pipe/PipeManager.cpp
    Pipe/ProcessInspector.cpp
    Pipe/Features/DenuvoAuth/DenuvoAuth.cpp
    Pipe/Features/DenuvoAuth/ProtectionScan.cpp
    Pipe/Features/Injection/Injection.cpp

    # Generated IPC message structs (depends on IPCMessages.steamd via add_custom_command)
    "${IPC_GEN}"

    # Per-category hook modules
    Hook/HookManager.cpp
    Hook/Hooks_CallBack.cpp
    Hook/Hooks_Decryption.cpp
    Hook/Hooks_IPC.cpp
    Hook/Hooks_IPC_ISteamUser.cpp
    Hook/Hooks_IPC_ISteamUtils.cpp
    Hook/Hooks_KeyValues.cpp
    Hook/Hooks_Manifest.cpp
    Hook/Hooks_Misc.cpp
    Hook/Hooks_NetPacket.cpp
    Hook/PendingAPICalls.cpp
    Hook/Hooks_SteamUI.cpp
    Hook/Hooks_Package.cpp

    # protobuf generated sources (full in Debug, lite in Release)
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
)

# Header search path — per-config include directory
target_include_directories(AmethystTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)

target_link_libraries(AmethystTool PRIVATE
    OSTPlatform
    lua_static
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
    Shell32
)

# Logging is compiled in only for Debug; Release reduces LOG_* to no-ops.
target_compile_definitions(AmethystTool PRIVATE
    $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
)

# ---------------------------------------------------------------------------
# Warning hardening (M-2). Strong warnings are scoped to AmethystTool only —
# the loader-hijack proxies (dwmapi, xinput1_4) and third-party FetchContent
# deps (Lua, protobuf, spdlog, toml++) keep their default level so this change
# stays a diff on our own code. /WX is intentionally NOT enabled here: the goal
# of this pass is to surface warnings, not to block the build. Turning them into
# errors is gated behind OST_WARNINGS_AS_ERRORS for a future CI opt-in.
#   /W4               high signal without /Wall's informational noise
#   /permissive-      reject MSVC-only non-conforming constructs
#   /w14062           unhandled enumerator in switch (off by default even at /W4)
#   /utf-8            source + execution charset UTF-8 (accented pt-BR strings)
#   /Zc:preprocessor  conforming preprocessor (variadic macro parity)
option(OST_WARNINGS_AS_ERRORS "Treat AmethystTool warnings as errors (/WX)" OFF)
if(MSVC)
    target_compile_options(AmethystTool PRIVATE
        /W4 /permissive- /w14062 /utf-8 /Zc:preprocessor
        # Keep /W4 pointed at OUR code: angle-bracket includes (Windows SDK,
        # toml++, spdlog, Lua, the protobuf runtime headers) are external and
        # compiled at W0. Our own headers use quoted includes and stay at W4.
        /external:anglebrackets /external:W0)
    if(OST_WARNINGS_AS_ERRORS)
        target_compile_options(AmethystTool PRIVATE /WX)
    endif()

    # The protobuf-generated translation unit is machine-emitted third-party
    # code (C4125/C4244 from protoc's output); it isn't ours to hand-edit, so it
    # compiles at W0 while the rest of the DLL stays at W4 (source COMPILE_OPTIONS
    # land after the target's /W4, so /W0 wins).
    #
    # Source-file properties are matched by path string and do NOT evaluate
    # generator expressions, so the genex $<CONFIG> path that PROTO_GEN_DIR adds
    # to the target won't match on its own. Under Ninja Multi-Config CMake keys
    # the compile off the concrete per-config paths, so those are listed too — the
    # combination is what makes it robust. Verified: the .pb.cc compile line
    # carries /W0 (not /W4) and third-party warnings drop to zero in both configs.
    set_source_files_properties(
        "${PROTO_GEN_DIR}/steam_messages.pb.cc"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Debug/steam_messages.pb.cc"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Release/steam_messages.pb.cc"
        TARGET_DIRECTORY AmethystTool
        PROPERTIES COMPILE_OPTIONS "/W0")
endif()

# Backend endpoint for on-demand eticket minting (strict Denuvo titles that bind
# their encrypted app ticket to a launch nonce). Empty by default: the feature is
# off and the DLL never makes a network request. Point your own build at your own
# backend and the resulting DLL is self-contained, no Lua config needed:
#
#   cmake -B build -DOST_ETICKET_URL="https://your-host/eticket"
#
# Left out of the source deliberately so no single deployment's backend ships
# baked into a public tree. seteticketurl() in the Lua config overrides it.
set(OST_ETICKET_URL "" CACHE STRING
    "Backend URL for on-demand eticket minting (empty disables the feature)")
if(OST_ETICKET_URL)
    target_compile_definitions(AmethystTool PRIVATE
        OST_ETICKET_URL="${OST_ETICKET_URL}")
endif()

# Base URL of the Tokeer code server used by the amethysttool:// bridge (code redemption).
# Empty by default (Amethyst fork): the amethysttool:// redeem action is inert and makes no
# network request unless you point the build at your own server.
#   cmake -B build -DOST_TOKEER_URL="https://your-host"
set(OST_TOKEER_URL "" CACHE STRING
    "Base URL for the Tokeer code server (amethysttool:// bridge); empty disables redemption")
if(OST_TOKEER_URL)
    target_compile_definitions(AmethystTool PRIVATE
        OST_TOKEER_URL="${OST_TOKEER_URL}")
endif()

# ---------------------------------------------------------------------------
# dwmapi.dll hijack — small loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(dwmapi SHARED
    dwmapi/dwmapi.cpp
)

# ---------------------------------------------------------------------------
# xinput1_4.dll hijack — secondary loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(xinput1_4 SHARED
    xinput1_4/xinput1_4.cpp
    xinput1_4/xinput1_4.def
)

# ---------------------------------------------------------------------------
# Unit tests (A-3). GoogleTest-based; fetched via FetchContent (cached at
# <repo>/.deps like the other deps). ON by default; disable with
#   cmake --preset ninja-multi -DOST_BUILD_TESTS=OFF
# to skip the fetch (e.g. a fully offline first configure). Tests link the
# OSTPlatform static lib and never touch the injected/hooking code paths.
# ---------------------------------------------------------------------------
option(OST_BUILD_TESTS "Build the AmethystTool unit test suite" ON)
if(OST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
