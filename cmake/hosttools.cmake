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

    set(BIN2REC       "${CMAKE_BINARY_DIR}/bin2rec")
    set(ZXSIGN        "${CMAKE_BINARY_DIR}/zxsign")
    set(ZXALLSYMS_GEN "${CMAKE_BINARY_DIR}/zxallsyms_gen")
endif()
