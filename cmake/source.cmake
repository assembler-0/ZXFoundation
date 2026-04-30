# ZXFoundation Sources Organization

file(GLOB INIT_SOURCES    "zxfoundation/init/*.c")
file(GLOB ARCH_INIT_STAGE1_SOURCES_31 "arch/s390x/init/stage1/*.S")
file(GLOB ARCH_INIT_STAGE2_SOURCES "arch/s390x/init/stage2/*.S")
file(GLOB SYS_SOURCES     "zxfoundation/sys/*.c")
file(GLOB TRAP_C_SOURCES  "arch/s390x/trap/*.c")
file(GLOB TRAP_S_SOURCES  "arch/s390x/trap/*.S")
file(GLOB CONSOLE_SOURCES "drivers/console/*.c")

include_directories(SYSTEM
    include
    ${CMAKE_SOURCE_DIR}
)

set(ZX_SOURCES_64
    ${INIT_SOURCES}
    ${ARCH_INIT_STAGE2_SOURCES}
    ${SYS_SOURCES}
    ${TRAP_C_SOURCES}
    ${TRAP_S_SOURCES}
    ${CONSOLE_SOURCES}
)