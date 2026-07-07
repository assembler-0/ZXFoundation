# SPDX-License-Identifier: Apache-2.0
# @brief Kernel nucleus compilation, loader, and symbol table.

set(zxfoundation_LINKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/link.ld"
    CACHE STRING "zxfoundation linker script")

set(_zxf_config_cxxm "${CMAKE_SOURCE_DIR}/zxfoundation/base/config.cxxm")

add_custom_target(zxf_bump_buildno ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DZXFoundation_Release=${ZXFoundation_Release}
        "-DZXFoundation_Copyright_Date=${ZXFoundation_Copyright_Date}"
        "-DZXFoundation_Host_Build_Platform=${ZXFoundation_Host_Build_Platform}"
        -DZXFoundation_Max_Cpus=${MAX_CPUS}
        "-DZXFoundation_Dasd_Boot_Serial=${DASD_SERIAL}"
        -P ${CMAKE_SOURCE_DIR}/cmake/build_number.cmake
    BYPRODUCTS "${_zxf_config_cxxm}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "zxfoundation::build: incrementing BUILD_NUMBER"
    VERBATIM
)

include(cmake/zxfl-compile.cmake)

macro(_zx_kernel_flags _tgt)
    target_include_directories(${_tgt} SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/include
    )
    target_include_directories(${_tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_options(${_tgt} PRIVATE
        -ffreestanding -fno-builtin
        -Wall -Wextra -Wpedantic -Werror
        -fno-strict-aliasing -fno-common -fwrapv
        -ftrivial-auto-var-init=pattern
        -fno-omit-frame-pointer -mbackchain
        -fstack-protector-strong -pipe
        -mno-packed-stack -mhard-float -mvx
        -fno-exceptions -fno-rtti
        -nostdlib
        -include ${ZX_VALIDATE_HEADER}
        -march=${MARCH_MODE} -mtune=${MARCH_MODE} -m64
        ${EXTRA_KERNEL_FLAGS}
        -O${OPT_LEVEL} -g${DSYM_LEVEL}
    )
    if(COMPILER_ID STREQUAL "clang")
        target_compile_options(${_tgt} PRIVATE
            -nostdlib++ --target=${COMMON_TARGET_TRIPLE}
        )
    endif()
    if(COMPILER_ID STREQUAL "gcc")
        target_compile_options(${_tgt} PRIVATE
            -static-libgcc -mzarch
        )
    endif()
    target_compile_definitions(${_tgt} PUBLIC
        __zxfoundation__
    )
    set_target_properties(${_tgt} PROPERTIES
        LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")
    target_link_options(${_tgt} PRIVATE
        -T ${zxfoundation_LINKER_SCRIPT}
        -static --no-dynamic-linker -ztext
        --no-pie -g -m${TARGET_EMULATION_MODE}
    )
    if (COMPILER_ID STREQUAL "gcc")
        target_link_options(${_tgt} PRIVATE
            --no-warn-rwx-segments
        )
    endif()
endmacro()

if(ZX_CAN_BUILD_SYMRES)
    include(cmake/zxfoundation-zxallsyms-compile.cmake)
else()
    message(STATUS "zxfoundation::build: symbol table disabled — building single-pass kernel with stub")
    add_executable(core.zxfoundation.nucleus)

    target_sources(core.zxfoundation.nucleus PRIVATE
        ${ZX_SOURCES_64}
        "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.cxx"
    )
    target_sources(core.zxfoundation.nucleus
        PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${ZX_SOURCES_MODULES_64}
    )
    _zx_kernel_flags(core.zxfoundation.nucleus)
endif()

add_dependencies(core.zxfoundation.nucleus zxf_bump_buildno)

if(ZX_CAN_BUILD_SYMRES)
    add_dependencies(core.zxfoundation.nucleus zxallsyms_data)
endif()

if(ZX_CAN_SIGN_KERNEL)
    add_dependencies(core.zxfoundation.nucleus tools)
    add_custom_command(
        TARGET core.zxfoundation.nucleus POST_BUILD
        COMMAND "${ZXSIGN}" "${CMAKE_BINARY_DIR}/core.zxfoundation.nucleus"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "zxfoundation::build: signing kernel segments (zxsign)"
        VERBATIM
    )
endif()
