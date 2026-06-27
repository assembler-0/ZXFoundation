# SPDX-License-Identifier: Apache-2.0
# @file cmake/ccache.cmake
# @brief CCache integration.  Detection is handled by toolchain-validate;
#        this module only applies the launcher configuration.

option(CCACHE_ENABLE "use ccache for faster compilation" ON)

if (CCACHE_ENABLE AND ZX_FOUND_CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_ASM_COMPILER_LAUNCHER ${CCACHE_PROGRAM})

    set(ENV{CCACHE_MAXSIZE} "12G")

    message(STATUS "zxfoundation::ccache: enabled (${CCACHE_PROGRAM})")

    execute_process(
        COMMAND ${CCACHE_PROGRAM} -s
        OUTPUT_VARIABLE CCACHE_STATS
        ERROR_QUIET
    )
    if(CCACHE_STATS)
        message(STATUS "zxfoundation::ccache: statistics:\n${CCACHE_STATS}")
    endif()
elseif (CCACHE_ENABLE)
    message(WARNING "zxfoundation::ccache: requested but not found")
endif()
