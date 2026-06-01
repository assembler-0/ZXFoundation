# preconf.cmake

set(ZXFoundation_Release "26h1")
set(ZXFoundation_Copyright_Date "2026")
set(ZXFoundation_Host_Build_Platform "${CMAKE_HOST_SYSTEM_NAME}@${CMAKE_HOST_SYSTEM_VERSION}::${CMAKE_HOST_SYSTEM_PROCESSOR}")

configure_file(
    ${CMAKE_SOURCE_DIR}/include/zxfoundation/zxconfig.h.in
    ${CMAKE_SOURCE_DIR}/include/zxfoundation/zxconfig.h
    @ONLY
)