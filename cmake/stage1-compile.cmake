# ZXFoundation stage1 IPL record compilation

add_executable(stage1.elf ${ARCH_INIT_STAGE1_SOURCES_31})

target_compile_options(stage1.elf PRIVATE
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
    target_compile_options(stage1.elf PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
        -m31
        -march=z900
    )
endif()

if(COMPILER_ID STREQUAL "gcc")
    target_compile_options(stage1.elf PRIVATE
        -static-libgcc
        -Wno-array-bounds
        -fno-delete-null-pointer-checks
        -mesa
        -m31
        -march=z900
    )
endif()

target_compile_definitions(stage1.elf PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(stage1.elf PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
    $<$<COMPILE_LANGUAGE:ASM>:-Wa,-march=z900>
)

target_compile_options(stage1.elf PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

# Linking
set(zxfoundation_stage1_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/stage1/stage1.ld" CACHE STRING "stage1 linker script")

set_target_properties(stage1.elf PROPERTIES LINK_DEPENDS "${zxfoundation_stage1_LINKER_SCRIPT}")

target_link_options(stage1.elf PRIVATE
    -T ${zxfoundation_stage1_LINKER_SCRIPT}
    -nostdlib
    -static
    -ztext
    -zmax-page-size=0x1000
    -melf_s390
    --no-pie
    -g
)

if(CMAKE_OBJCOPY AND BIN2REC)
    add_dependencies(stage1.elf tools)
    add_custom_command(
        TARGET stage1.elf
        COMMAND ${CMAKE_OBJCOPY} -O binary stage1.elf stage1.bin
        COMMAND ${BIN2REC} stage1.bin ipltext.obj
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "zxfoundation::build: generating IPL1 record (24 bytes)"
        POST_BUILD
    )
endif()