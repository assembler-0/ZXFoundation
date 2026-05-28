# ZXFoundation Sources Organization

file(GLOB INIT_SOURCES    "zxfoundation/init/*.c")
file(GLOB ARCH_INIT_SOURCES_C "arch/s390x/init/*.c")
file(GLOB ARCH_INIT_SOURCES_64 "arch/s390x/init/*.S")
file(GLOB ARCH_S390X_SOURCES "arch/s390x/*.c")
file(GLOB ARCH_MM_SOURCES "arch/s390x/mmu/*.c")
file(GLOB ARCH_TIME_SOURCES "arch/s390x/time/*.c")
file(GLOB ARCH_MM_SOURCES_S "arch/s390x/mmu/*.S")
file(GLOB SYS_SOURCES     "zxfoundation/sys/*.c")
list(REMOVE_ITEM SYS_SOURCES
    "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.c"
)
file(GLOB TRAP_C_SOURCES  "arch/s390x/trap/*.c")
file(GLOB TRAP_S_SOURCES  "arch/s390x/trap/*.S")
file(GLOB IRQ_SOURCES     "zxfoundation/sys/irq/*.c")
file(GLOB CONSOLE_SOURCES "drivers/console/*.c")
file(GLOB LIB_SOURCES     "lib/*.c")
file(GLOB LIB_SOURCES_S   "lib/*.S")
file(GLOB ARCH_LIB_SOURCES "arch/s390x/lib/*.c")
file(GLOB ARCH_LIB_SOURCES_S "arch/s390x/lib/*.S")
file(GLOB LIBSSP_SOURCES  "lib/libssp/*.c")

include_directories(SYSTEM
    ${CMAKE_SOURCE_DIR}/include
)

file(GLOB CPU_SOURCES      "arch/s390x/cpu/*.c")
file(GLOB CPU_SOURCES_S    "arch/s390x/cpu/*.S")
file(GLOB SYNC_SOURCES     "zxfoundation/sync/*.c")
file(GLOB OBJECT_SOURCES   "zxfoundation/object/*.c")
file(GLOB MEMORY_SOURCES   "zxfoundation/memory/*.c")
file(GLOB SCHED_SOURCES    "zxfoundation/sched/*.c")
file(GLOB TIME_SOURCES     "zxfoundation/time/*.c")
file(GLOB ARCH_TIME_SOURCES "arch/s390x/time/*.c")
file(GLOB CRYPTO_SOURCES   "crypto/*.c")

# ---------------------------------------------------------------------------
# libubsanrt — only compiled when CONFIG_UBSAN is enabled
# ---------------------------------------------------------------------------
if (CONFIG_UBSAN)
    file(GLOB UBSAN_SOURCES "lib/libubsanrt/*.c")
else()
    set(UBSAN_SOURCES "")
endif()

set(ZX_SOURCES_64
    ${INIT_SOURCES}
    ${ARCH_INIT_SOURCES_C}
    ${ARCH_TIME_SOURCES}
    ${TRAP_C_SOURCES}
    ${TRAP_S_SOURCES}
    ${ARCH_S390X_SOURCES}
    ${ARCH_MM_SOURCES}
    ${ARCH_MM_SOURCES_S}
    ${ARCH_LIB_SOURCES}
    ${ARCH_LIB_SOURCES_S}
    ${LIBSSP_SOURCES}
    ${ARCH_INIT_SOURCES_64}
    ${SYS_SOURCES}
    ${IRQ_SOURCES}
    ${CONSOLE_SOURCES}
    ${LIB_SOURCES}
    ${LIB_SOURCES_S}
    ${CPU_SOURCES}
    ${CPU_SOURCES_S}
    ${SYNC_SOURCES}
    ${OBJECT_SOURCES}
    ${MEMORY_SOURCES}
    ${SCHED_SOURCES}
    ${TIME_SOURCES}
    ${ARCH_TIME_SOURCES}
    ${CRYPTO_SOURCES}
    ${UBSAN_SOURCES}
    "arch/s390x/cpu/ipi.c"
)

set(ZX_SOURCES_64_NO_STUB ${ZX_SOURCES_64})
list(APPEND ZX_SOURCES_64 "${CMAKE_BINARY_DIR}/zxallsyms_data.c")