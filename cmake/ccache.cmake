# ccache configuration

option(CCACHE_ENABLE "use ccache for faster compilation" ON)

if(CCACHE_ENABLE)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_ASM_COMPILER_LAUNCHER ${CCACHE_PROGRAM})

        set(ENV{CCACHE_MAXSIZE} "12G")

        message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")

        execute_process(
            COMMAND ${CCACHE_PROGRAM} -s
            OUTPUT_VARIABLE CCACHE_STATS
            ERROR_QUIET
        )
        if(CCACHE_STATS)
            message(STATUS "ccache statistics:\n${CCACHE_STATS}")
        endif()
    else()
        message(WARNING "ccache requested but not found")
    endif()
endif()