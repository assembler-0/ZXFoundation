# SPDX-License-Identifier: Apache-2.0
# user/tests/CMakeLists.txt — ZXFoundation test suites (VTOC-packed,
# one binary per subsystem; the init orchestrator sequences them).

function(zxt_add_suite name)
    add_executable(${name}.elf ${name}.c common.S ${ARGN})

    target_compile_options(${name}.elf PRIVATE
        -ffreestanding
        -nostdlib
        -nostdinc
        -m64
        -march=${MARCH_MODE}
    )

    if (COMPILER_ID STREQUAL "clang")
        target_compile_options(${name}.elf PRIVATE
            --target=${COMMON_TARGET_TRIPLE}
        )
    endif()

    set_target_properties(${name}.elf
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/link.ld"
            CXX_MODULE_STD OFF
    )

    target_link_libraries(${name}.elf PRIVATE libzxf.nuclbase)

    target_link_options(${name}.elf PRIVATE
        -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
        -static
        --no-dynamic-linker
        -nostdlib
        --no-pie
        -m${TARGET_EMULATION_MODE}
    )
endfunction()

zxt_add_suite(zxtpcss zxtpcss.S)
zxt_add_suite(zxtsched)
zxt_add_suite(zxtdom)
zxt_add_suite(zxtipc)
zxt_add_suite(zxtsgn)
zxt_add_suite(zxtsurf)
