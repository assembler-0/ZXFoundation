# SPDX-License-Identifier: Apache-2.0
# @file cmake/zxfoundation-zxallsyms-compile.cmake
# @brief Two-pass zxallsyms symbol table generation.

set(ZXALLSYMS_DATA "${CMAKE_BINARY_DIR}/zxallsyms_data.cxx")

add_executable(core.zxfoundation.nucleus.pass1 EXCLUDE_FROM_ALL)

target_sources(core.zxfoundation.nucleus.pass1 PRIVATE
    ${ZX_SOURCES_64_NO_STUB}
    "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.cxx"
)
target_sources(core.zxfoundation.nucleus.pass1
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ZX_SOURCES_MODULES_64}
)

_zx_kernel_flags(core.zxfoundation.nucleus.pass1)

add_custom_command(
    OUTPUT "${ZXALLSYMS_DATA}"
    COMMAND "${ZX_NM}" --defined-only --numeric-sort
        "$<TARGET_FILE:core.zxfoundation.nucleus.pass1>"
        | ${ZX_CXXFILT}
        | "${ZXALLSYMS_GEN}" "${ZXALLSYMS_DATA}"
    DEPENDS core.zxfoundation.nucleus.pass1
    COMMENT "zxfoundation::build: generating zxallsyms symbol table"
    VERBATIM
)

add_custom_target(zxallsyms_data
    DEPENDS "${ZXALLSYMS_DATA}"
)

add_executable(core.zxfoundation.nucleus)

target_sources(core.zxfoundation.nucleus PRIVATE
    ${ZX_SOURCES_64}
    "${ZXALLSYMS_DATA}"
)
target_sources(core.zxfoundation.nucleus
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ZX_SOURCES_MODULES_64}
)

_zx_kernel_flags(core.zxfoundation.nucleus)

set_source_files_properties("${ZXALLSYMS_DATA}" PROPERTIES GENERATED TRUE)
