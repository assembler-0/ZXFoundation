# ZXFoundation linking & compilation

add_executable(zxfoundation.krnl
    ${US_SOURCES}
)

target_compile_options(zxfoundation.krnl PRIVATE
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
    -m64
    -march=arch9
)

if(COMPILER_ID STREQUAL "clang")
    target_compile_options(zxfoundation.krnl PRIVATE
        --target=${TARGET_TRIPLE}
    )
endif()

target_compile_definitions(zxfoundation.krnl PUBLIC
    $<$<COMPILE_LANGUAGE:C>:__zxfoundation__>
)

target_compile_options(zxfoundation.krnl PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-D__zxfoundation__>
)

# optimization
target_compile_options(zxfoundation.krnl PRIVATE
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

# linking
set(zxfoundation_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/boot/link.ld" CACHE STRING "zxfoundation linker script")

set_target_properties(zxfoundation.krnl PROPERTIES LINK_DEPENDS "${zxfoundation_LINKER_SCRIPT}")

target_link_options(zxfoundation.krnl PRIVATE
    -T ${zxfoundation_LINKER_SCRIPT}
    -nostdlib
    -static
    --no-dynamic-linker
    -ztext
    -zmax-page-size=0x1000
    --no-pie -g
    -m${TARGET_EMULATION_MODE}
)
