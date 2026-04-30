# ZXFoundation Sources Organization

file(GLOB INIT_SOURCES    "zxfoundation/init/*.c")
file(GLOB ARCH_INIT_SOURCES_64 "arch/s390x/init/*.S")
file(GLOB SYS_SOURCES     "zxfoundation/sys/*.c")
file(GLOB TRAP_C_SOURCES  "arch/s390x/trap/*.c")
file(GLOB TRAP_S_SOURCES  "arch/s390x/trap/*.S")
file(GLOB CONSOLE_SOURCES "drivers/console/*.c")
file(GLOB LIB_SOURCES     "lib/*.c")

include_directories(SYSTEM
    include
    ${CMAKE_SOURCE_DIR}
)

set(ZX_SOURCES_64
    ${INIT_SOURCES}
    ${ARCH_INIT_SOURCES_64}
    ${SYS_SOURCES}
    ${TRAP_C_SOURCES}
    ${TRAP_S_SOURCES}
    ${CONSOLE_SOURCES}
    ${LIB_SOURCES}
)