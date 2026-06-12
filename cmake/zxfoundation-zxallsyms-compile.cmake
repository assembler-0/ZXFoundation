# SPDX-License-Identifier: Apache-2.0
# cmake/zxfoundation-zxallsyms-compile.cmake — Two-pass zxallsyms table generation

add_executable(core.zxfoundation.nucleus.pass1 EXCLUDE_FROM_ALL)

target_sources(core.zxfoundation.nucleus.pass1 PRIVATE
    ${ZX_SOURCES_64_NO_STUB}
    "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.cxx"
)

target_sources(core.zxfoundation.nucleus.pass1
    PUBLIC
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ZX_SOURCES_MODULES_64}
)

target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
    -ffreestanding
    -fno-builtin
    -Wall -Wextra -Wpedantic -Werror
    -fno-strict-aliasing
    -fno-common
    -fwrapv
    -ftrivial-auto-var-init=pattern
    -fno-omit-frame-pointer
    -mbackchain
    -fno-stack-protector
    -pipe
    -mno-packed-stack
    -msoft-float
    -mno-vx
    -fno-exceptions
    -fno-rtti
    -nostdlib
    -nostdinc
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -m64
)

if (CONFIG_SSP)
    target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
        -fstack-protector-all
    )
endif()

if (COMPILER_ID STREQUAL "clang")
    target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
        -nostdlib++
        --target=${COMMON_TARGET_TRIPLE}
    )
endif()

target_compile_definitions(core.zxfoundation.nucleus.pass1 PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

set_target_properties(core.zxfoundation.nucleus.pass1 PROPERTIES
    LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")

target_link_options(core.zxfoundation.nucleus.pass1 PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -static
    --no-dynamic-linker
    -ztext
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)

set(ZXALLSYMS_DATA "${CMAKE_BINARY_DIR}/zxallsyms_data.cxx")

add_custom_command(
    OUTPUT "${ZXALLSYMS_DATA}"
    COMMAND "${ZX_NM}" --defined-only --numeric-sort
    "$<TARGET_FILE:core.zxfoundation.nucleus.pass1>"
    | ${ZX_CXXFILT} | "${ZXALLSYMS_GEN}" "${ZXALLSYMS_DATA}"
    DEPENDS
    core.zxfoundation.nucleus.pass1
    COMMENT "zxfoundation::build: generating zxallsyms symbol table"
    VERBATIM
)

add_custom_target(zxallsyms_data
    DEPENDS "${ZXALLSYMS_DATA}"
)