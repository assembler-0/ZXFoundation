# SPDX-License-Identifier: Apache-2.0
# cmake/zxfl-compile.cmake — ZXFoundationLoader compilation

set(ZXFL_COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/dasd_io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/dasd_vtoc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/diag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/ebcdic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/panic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/parmfile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/stfle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/lowcore.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/mmu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/cpu/stsi.c
)

set(ZXFL_COMMON_FLAGS
    -ffreestanding
    -nostdlib
    -fno-builtin
    -Wall -Wextra -Wpedantic -Werror
    -fno-strict-aliasing
    -fno-common
    -fwrapv
    -ftrivial-auto-var-init=pattern
    -fno-stack-protector
    -m64 -mzarch
    -msoft-float
    -mno-vx
    -march=${MARCH_MODE}
    -mtune=${MARCH_MODE}
    -O${OPT_LEVEL}
    -g${DSYM_LEVEL}
)

set(ZXFL_COMMON_LINK_FLAGS
    -nostdlib
    -static
    -ztext
    -zmax-page-size=0x1000
    -m ${TARGET_EMULATION_MODE}
    --no-pie
    --no-warn-rwx-segment
)

# ---------------------------------------------------------------------------
# Stage 1: IPL Loader (core.zxfoundationloader00.sys)
# ---------------------------------------------------------------------------
set(ZXFL_STAGE1_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage1/head.S
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage1/entry.c
    ${ZXFL_COMMON_SOURCES}
)

add_executable(zxfl_stage1.elf ${ZXFL_STAGE1_SOURCES})
target_include_directories(zxfl_stage1.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(zxfl_stage1.elf PRIVATE ${ZXFL_COMMON_FLAGS})
target_compile_definitions(zxfl_stage1.elf PUBLIC __zxfoundation__)

set(STAGE1_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage1/stage1.ld")
set_target_properties(zxfl_stage1.elf PROPERTIES LINK_DEPENDS "${STAGE1_LINKER_SCRIPT}")
target_link_options(zxfl_stage1.elf PRIVATE
    -T ${STAGE1_LINKER_SCRIPT}
    ${ZXFL_COMMON_LINK_FLAGS}
)

# ---------------------------------------------------------------------------
# Stage 2: 64-bit Production Loader (core.zxfoundationloader01.sys)
# ---------------------------------------------------------------------------
set(ZXFL_STAGE2_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage2/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage2/entry.c
    ${ZXFL_COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/common/elfload.c
)

add_executable(zxfl_stage2.elf ${ZXFL_STAGE2_SOURCES})
target_include_directories(zxfl_stage2.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(zxfl_stage2.elf PRIVATE ${ZXFL_COMMON_FLAGS})
target_compile_definitions(zxfl_stage2.elf PUBLIC __zxfoundation__)

set(STAGE2_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/stage2/stage2.ld")
set_target_properties(zxfl_stage2.elf PROPERTIES LINK_DEPENDS "${STAGE2_LINKER_SCRIPT}")
target_link_options(zxfl_stage2.elf PRIVATE
    -T ${STAGE2_LINKER_SCRIPT}
    ${ZXFL_COMMON_LINK_FLAGS}
)

# ---------------------------------------------------------------------------
# Post-build
# ---------------------------------------------------------------------------
if(CMAKE_OBJCOPY AND BIN2REC)
    add_dependencies(zxfl_stage1.elf tools)
    add_custom_command(
        TARGET zxfl_stage1.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary zxfl_stage1.elf zxfl_stage1.bin
        COMMAND ${BIN2REC} zxfl_stage1.bin core.zxfoundationloader00.sys
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "zxfoundation::build: generating Stage 1 IPL record (00)"
    )

    add_dependencies(zxfl_stage2.elf tools)
    add_custom_command(
        TARGET zxfl_stage2.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary zxfl_stage2.elf core.zxfoundationloader01.sys
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "zxfoundation::build: generating Stage 2 flat binary (01)"
    )
endif()
