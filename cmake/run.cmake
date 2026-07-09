# SPDX-License-Identifier: Apache-2.0
# @brief Post-build / run targets.

if (ZX_CAN_BUILD_DASD)
    add_custom_command(
        OUTPUT sysres.3390
        COMMAND ${CMAKE_COMMAND} -E rm -f sysres.3390
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/scripts/etc.zxfoundation.parm
            etc.zxfoundation.parm
        COMMAND ${DASDLOAD} -z ${CMAKE_SOURCE_DIR}/scripts/sysres.conf
            sysres.3390
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/scripts/hercules.cnf
            hercules.cnf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS core.zxfoundation.nucleus zxfl_stage1.elf zxfl_stage2.elf
                zxf.init ${CMAKE_SOURCE_DIR}/scripts/sysres.conf
        COMMENT "zxfoundation::build: generating sysres.3390"
    )
    add_custom_target(dasd ALL DEPENDS sysres.3390)
endif()

if (ZX_CAN_BUILD_DASD AND ZX_CAN_CHECK_DASD)
    add_custom_command(
        OUTPUT sysres.3390.chk
        COMMAND ${CCKDCDSK} -ro -3 sysres.3390
        COMMAND ${CMAKE_COMMAND} -E touch sysres.3390.chk
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS dasd
        COMMENT "zxfoundation::build: checking sysres.3390"
    )
    add_custom_target(dasdcheck ALL DEPENDS sysres.3390.chk sysres.3390)
endif()

if (ZX_CAN_BUILD_DASD AND ZX_CAN_SERIAL_DASD AND DASD_SERIAL)
    add_custom_command(
        OUTPUT sysres.3390.serial
        COMMAND ${DASDSER} sysres.3390 ${DASD_SERIAL}
        COMMAND ${CMAKE_COMMAND} -E touch sysres.3390.serial
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS dasd
        COMMENT "zxfoundation::build: serializing sysres.3390"
    )
    add_custom_target(dasdserial ALL DEPENDS sysres.3390.serial sysres.3390)
endif()

file(READ ${CMAKE_SOURCE_DIR}/BUILD_NUMBER BUILD_VERSION)
string(STRIP "${BUILD_VERSION}" BUILD_VERSION_CLEAN)

add_custom_target(releaseinfo
    COMMAND ${CMAKE_COMMAND} -E echo
        "ZXFoundation (TM) ${ZXFoundation_Release} build #${BUILD_VERSION_CLEAN}"
    COMMAND ${CMAKE_COMMAND} -E echo
        "Copyright (C) ${ZXFoundation_Copyright_Date} assembler-0. Licensed under the Apache License, Version 2.0."
    COMMAND ${CMAKE_COMMAND} -E echo
        "Built on ${ZXFoundation_Host_Build_Platform} ${CURRENT_MONTH}/${CURRENT_YEAR}"
)

add_custom_target(docs
    COMMAND ${CMAKE_COMMAND} -E echo
        "To keep the kernel dependencies minimal. We have chose not to include Doxygen/Python dependencies"
    COMMAND ${CMAKE_COMMAND} -E echo
        "The procedure is highly straight-forward. You can build and view documents using the following command(s) in the project's root:"
    COMMAND ${CMAKE_COMMAND} -E echo "doxygen && python -m http.server -d doxygen-build/html"
    COMMAND ${CMAKE_COMMAND} -E echo "Sorry for your inconvenience"
)
