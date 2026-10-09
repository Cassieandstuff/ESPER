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

# ── Schema generator (contributors only) ──────────────────────────────────────
# schema/ is compiled into schema.json + include/ESPER/generated/*.h by
# tools/schemagen. The output is committed, so consumers never need Python;
# these targets exist only where Python 3 + PyYAML are available.
set(ESPER_SCHEMAGEN_AVAILABLE OFF)
if(PROJECT_IS_TOP_LEVEL)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import yaml"
                        RESULT_VARIABLE _esper_yaml_rc OUTPUT_QUIET ERROR_QUIET)
        if(_esper_yaml_rc EQUAL 0)
            set(ESPER_SCHEMAGEN_AVAILABLE ON)
        endif()
    endif()
endif()

set(_ESPER_SCHEMAGEN "${CMAKE_CURRENT_SOURCE_DIR}/tools/schemagen/schemagen.py")
set(_ESPER_GEN_MAIN
    --schema "${CMAKE_CURRENT_SOURCE_DIR}/schema"
    --out "${CMAKE_CURRENT_SOURCE_DIR}/include/ESPER/generated"
    --json "${CMAKE_CURRENT_SOURCE_DIR}/schema.json")
set(_ESPER_GEN_TEST
    --schema "${CMAKE_CURRENT_SOURCE_DIR}/tests/schema"
    --out "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated"
    --tables-ns esper_test::tables --fields-ns esper_test::fields --views-ns esper_test::view)

if(ESPER_SCHEMAGEN_AVAILABLE)
    add_custom_target(esper-schemagen
        COMMAND "${Python3_EXECUTABLE}" -I "${_ESPER_SCHEMAGEN}" ${_ESPER_GEN_MAIN}
        COMMAND "${Python3_EXECUTABLE}" -I "${_ESPER_SCHEMAGEN}" ${_ESPER_GEN_TEST}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Regenerating ESPER schema outputs"
        VERBATIM)
endif()

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

    # The schema engine, driven by a test-only schema (tests/schema) that
    # exercises every language construct, plus the real schema's records.
    add_executable(esper-schema-test tests/schema_test.cpp)
    target_link_libraries(esper-schema-test PRIVATE esper::esper)
    target_include_directories(esper-schema-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(MSVC)
        target_compile_options(esper-schema-test PRIVATE /W4 /wd4100 /utf-8)
    endif()
    add_test(NAME esper-schema-test COMMAND esper-schema-test)

    if(ESPER_SCHEMAGEN_AVAILABLE)
        add_test(NAME esper-schema-fresh
            COMMAND "${Python3_EXECUTABLE}" -I "${_ESPER_SCHEMAGEN}" ${_ESPER_GEN_MAIN} --check)
        add_test(NAME esper-test-schema-fresh
            COMMAND "${Python3_EXECUTABLE}" -I "${_ESPER_SCHEMAGEN}" ${_ESPER_GEN_TEST} --check)
        add_test(NAME esper-schemagen-test
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/schemagen_test.py")
    endif()
endif()

# ── Tools (need game data; not ctests) ────────────────────────────────────────
#   roundtrip-gate — the real-data round-trip gate over the vanilla masters
#   esm-dump       — read-only research dumper (GRUP tree, records by sig/FormID)
if(ESPER_BUILD_TOOLS)
    foreach(_tool IN ITEMS roundtrip-gate esm-dump)
        string(REPLACE "-" "_" _src "${_tool}")
        add_executable(${_tool} tools/${_src}.cpp)
        target_link_libraries(${_tool} PRIVATE esper::esper)
        if(MSVC)
            target_compile_options(${_tool} PRIVATE /utf-8)
        endif()
    endforeach()
endif()
