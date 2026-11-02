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
# -DNDEBUG is what makes assert() compile to nothing. CMake's default Release is
# "-O3 -DNDEBUG"; overriding the variable means we have to put NDEBUG back ourselves,
# or every assert stays live in the build we measure latency in.
# TODO: -O2 is a deliberate choice over CMake's -O3 (less aggressive inlining =
# less I-cache pressure). Try both once there is something to measure.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -march=native -DNDEBUG")

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
# One library per layer, and the link lines ARE the dependency rules:
#   types/     header-only, depends on nothing, must compile with all else deleted
#   codec/     all wire knowledge. depends only on types/.
#   transport/ sockets and bytes. depends only on types/ -- NOT on codec/, which is
#              what keeps receive() swappable for a kernel-bypass implementation.
#   app/       the only thing that depends on everything. owns the event loop.
# ---------------------------------------------------------------------------
add_library(mm_types INTERFACE)
target_include_directories(mm_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mm_codec STATIC
    codec/itch.cpp
)
target_include_directories(mm_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_codec PUBLIC mm_types PRIVATE mm_warnings mm_sanitizers)

# Deliberately does NOT link mm_codec. If this ever needs to, the layering broke.
add_library(mm_transport STATIC
    transport/udp_receiver.cpp
)
target_include_directories(mm_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_transport PUBLIC mm_types PRIVATE mm_warnings mm_sanitizers)

add_executable(trading_system
    app/main.cpp
)
target_link_libraries(trading_system PRIVATE mm_transport mm_codec mm_warnings mm_sanitizers)

# ---------------------------------------------------------------------------
# LTO (Link Time Optimization) -- Release only.
#
# Normally each .cpp is compiled to finished machine code in isolation, so when the
# compiler builds main.cpp it has never seen receive()'s or decode_packet()'s body and
# must emit an opaque call. Our hot loop pays for that twice per packet, and main's
# `packet.empty()` re-tests something receive() already decided.
#
# LTO makes each .o store a half-compiled intermediate form instead, and hands all of
# them to the optimiser at LINK time -- so it can finally inline across files.
#
# This does NOT weaken the layering: LTO merges only at code generation. Source files,
# headers and the CMake dependency graph above are untouched.
#
# Release only because it makes builds much slower and debugging harder.
# ---------------------------------------------------------------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT MM_LTO_OK OUTPUT MM_LTO_WHY)

if(MM_LTO_OK)
    set_property(TARGET trading_system mm_transport mm_codec
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(WARNING "LTO unavailable, building without it: ${MM_LTO_WHY}")
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(test)
