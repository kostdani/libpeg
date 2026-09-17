#
# src/term/CMakeLists.txt - standalone entry point for the term module.
#
# The main build does NOT add_subdirectory() this file: it compiles node.c
# and sexpr.c straight into libpeg, so there is one archive to link and
# install and the sanitizer flags reach every executable from one place.
# This file exists so the module can be developed and proved on its own,
# without building the parser:
#
#   cmake -S src/term -B build-term
#   cmake --build build-term && ctest --test-dir build-term
#
# The public header resolution below is the one thing the two builds must
# agree on; keep it pointed at the repository's include/ directory.
#
cmake_minimum_required(VERSION 3.16)

project(peg_term C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Release so a bare configure is optimized: the term type is on
# the parser's hot path.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PEG_TERM_SANITIZE "Build the term module with ASan+UBSan" OFF)
option(PEG_TERM_WERROR "Treat compiler warnings as errors" OFF)

# Public headers live under include/peg/ at the repository root.
set(PEG_TERM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

add_library(peg_term STATIC
    node.c
    sexpr.c)

target_include_directories(peg_term PUBLIC
    $<BUILD_INTERFACE:${PEG_TERM_INCLUDE_DIR}>)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(peg_term PRIVATE -Wall -Wextra)
endif()

if(PEG_TERM_WERROR)
    target_compile_options(peg_term PRIVATE -Werror)
endif()

if(PEG_TERM_SANITIZE)
    # PUBLIC: the flags have to reach the link line of every executable that
    # links this static library, not just this compilation.
    target_compile_options(peg_term PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(peg_term PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()

# The module's test suite lives with the other unit tests, so both builds
# run the same file and it cannot drift between them.
add_executable(test_term ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/test_term.c)
target_link_libraries(test_term PRIVATE peg_term)
add_test(NAME test_term COMMAND test_term)
