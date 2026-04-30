# ZXFoundation host tools

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bin2rec"
    COMMAND ${ZX_HOST_CC} ${CMAKE_SOURCE_DIR}/tools/bin2rec.c -o "${CMAKE_BINARY_DIR}/bin2rec"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
    COMMENT "zxfoundation::build: building bin2rec"
)
add_custom_target(tools ALL DEPENDS "${CMAKE_BINARY_DIR}/bin2rec")

set(BIN2REC "${CMAKE_BINARY_DIR}/bin2rec")