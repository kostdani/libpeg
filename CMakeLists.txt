cmake_minimum_required(VERSION 3.16)

project(libpeg
    VERSION 0.1.0
    DESCRIPTION "Fast incremental PEG parsing"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Release so an unconfigured build gets optimization (the
# benchmarks are meaningless at -O0).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PEG_BUILD_TESTS "Build the test suite" ON)
option(PEG_BUILD_BENCH "Build the benchmark harness" OFF)
option(PEG_WERROR "Treat compiler warnings as errors" OFF)
option(PEG_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)

include(GNUInstallDirs)

# ------------------------------------------------------------------
# Library
# ------------------------------------------------------------------

# include/peg/ holds the public headers and is the only thing installed;
# src/ holds the implementation together with the internal headers
# (util.h, vm_internal.h), which no consumer sees.
add_library(peg STATIC
    src/util.c
    src/interval_tree.c
    src/memo.c
    src/vm_code.c
    src/vm.c
    src/pattern.c
    src/peg.c)
add_library(libpeg::peg ALIAS peg)

target_include_directories(peg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(peg PRIVATE -Wall -Wextra)
endif()

if(PEG_WERROR)
    target_compile_options(peg PRIVATE -Werror)
endif()

if(PEG_SANITIZE)
    # PUBLIC: the flags must reach the link line of every executable
    # that links the static library, not just its compilation.
    target_compile_options(peg PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(peg PUBLIC -fsanitize=address,undefined)
endif()

set_target_properties(peg PROPERTIES
    OUTPUT_NAME peg
    POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------
# Test data
# ------------------------------------------------------------------

# The large inputs for the end-to-end tests and benchmarks live in a
# sibling testdata/ directory; pass its absolute path to the tests so
# they work from any build directory.  Tests skip gracefully when the
# directory is absent.  A cache entry (rather than a plain variable) so
# that -DPEG_TESTDATA_DIR=... can point at a checkout that keeps the
# data elsewhere.
set(PEG_TESTDATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/testdata/"
    CACHE PATH "Directory holding the large test/benchmark inputs")

# CMake normalizes PATH cache entries and strips any trailing slash, but
# the tests reach their inputs by literal concatenation
# (PEG_TESTDATA_DIR "test.json"), so the macro must end in a separator.
# Re-add it here rather than depending on how -D was spelled.
string(REGEX REPLACE "/+$" "" PEG_TESTDATA_DIR "${PEG_TESTDATA_DIR}")
set(PEG_TESTDATA_DIR "${PEG_TESTDATA_DIR}/")

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------

if(PEG_BUILD_TESTS)
    enable_testing()

    set(PEG_UNIT_TESTS
        test_interval_tree
        test_memo
        test_vm
        test_pattern)

    foreach(test IN LISTS PEG_UNIT_TESTS)
        add_executable(${test} tests/${test}.c)
        target_link_libraries(${test} PRIVATE peg)
        # In-tree tests may include the internal headers from src/.
        target_include_directories(${test} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()

    # test_interval_tree cross-checks the AVL tree against a naive array
    # oracle.  The oracle exists only for that differential test, so it
    # is compiled into the test rather than shipped in the library.
    target_sources(test_interval_tree PRIVATE tests/interval_array.c)

    # The end-to-end test reads the real-world inputs.
    add_executable(test_peg tests/test_peg.c)
    target_link_libraries(test_peg PRIVATE peg)
    target_include_directories(test_peg PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_peg PRIVATE
        PEG_TESTDATA_DIR="${PEG_TESTDATA_DIR}")
    add_test(NAME test_peg COMMAND test_peg)
endif()

# ------------------------------------------------------------------
# Benchmarks
# ------------------------------------------------------------------

if(PEG_BUILD_BENCH)
    add_executable(bench_peg bench/bench_peg.c)
    target_link_libraries(bench_peg PRIVATE peg)
    target_include_directories(bench_peg PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(bench_peg PRIVATE
        PEG_TESTDATA_DIR="${PEG_TESTDATA_DIR}")
endif()

# ------------------------------------------------------------------
# Install
# ------------------------------------------------------------------

# Only the public API: the transitive closure of what peg.h includes.
install(TARGETS peg
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    include/peg/peg.h
    include/peg/pattern.h
    include/peg/vm.h
    include/peg/memo.h
    include/peg/interval_tree.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peg)
