# SPDX-License-Identifier: Apache-2.0
# @brief Post-build / run targets.

if (ZX_CAN_BUILD_DASD)
    add_custom_command(
        OUTPUT core.zxfoundation.sysdsk00.cckd
        COMMAND ${CMAKE_COMMAND} -E rm -f core.zxfoundation.sysdsk00.cckd
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/scripts/etc.zxfoundation.parm
            etc.zxfoundation.parm
        COMMAND ${DASDLOAD} -z ${CMAKE_SOURCE_DIR}/scripts/sysres.conf
            core.zxfoundation.sysdsk00.cckd
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/scripts/hercules.cnf
            hercules.cnf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS core.zxfoundation.nucleus zxfl_stage1.elf zxfl_stage2.elf
                zxf.init ${CMAKE_SOURCE_DIR}/scripts/sysres.conf
        COMMENT "zxfoundation::build: generating core.zxfoundation.sysdsk00.cckd"
    )
    add_custom_target(dasd ALL DEPENDS core.zxfoundation.sysdsk00.cckd)
endif()

if (ZX_CAN_BUILD_DASD AND ZX_CAN_CHECK_DASD)
    add_custom_command(
        OUTPUT core.zxfoundation.sysdsk00.cckd.chk
        COMMAND ${CCKDCDSK} -ro -3 core.zxfoundation.sysdsk00.cckd
        COMMAND ${CMAKE_COMMAND} -E touch core.zxfoundation.sysdsk00.cckd.chk
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS dasd
        COMMENT "zxfoundation::build: checking core.zxfoundation.sysdsk00.cckd"
    )
    add_custom_target(dasdcheck ALL DEPENDS core.zxfoundation.sysdsk00.cckd.chk core.zxfoundation.sysdsk00.cckd)
endif()

if (ZX_CAN_BUILD_DASD AND ZX_CAN_SERIAL_DASD AND DASD_SERIAL)
    add_custom_command(
        OUTPUT core.zxfoundation.sysdsk00.cckd.serial
        COMMAND ${DASDSER} core.zxfoundation.sysdsk00.cckd ${DASD_SERIAL}
        COMMAND ${CMAKE_COMMAND} -E touch core.zxfoundation.sysdsk00.cckd.serial
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS dasd
        COMMENT "zxfoundation::build: serializing core.zxfoundation.sysdsk00.cckd"
    )
    add_custom_target(dasdserial ALL DEPENDS core.zxfoundation.sysdsk00.cckd.serial core.zxfoundation.sysdsk00.cckd)
endif()

if (ZX_CAN_DUMP_ARTIFACTS)
    add_custom_target(artifact-dump
        COMMAND ${ZX_NM} core.zxfoundation.nucleus > core.zxfoundation.nucleus.sym
        COMMAND ${ZX_NM} core.zxfoundation.nucleus.pass1 > core.zxfoundation.nucleus.pass1.sym
        COMMAND ${ZX_NM} zxfl_stage1.elf > zxfl_stage1.elf.sym
        COMMAND ${ZX_NM} zxfl_stage2.elf > zxfl_stage2.elf.sym
        COMMAND ${ZX_NM} zxf.init > zxf.init.sym
        COMMAND ${CMAKE_OBJDUMP} -d core.zxfoundation.nucleus > core.zxfoundation.nucleus.dism
        COMMAND ${CMAKE_OBJDUMP} -d core.zxfoundation.nucleus.pass1 > core.zxfoundation.nucleus.pass1.dism
        COMMAND ${CMAKE_OBJDUMP} -d zxfl_stage1.elf > zxfl_stage1.elf.dism
        COMMAND ${CMAKE_OBJDUMP} -d zxfl_stage2.elf > zxfl_stage2.elf.dism
        COMMAND ${CMAKE_OBJDUMP} -d zxf.init > zxf.init.dism
        VERBATIM
        DEPENDS core.zxfoundation.nucleus zxfl_stage1.elf zxfl_stage2.elf zxf.init
        COMMENT "zxfoundation::build: disassembling and extracting symbols from artifacts"
    )
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
