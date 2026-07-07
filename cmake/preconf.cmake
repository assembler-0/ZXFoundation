# SPDX-License-Identifier: Apache-2.0
# cmake/preconf.cmake
# Pre-configuration: compute release label, copyright date, and host platform.

string(TIMESTAMP CURRENT_YEAR "%Y")
string(TIMESTAMP CURRENT_MONTH "%m")

if(CURRENT_YEAR GREATER 2026)
    set(ZXFoundation_Copyright_Date "2026-${CURRENT_YEAR}")
else()
    set(ZXFoundation_Copyright_Date "2026")
endif()

string(SUBSTRING "${CURRENT_YEAR}" 2 2 CURRENT_YEAR_SHORT)
if(CURRENT_YEAR GREATER_EQUAL 2026)
    set(RELEASE_YY "${CURRENT_YEAR_SHORT}")
else()
    set(RELEASE_YY "26")
endif()

if(CURRENT_MONTH GREATER 6)
    set(RELEASE_H "h2")
else()
    set(RELEASE_H "h1")
endif()

set(ZXFoundation_Dasd_Boot_Serial "${DASD_SERIAL}")

set(ZXFoundation_Max_Cpus "${MAX_CPUS}")

set(ZXFoundation_Release "${RELEASE_YY}${RELEASE_H}")

set(ZXFoundation_Host_Build_Platform
    "${CMAKE_HOST_SYSTEM_NAME}@${CMAKE_HOST_SYSTEM_VERSION}::${CMAKE_HOST_SYSTEM_PROCESSOR}")

configure_file(
    ${CMAKE_SOURCE_DIR}/arch/s390x/init/zxfl/include/zxfoundation/zxconfig.h.in
    ${CMAKE_SOURCE_DIR}/arch/s390x/init/zxfl/include/zxfoundation/zxconfig.h
    @ONLY
)

configure_file(
    ${CMAKE_SOURCE_DIR}/zxfoundation/base/config.cxxm.in
    ${CMAKE_SOURCE_DIR}/zxfoundation/base/config.cxxm
    @ONLY
)
