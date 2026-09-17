cmake_minimum_required(VERSION 3.16)

project(libpeg
    VERSION 0.1.0
    DESCRIPTION "Fast incremental PEG parsing"
    LANGUAGES C CXX)

# The library is C11 with no dependencies.  C++ enters only at the two
# edges: the header-only wrapper (include/peg/peg.hpp), and the test
# that holds it to its promises.  C++17 is the floor for string_view.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release so an unconfigured build gets optimization (the
# benchmarks are meaningless at -O0).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PEG_BUILD_TESTS "Build the test suite" ON)
option(PEG_BUILD_BENCH "Build the benchmark harness" OFF)
option(PEG_WERROR "Treat compiler warnings as errors" OFF)
option(PEG_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PegCompileOptions)

# ------------------------------------------------------------------
# Library
# ------------------------------------------------------------------

# The modules are directories, not separate targets: one static library
# keeps the link line (and the sanitizer flags, which must reach it)
# in one place, and gives the embedded ports a single archive to link.
# Public headers live under include/peg/.
#
# src/term/ also carries its own CMakeLists.txt, but only as a standalone
# entry point for developing that module in isolation (cmake -S src/term).
# The main build compiles its sources here, into this one archive, rather
# than pulling in a second library to link and install.
add_library(peg STATIC
    src/alloc/util.c
    src/interval/interval_tree.c
    src/memo/memo.c
    src/vm/vm_code.c
    src/vm/vm.c
    src/pattern/pattern.c
    src/term/node.c
    src/term/sexpr.c
    src/peg.c)
add_library(peg::peg ALIAS peg)

target_include_directories(peg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

peg_set_compile_options(peg)

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
# Tests
# ------------------------------------------------------------------

# The large inputs for the end-to-end tests and benchmarks live in a
# sibling testdata/ directory; pass its absolute path to the tests so
# they work from any build directory.  Tests skip gracefully when the
# directory is absent.  A cache entry (rather than a plain variable) so
# that -DPEG_TESTDATA_DIR=... can point at a checkout that keeps the
# data elsewhere.
set(PEG_TESTDATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../testdata/"
    CACHE PATH "Directory holding the large test/benchmark inputs")

# The tests concatenate this with a bare filename, so the trailing
# separator is load-bearing; -DPEG_TESTDATA_DIR=... rarely carries one,
# and a missing slash silently turns "test.json" into a sibling of the
# directory rather than a file inside it.  Normalise once, here.
set(PEG_TESTDATA_DIR_DEF "${PEG_TESTDATA_DIR}")
if(NOT PEG_TESTDATA_DIR_DEF MATCHES "/$")
    string(APPEND PEG_TESTDATA_DIR_DEF "/")
endif()

if(PEG_BUILD_TESTS)
    enable_testing()

    set(PEG_UNIT_TESTS
        test_interval_tree
        test_memo
        test_vm
        test_pattern
        test_term)

    foreach(test IN LISTS PEG_UNIT_TESTS)
        add_executable(${test} tests/unit/${test}.c)
        target_link_libraries(${test} PRIVATE peg)
        peg_set_compile_options(${test})
        add_test(NAME ${test} COMMAND ${test})
    endforeach()

    # test_interval_tree is the differential test: it cross-checks the
    # interval tree against the naive array oracle.  The oracle is a
    # test-only artifact — it is neither part of the library nor
    # installed — so it is linked into that one executable.
    target_sources(test_interval_tree PRIVATE tests/oracle/interval_array.c)
    target_include_directories(test_interval_tree
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle)

    # The end-to-end test reads the real-world inputs.
    add_executable(test_peg tests/unit/test_peg.c)
    target_link_libraries(test_peg PRIVATE peg)
    target_compile_definitions(test_peg PRIVATE
        PEG_TESTDATA_DIR="${PEG_TESTDATA_DIR_DEF}")
    add_test(NAME test_peg COMMAND test_peg)

    # The C++ wrapper test: a C++17 translation unit that includes every
    # public C header through peg.hpp and exercises the whole ABI
    # through the RAII handles.  It links the same archive the C tests
    # do -- the wrapper is header-only, so there is no second library.
    add_executable(test_peg_cpp tests/unit/test_peg_cpp.cpp)
    target_link_libraries(test_peg_cpp PRIVATE peg)
    peg_set_compile_options(test_peg_cpp)
    add_test(NAME test_peg_cpp COMMAND test_peg_cpp)
endif()

# ------------------------------------------------------------------
# Benchmarks
# ------------------------------------------------------------------

if(PEG_BUILD_BENCH)
    add_executable(bench_peg tools/lpegc/bench.c)
    target_link_libraries(bench_peg PRIVATE peg)
    peg_set_compile_options(bench_peg)
    target_compile_definitions(bench_peg PRIVATE
        PEG_TESTDATA_DIR="${PEG_TESTDATA_DIR_DEF}")
endif()

# ------------------------------------------------------------------
# Install
# ------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(PEG_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/libpeg")

# EXPORT: what makes the installed library usable as find_package(libpeg)
# with the target peg::peg.
install(TARGETS peg
    EXPORT libpegTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES
    include/peg/peg.h
    include/peg/peg_util.h
    include/peg/peg_interval.h
    include/peg/peg_memo.h
    include/peg/peg_vm.h
    include/peg/peg_pattern.h
    include/peg/peg_term.h
    include/peg/peg.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peg)

install(EXPORT libpegTargets
    NAMESPACE peg::
    FILE libpegTargets.cmake
    DESTINATION ${PEG_INSTALL_CMAKEDIR})

configure_package_config_file(
    cmake/libpegConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/libpegConfig.cmake"
    INSTALL_DESTINATION ${PEG_INSTALL_CMAKEDIR})

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libpegConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libpegConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libpegConfigVersion.cmake"
    DESTINATION ${PEG_INSTALL_CMAKEDIR})

# The same information for consumers that do not use CMake at all.
configure_file(cmake/libpeg.pc.in
    "${CMAKE_CURRENT_BINARY_DIR}/libpeg.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libpeg.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
