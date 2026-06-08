# ZXFoundation linking & compilation

set(zxfoundation_LINKER_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/link.ld"
        CACHE STRING "zxfoundation linker script")

include(cmake/zxfl-compile.cmake)

add_executable(core.zxfoundation.nucleus
    ${ZX_SOURCES_64}
)

target_sources(core.zxfoundation.nucleus
    PUBLIC
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ZX_SOURCES_MODULES_64}
)

target_compile_options(core.zxfoundation.nucleus PRIVATE
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
    -Wno-pch-date-time
    -Wno-c99-extensions
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -m64
)

if (COMPILER_ID STREQUAL "clang")
    target_compile_options(core.zxfoundation.nucleus PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
        -Wno-gnu-statement-expression-from-macro-expansion
        -Wno-gnu-pointer-arith
        -Wno-gnu-zero-variadic-macro-arguments
        -Wno-c2y-extensions
    )
endif()

if (COMPILER_ID STREQUAL "gcc")
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

set_target_properties(core.zxfoundation.nucleus PROPERTIES
    LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")

target_link_options(core.zxfoundation.nucleus PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -static
    --no-dynamic-linker
    -ztext
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)

if (COMPILER_ID STREQUAL "gcc")
    target_link_options(core.zxfoundation.nucleus PRIVATE
        --no-warn-rwx-segments
    )
endif()

if (ZXSIGN)
    add_dependencies(core.zxfoundation.nucleus tools)

    add_custom_command(
        TARGET core.zxfoundation.nucleus POST_BUILD
        COMMAND "${ZXSIGN}" "${CMAKE_BINARY_DIR}/core.zxfoundation.nucleus"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "zxfoundation::build: signing kernel segments (zxsign)"
        VERBATIM
    )
endif()
