# ZXFoundation Sources Organization

file(GLOB ZX_START_SOURCES   "zxfoundation/init/*.cxxm")
file(GLOB ZX_SOURCES         "zxfoundation/*.cxxm")
file(GLOB ZX_BASE_SOURCES    "zxfoundation/base/*.cxxm")
file(GLOB ZX_MEMORY_SOURCES  "zxfoundation/memory/*.cxxm")
file(GLOB LIB_SOURCES        "lib/*.cxxm")
file(GLOB CRYPTO_SOURCES     "crypto/*.cxxm")
file(GLOB ZXFL_SOURCES       "arch/s390x/init/zxfl/*.cxxm")
file(GLOB INIT_SOURCES_S     "arch/s390x/init/*.S")
file(GLOB ARCH_TRAP_SOURCES_S "arch/s390x/trap/*.S")
file(GLOB INIT_SOURCES       "arch/s390x/init/*.cxxm")
file(GLOB ARCH_CPU_SOURCES   "arch/s390x/cpu/*.cxxm")
file(GLOB ARCH_LIB_SOURCES   "arch/s390x/lib/*.cxxm")
file(GLOB ZX_LOCKING_SOURCES "zxfoundation/locking/*.cxxm")
file(GLOB DRIVERS_SOURCES    "drivers/*.cxxm")
file(GLOB CONSOLE_SOURCES    "drivers/console/*.cxxm")

include_directories(SYSTEM
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(ZX_SOURCES_64
    ${INIT_SOURCES_S}
    ${ARCH_TRAP_SOURCES_S}
)

set(ZX_SOURCES_MODULES_64
    ${ZX_BASE_SOURCES}
    ${ZX_MEMORY_SOURCES}
    ${LIB_SOURCES}
    ${CRYPTO_SOURCES}
    ${ARCH_CPU_SOURCES}
    ${ARCH_LIB_SOURCES}
    ${ZX_LOCKING_SOURCES}
    ${ZXFL_SOURCES}
    ${INIT_SOURCES}
    ${ZX_START_SOURCES}
    ${DRIVERS_SOURCES}
    ${CONSOLE_SOURCES}
)