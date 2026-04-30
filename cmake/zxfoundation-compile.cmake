# ZXFoundation linking & compilation

include(cmake/stage1-compile.cmake)

add_executable(zxfoundation.elf
    ${ZX_SOURCES_64}
)

target_compile_options(zxfoundation.elf PRIVATE
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
    -march=${MARCH_MODE}
    -m64
)

if(COMPILER_ID STREQUAL "clang")
    target_compile_options(zxfoundation.elf PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
    )
endif()

if(COMPILER_ID STREQUAL "gcc")
    target_compile_options(zxfoundation.elf PRIVATE
        -static-libgcc
        -Wno-array-bounds
        -fno-delete-null-pointer-checks
        -mzarch
    )
endif()

target_compile_definitions(zxfoundation.elf PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(zxfoundation.elf PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

target_compile_options(zxfoundation.elf PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

# linking
set(zxfoundation_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/link.ld" CACHE STRING "zxfoundation linker script")

set_target_properties(zxfoundation.elf PROPERTIES LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")

target_link_options(zxfoundation.elf PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -nostdlib
    -static
    --no-dynamic-linker
    -ztext
    -zmax-page-size=0x1000
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)

if(CMAKE_OBJCOPY)
    add_custom_command(
        TARGET zxfoundation.elf
        COMMAND ${CMAKE_OBJCOPY} -O binary ${CMAKE_BINARY_DIR}/zxfoundation.elf zxfoundation.krnl
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "zxfoundation::build: generating bootloader and kernel binaries"
        POST_BUILD
    )
endif()