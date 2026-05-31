# ZXFoundation host tools

if (ZX_HOST_CC)
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
                -I${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/tools/zxsign.c
                ${CMAKE_SOURCE_DIR}/crypto/sha256.c
                -o "${CMAKE_BINARY_DIR}/zxsign"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/tools/zxsign.c"
            "${CMAKE_SOURCE_DIR}/crypto/sha256.c"
            "${CMAKE_SOURCE_DIR}/include/crypto/sha256.h"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "zxfoundation::build: building zxsign"
    )

    add_custom_target(tools ALL
        DEPENDS
            "${CMAKE_BINARY_DIR}/bin2rec"
            "${CMAKE_BINARY_DIR}/zxsign"
    )

    set(BIN2REC       "${CMAKE_BINARY_DIR}/bin2rec")
    set(ZXSIGN "${CMAKE_BINARY_DIR}/zxsign")
endif()