cmake_minimum_required(VERSION 3.28)

project(ScrubTub
    VERSION 0.1.0
    DESCRIPTION "Video catalogue for Windows and Linux"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SCRUBTUB_BUILD_TESTS "Build tests" ON)
option(SCRUBTUB_SANITIZERS "Build with AddressSanitizer/UBSan (GCC/Clang)" OFF)

# --- Dependencies -----------------------------------------------------------

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Test)
qt_standard_project_setup(REQUIRES 6.8)

find_package(SQLite3 REQUIRED)

# RapidFuzz: prefer the system package; fall back to a pinned FetchContent copy
# so a clean checkout still configures. Pinned per docs/DEPENDENCIES.md.
find_package(rapidfuzz 3.3.4 QUIET)
if(NOT rapidfuzz_FOUND)
    message(STATUS "rapidfuzz >= 3.3.4 not found locally; fetching pinned copy")
    include(FetchContent)
    FetchContent_Declare(rapidfuzz
        URL https://github.com/rapidfuzz/rapidfuzz-cpp/archive/refs/tags/v3.3.4.tar.gz
        URL_HASH SHA256=a0dd2ef361cac165e12076696e7c7e8d069a2908abd9599ad4bd190de33f9881)
    FetchContent_MakeAvailable(rapidfuzz)
endif()

# --- Global compile settings -------------------------------------------------

add_library(scrubtub_warnings INTERFACE)
if(MSVC)
    target_compile_options(scrubtub_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(scrubtub_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(SCRUBTUB_SANITIZERS AND NOT MSVC)
    add_library(scrubtub_sanitizers INTERFACE)
    target_compile_options(scrubtub_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(scrubtub_sanitizers INTERFACE -fsanitize=address,undefined)
    message(STATUS "Sanitizers enabled (ASan/UBSan)")
endif()

add_library(scrubtub_defaults INTERFACE)
target_link_libraries(scrubtub_defaults INTERFACE scrubtub_warnings)
if(TARGET scrubtub_sanitizers)
    target_link_libraries(scrubtub_defaults INTERFACE scrubtub_sanitizers)
endif()

# --- Targets -----------------------------------------------------------------

add_subdirectory(src)
if(SCRUBTUB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
