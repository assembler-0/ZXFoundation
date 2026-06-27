# SPDX-License-Identifier: Apache-2.0
# @file cmake/hosttools.cmake
# @brief Build host tools (bin2rec, zxsign, zxallsyms_gen).
#        Guarded by ZX_CAN_BUILD_HOST_TOOLS (set by toolchain-validate).

if(NOT ZX_CAN_BUILD_HOST_TOOLS)
    message(WARNING "zxfoundation::hosttools: host CC unavailable — skipping host tools")
    return()
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bin2rec"
    COMMAND ${ZX_HOST_CC} ${CMAKE_SOURCE_DIR}/tools/bin2rec.c
            -o "${CMAKE_BINARY_DIR}/bin2rec"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/bin2rec.c"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
    COMMENT "zxfoundation::build: building bin2rec"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/zxsign"
    COMMAND ${ZX_HOST_CC}
            -I${CMAKE_SOURCE_DIR}/arch/s390x/init/zxfl/include
            ${CMAKE_SOURCE_DIR}/tools/zxsign.c
            ${CMAKE_SOURCE_DIR}/arch/s390x/init/zxfl/common/sha256.c
            -o "${CMAKE_BINARY_DIR}/zxsign"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
    COMMENT "zxfoundation::build: building zxsign"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/zxallsyms_gen"
    COMMAND ${ZX_HOST_CC} ${CMAKE_SOURCE_DIR}/tools/zxallsyms_gen.c
            -o "${CMAKE_BINARY_DIR}/zxallsyms_gen"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/zxallsyms_gen.c"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
    COMMENT "zxfoundation::build: building zxallsyms_gen"
)

add_custom_target(tools ALL
    DEPENDS
        "${CMAKE_BINARY_DIR}/bin2rec"
        "${CMAKE_BINARY_DIR}/zxsign"
        "${CMAKE_BINARY_DIR}/zxallsyms_gen"
)

set(BIN2REC       "${CMAKE_BINARY_DIR}/bin2rec"       CACHE FILEPATH "bin2rec tool" FORCE)
set(ZXSIGN        "${CMAKE_BINARY_DIR}/zxsign"        CACHE FILEPATH "zxsign tool" FORCE)
set(ZXALLSYMS_GEN "${CMAKE_BINARY_DIR}/zxallsyms_gen" CACHE FILEPATH "zxallsyms_gen tool" FORCE)

message(STATUS "zxfoundation::hosttools: tools target ready (bin2rec, zxsign, zxallsyms_gen)")
