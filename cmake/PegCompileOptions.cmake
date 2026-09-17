# PegCompileOptions.cmake - the project's warning policy, in one place.
#
# peg_set_compile_options(<target>) is the only way a target in this tree
# picks up warning flags, so -DPEG_WERROR=ON means the same thing for the
# library, the tests and the tools, and there is one place to add a flag
# when a class of bug bites.
#
# The flags are PRIVATE: none of them are a consumer's business, and a
# flag a consumer's compiler does not know is a flag that breaks their
# build.  Compilers other than GCC and Clang get nothing rather than a
# guess at an equivalent.

include_guard(GLOBAL)

function(peg_set_compile_options target)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wshadow
        -Wpointer-arith
        # C only: the library's headers promise prototypes for everything
        # they declare, and a missing one is an implicit-int bug waiting
        # for the next reader.
        $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
        $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)

    if(PEG_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
