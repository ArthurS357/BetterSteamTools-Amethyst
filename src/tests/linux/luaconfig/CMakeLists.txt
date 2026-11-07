cmake_minimum_required(VERSION 3.20)
project(LuaConfigConcurrencyTests LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# luaconfig_concurrency_test.cpp under a race detector (CI:
# .github/workflows/linux-logic-tests.yml). MSVC has no ThreadSanitizer, so this
# is the only place the LuaConfig locking is checked by a tool rather than by
# review. It is its own project because TSan and ASan cannot share a binary, and
# Lua is built from source here so TSan sees races inside the interpreter too:
# an uninstrumented liblua would hide exactly the bug this exists to catch.
#
# Usage:
#   cmake -S src/tests/linux/luaconfig -B build-luaconfig -G Ninja -DLUACONFIG_SANITIZE=thread
#   cmake --build build-luaconfig && build-luaconfig/LuaConfigConcurrencyTests
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LUACONFIG_SANITIZE "thread" CACHE STRING "-fsanitize= value: thread, or address,undefined")
add_compile_options(-fsanitize=${LUACONFIG_SANITIZE} -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=${LUACONFIG_SANITIZE})

get_filename_component(_src "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)   # <repo>/src

list(APPEND CMAKE_MODULE_PATH "${_src}/cmake")
include(Lua)   # same pinned release as the DLL; the add_compile_options above apply

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(LuaConfigConcurrencyTests
    "${_src}/tests/luaconfig_concurrency_test.cpp"
    "${_src}/Utils/Config/LuaConfig.cpp"
    "${_src}/OSTPlatform/Numbers.cpp"
)

# ./shim first so "dllmain.h" resolves to the stand-in, then ../shim for Encoding.h.
target_include_directories(LuaConfigConcurrencyTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shim"
    "${_src}"
)
target_link_libraries(LuaConfigConcurrencyTests PRIVATE
    GTest::gtest_main Threads::Threads lua_static m)

# Without this UBSan only prints and the job stays green. On our sources only: UB
# inside the vendored interpreter is not ours to fix and must not fail this job.
if(LUACONFIG_SANITIZE MATCHES "undefined")
    target_compile_options(LuaConfigConcurrencyTests PRIVATE -fno-sanitize-recover=undefined)
endif()
