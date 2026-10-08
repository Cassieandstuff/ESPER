# ── ESPER ─────────────────────────────────────────────────────────────────────
# Clean-room C++23 Bethesda plugin (ESM/ESP/ESL) reader/writer: load-order
# resolution, TES4 + GRUP tree, full record index, FormKey identity, override
# chains, the declarative record-schema system, the write/mutation pipeline,
# the world/cell index, and localized STRINGS.
#
# Consumed via FetchContent; consumers link `esper::esper` and
# `#include <ESPER.h>`. Layout:
#   include/ESPER.h          — the umbrella (the one header consumers include)
#   include/ESPER/Foo.h      — the component headers behind it
#   cpp/                     — the impl
#   third_party/miniz        — vendored zlib (exposed as esper::miniz)
#
# Dependency-free LEAF library. The MSVC runtime (static /MT vs dynamic /MD) is
# deliberately NOT set here — it inherits CMAKE_MSVC_RUNTIME_LIBRARY from the
# consuming project, so the same source serves SKSE plugins and desktop apps.

cmake_minimum_required(VERSION 3.28)

project(ESPER
    VERSION 0.1.0
    DESCRIPTION "Clean-room C++23 Bethesda plugin (ESM/ESP/ESL) reader/writer"
    LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(ESPER_BUILD_TESTS "Build ESPER tests" ${PROJECT_IS_TOP_LEVEL})
option(ESPER_BUILD_TOOLS "Build ESPER tools (roundtrip-gate)" OFF)

# ── Vendored miniz (zlib inflate/deflate) ─────────────────────────────────────
# Its own target so consumers that also need zlib (zip writers, test fixtures)
# link this one copy instead of compiling a second miniz.c into the same image.
add_library(esper_miniz STATIC third_party/miniz/miniz.c)
add_library(esper::miniz ALIAS esper_miniz)
target_include_directories(esper_miniz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz>)
if(MSVC)
    target_compile_options(esper_miniz PRIVATE /w)
endif()

# ── esper ─────────────────────────────────────────────────────────────────────
file(GLOB_RECURSE ESPER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")
file(GLOB_RECURSE ESPER_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

add_library(esper STATIC ${ESPER_SOURCES} ${ESPER_HEADERS})
add_library(esper::esper ALIAS esper)

target_include_directories(esper PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(esper PUBLIC cxx_std_23)
# PRIVATE: only Zlib.cpp includes miniz.h; the link dependency still propagates.
target_link_libraries(esper PRIVATE esper::miniz)

if(MSVC)
    target_compile_options(esper PRIVATE /W4 /wd4100 /utf-8)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ESPER_SOURCES} ${ESPER_HEADERS})

# ── Tests ─────────────────────────────────────────────────────────────────────
if(ESPER_BUILD_TESTS)
    enable_testing()
    add_executable(esper-test tests/plugin_test.cpp)
    # The test compresses fixture records with miniz mz_compress.
    target_link_libraries(esper-test PRIVATE esper::esper esper::miniz)
    if(MSVC)
        target_compile_options(esper-test PRIVATE /utf-8)
    endif()
    add_test(NAME esper-test COMMAND esper-test)
endif()

# ── Tools: the real-data round-trip gate (needs game masters; not a ctest) ────
if(ESPER_BUILD_TOOLS)
    add_executable(roundtrip-gate tools/roundtrip_gate.cpp)
    target_link_libraries(roundtrip-gate PRIVATE esper::esper)
    if(MSVC)
        target_compile_options(roundtrip-gate PRIVATE /utf-8)
    endif()
endif()
