cmake_minimum_required(VERSION 3.28)

project(marketmaking LANGUAGES CXX)

# ---------------------------------------------------------------------------
# language
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # no GNU extensions -- keep us on standard C++

# so clangd / IntelliSense know the exact flags for every file
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# warnings -- priority #1 is safety, and most of it lives here.
# -Wconversion is THE unit-safety flag: catches implicit narrowing and
# signed/unsigned mixing, i.e. the price-vs-qty and ns-vs-us bug class.
# ---------------------------------------------------------------------------
add_library(mm_warnings INTERFACE)
target_compile_options(mm_warnings INTERFACE
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wimplicit-fallthrough
    -Wpedantic
    # NOT -Werror yet: the known TODOs (uninitialised `side`, unused decoded
    # structs) would block the build. Turn it on once those are closed.
)

# ---------------------------------------------------------------------------
# build types
#   Debug   -O0 -g + sanitizers        -> run tests here
#   Release -O2 -g -march=native       -> measure latency here, never in Debug
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fno-omit-frame-pointer")
# TODO: missing -DNDEBUG. CMake's default Release is "-O3 -DNDEBUG"; overriding it
# dropped NDEBUG, so asserts stay LIVE in the build we measure latency in.
# TODO: -O2 is a deliberate choice over CMake's -O3 (less aggressive inlining =
# less I-cache pressure). Try both once there is something to measure.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -march=native")

option(MM_SANITIZE "ASan + UBSan in Debug builds" ON)

add_library(mm_sanitizers INTERFACE)
if(MM_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # UBSan is the valuable one here: it catches misaligned loads, which is
    # exactly what a struct-cast over an ITCH buffer would produce.
    # NOTE: LeakSanitizer is unsupported on arm64 macOS -- use
    #   leaks --atExit -- ./binary
    # for leaks. If an ASan run appears to hang, try ASAN_OPTIONS=symbolize=0.
    target_compile_options(mm_sanitizers INTERFACE -fsanitize=address,undefined)
    target_link_options(mm_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# targets
#
# types/ is header-only and must compile with every other folder deleted.
# codec/ owns all wire knowledge and depends only on types/.
# ---------------------------------------------------------------------------
add_library(mm_types INTERFACE)
target_include_directories(mm_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mm_codec STATIC
    codec/itch.cpp
)
target_include_directories(mm_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_codec PUBLIC mm_types PRIVATE mm_warnings mm_sanitizers)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(test)
