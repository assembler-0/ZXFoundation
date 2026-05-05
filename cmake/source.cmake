# ZXFoundation Sources Organization

file(GLOB INIT_SOURCES    "zxfoundation/init/*.c")
file(GLOB ARCH_INIT_SOURCES_C "arch/s390x/init/*.c")
file(GLOB ARCH_INIT_SOURCES_64 "arch/s390x/init/*.S")
file(GLOB ARCH_MM_SOURCES "arch/s390x/mmu/*.c")
file(GLOB ARCH_MM_SOURCES_S "arch/s390x/mmu/*.S")
file(GLOB SYS_SOURCES     "zxfoundation/sys/*.c")
file(GLOB TRAP_C_SOURCES  "arch/s390x/trap/*.c")
file(GLOB TRAP_S_SOURCES  "arch/s390x/trap/*.S")
file(GLOB CONSOLE_SOURCES "drivers/console/*.c")
file(GLOB LIB_SOURCES     "lib/*.c")

include_directories(SYSTEM
    ${CMAKE_SOURCE_DIR}/include
)

file(GLOB CPU_SOURCES      "arch/s390x/cpu/*.c")
file(GLOB SYNC_SOURCES     "zxfoundation/sync/*.c")
file(GLOB OBJECT_SOURCES   "zxfoundation/object/*.c")
file(GLOB MEMORY_SOURCES   "zxfoundation/memory/*.c")

file(GLOB CRYPTO_SOURCES   "crypto/*.c")

set(ZX_SOURCES_64
    ${INIT_SOURCES}
    ${ARCH_INIT_SOURCES_C}
    ${ARCH_MM_SOURCES}
    ${ARCH_MM_SOURCES_S}
    ${ARCH_INIT_SOURCES_64}
    ${SYS_SOURCES}
    ${TRAP_C_SOURCES}
    ${TRAP_S_SOURCES}
    ${CONSOLE_SOURCES}
    ${LIB_SOURCES}
    ${CPU_SOURCES}
    ${SYNC_SOURCES}
    ${OBJECT_SOURCES}
    ${MEMORY_SOURCES}
    ${CRYPTO_SOURCES}
)