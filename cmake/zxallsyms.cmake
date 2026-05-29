# SPDX-License-Identifier: Apache-2.0
# cmake/zxallsyms.cmake — Two-pass zxallsyms table generation

if (NOT ZX_HOST_CC)
    message(FATAL_ERROR "zxallsyms: ZX_HOST_CC is not set — cannot build zxallsyms_gen")
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/zxallsyms_gen"
    COMMAND ${ZX_HOST_CC}
            -O2
            -o "${CMAKE_BINARY_DIR}/zxallsyms_gen"
            "${CMAKE_SOURCE_DIR}/tools/zxallsyms_gen.c"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/zxallsyms_gen.c"
    COMMENT "zxfoundation::build: building zxallsyms_gen"
    VERBATIM
)

add_custom_target(zxallsyms_gen_tool
    DEPENDS "${CMAKE_BINARY_DIR}/zxallsyms_gen"
)

# ---------------------------------------------------------------------------
# Pass 1: link with stub
# ---------------------------------------------------------------------------

add_executable(core.zxfoundation.nucleus.pass1 EXCLUDE_FROM_ALL
    ${ZX_SOURCES_64_NO_STUB}
    "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.c"
)

# Inherit the same compile options as the final target.
target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
    -ffreestanding
    -nostdlib
    -fno-builtin
    -Wall -Wextra -Wpedantic -Werror
    -fno-strict-aliasing
    -fno-common
    -fwrapv
    -ftrivial-auto-var-init=pattern
    -fno-omit-frame-pointer
    -mbackchain
    -fstack-protector-all
    -pipe
    -mno-packed-stack
    -msoft-float
    -mno-vx
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -m64
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

if (COMPILER_ID STREQUAL "clang")
    target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
        -Wno-gnu-statement-expression-from-macro-expansion
        -Wno-gnu-pointer-arith
        -Wno-gnu-zero-variadic-macro-arguments
        -Wno-c2y-extensions
    )
endif()

if (COMPILER_ID STREQUAL "gcc")
    target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
        -static-libgcc
        -Wno-array-bounds
        -fno-delete-null-pointer-checks
        -mzarch
    )
endif()

target_compile_definitions(core.zxfoundation.nucleus.pass1 PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

if (CONFIG_UBSAN)
    target_compile_options(core.zxfoundation.nucleus.pass1 PRIVATE
        -fsanitize=undefined,bounds,shift,alignment,null,vla-bound,object-size,return
    )
    target_compile_definitions(core.zxfoundation.nucleus.pass1 PUBLIC
        CONFIG_UBSAN=1
    )
endif()

set_target_properties(core.zxfoundation.nucleus.pass1 PROPERTIES
    LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_options(core.zxfoundation.nucleus.pass1 PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -nostdlib
    -static
    --no-dynamic-linker
    -ztext
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)

if (COMPILER_ID STREQUAL "gcc")
    target_link_options(core.zxfoundation.nucleus.pass1 PRIVATE
        --no-warn-rwx-segments
    )
endif()

set(ZXALLSYMS_DATA_C "${CMAKE_BINARY_DIR}/zxallsyms_data.c")

add_custom_command(
    OUTPUT "${ZXALLSYMS_DATA_C}"
    COMMAND "${CMAKE_NM}" --defined-only --numeric-sort
                "$<TARGET_FILE:core.zxfoundation.nucleus.pass1>"
            | "${CMAKE_BINARY_DIR}/zxallsyms_gen"
                "${ZXALLSYMS_DATA_C}"
    DEPENDS
        core.zxfoundation.nucleus.pass1
        zxallsyms_gen_tool
    COMMENT "zxfoundation::build: generating zxallsyms symbol table"
    VERBATIM
)

add_custom_target(zxallsyms_data
    DEPENDS "${ZXALLSYMS_DATA_C}"
)
