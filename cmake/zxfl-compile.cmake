# ZXFoundation ZXFL bootloader compilation

set(ZXFL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/zxfl_ipl.S
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/zxfl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/diag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/ebcdic.c
)

add_executable(zxfl.elf ${ZXFL_SOURCES})

target_compile_options(zxfl.elf PRIVATE
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
)

if(COMPILER_ID STREQUAL "clang")
    target_compile_options(zxfl.elf PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
        -m31
    )
endif()

if(COMPILER_ID STREQUAL "gcc")
    target_compile_options(zxfl.elf PRIVATE
        -static-libgcc
        -Wno-array-bounds
        -fno-delete-null-pointer-checks
        -mesa
        -m31
    )
endif()

target_compile_definitions(zxfl.elf PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(zxfl.elf PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

target_compile_options(zxfl.elf PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

# Linking
set(zxfoundation_zxfl_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/zxfl.ld" CACHE STRING "zxfl linker script")

set_target_properties(zxfl.elf PROPERTIES LINK_DEPENDS "${zxfoundation_zxfl_LINKER_SCRIPT}")

target_link_options(zxfl.elf PRIVATE
    -T ${zxfoundation_zxfl_LINKER_SCRIPT}
    -nostdlib
    -static
    -ztext
    -zmax-page-size=0x1000
    -melf_s390
    --no-pie
    --no-warn-rwx-segment
    -g
)

if(CMAKE_OBJCOPY AND BIN2REC)
    add_dependencies(zxfl.elf tools)
    add_custom_command(
        TARGET zxfl.elf
        COMMAND ${CMAKE_OBJCOPY} -O binary zxfl.elf zxfl.bin
        COMMAND ${BIN2REC} zxfl.bin ipltext.obj
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "zxfoundation::build: generating ZXFL bootloader"
        POST_BUILD
    )
endif()