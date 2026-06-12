# ZXFoundation linking & compilation

set(zxfoundation_LINKER_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/link.ld"
        CACHE STRING "zxfoundation linker script")

include(cmake/zxfl-compile.cmake)
if (ZXALLSYMS_GEN AND ZX_NM AND ZX_CXXFILT)
    include(cmake/zxfoundation-zxallsyms-compile.cmake)
endif()

add_executable(core.zxfoundation.nucleus)

target_sources(core.zxfoundation.nucleus PRIVATE ${ZX_SOURCES_64})

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
    -nostdlib
    -nostdinc
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -m64
)

if (CONFIG_SSP)
    target_compile_options(core.zxfoundation.nucleus PRIVATE
        -fstack-protector-all
    )
endif()

if (COMPILER_ID STREQUAL "clang")
    target_compile_options(core.zxfoundation.nucleus PRIVATE
        -nostdlib++
        --target=${COMMON_TARGET_TRIPLE}
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

if (ZXALLSYMS_GEN AND ZX_NM AND ZX_CXXFILT)
    add_dependencies(core.zxfoundation.nucleus zxallsyms_data)

    set_source_files_properties(
        ${ZXALLSYMS_DATA}
        PROPERTIES GENERATED TRUE
    )
endif()