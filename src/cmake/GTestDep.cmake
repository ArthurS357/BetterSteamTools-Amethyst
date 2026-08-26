# Fetches GoogleTest + GMock and exposes GTest::gtest_main / GTest::gmock.
# Named GTestDep (not "GoogleTest") on purpose: CMake ships a builtin module
# called GoogleTest that provides gtest_discover_tests(), and this file must not
# shadow it. Idempotent.
if(TARGET GTest::gtest_main)
    return()
endif()

include(FetchCache)
include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0   # latest stable at time of writing; pinned, not a branch
)

# CRT MUST match the project (static /MT — see CMakeLists.txt and cpp-build-config).
# gtest_force_shared_crt OFF keeps GoogleTest on the static runtime; ON would force
# /MD and mix runtimes in one binary (duplicate symbols / two heaps). The project
# also sets CMAKE_MSVC_RUNTIME_LIBRARY globally under CMP0091, which GoogleTest
# honours; OFF here is the belt-and-suspenders that stops a stray /MD.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)
