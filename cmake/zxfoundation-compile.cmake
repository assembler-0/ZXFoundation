# ZXFoundation linking & compilation

include(cmake/zxfl-compile.cmake)

add_executable(core.zxfoundation.nucleus
    ${ZX_SOURCES_64}
)

target_compile_options(core.zxfoundation.nucleus PRIVATE
    -ffreestanding
    -nostdlib
    -fno-builtin
    -Wall -Wextra -Wpedantic -Werror
    -fno-strict-aliasing
    -fno-common
    -fwrapv
    -ftrivial-auto-var-init=pattern
    -fno-stack-protector
    -pipe
    -mpacked-stack
    -msoft-float
    -mno-vx
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -m64
)

if(COMPILER_ID STREQUAL "clang")
    target_compile_options(core.zxfoundation.nucleus PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
    )
endif()

if(COMPILER_ID STREQUAL "gcc")
    target_compile_options(core.zxfoundation.nucleus PRIVATE
        -static-libgcc
        -Wno-array-bounds
        -fno-delete-null-pointer-checks
        -mzarch
    )
endif()

target_compile_definitions(core.zxfoundation.nucleus PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(core.zxfoundation.nucleus PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

target_compile_options(core.zxfoundation.nucleus PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

set(zxfoundation_LINKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/link.ld"
    CACHE STRING "zxfoundation linker script")

set_target_properties(core.zxfoundation.nucleus PROPERTIES
    LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")

target_link_options(core.zxfoundation.nucleus PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -nostdlib
    -static
    --no-dynamic-linker
    -ztext
    -zmax-page-size=0x1000
    --no-warn-rwx-segments
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)

# ---------------------------------------------------------------------------
# Post-build
# ---------------------------------------------------------------------------
add_dependencies(core.zxfoundation.nucleus tools)

add_custom_command(
    TARGET core.zxfoundation.nucleus POST_BUILD
    COMMAND "${GEN_CHECKSUMS}" "${CMAKE_BINARY_DIR}/core.zxfoundation.nucleus"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "zxfoundation::build: signing kernel segments (gen_checksums)"
    VERBATIM
)
