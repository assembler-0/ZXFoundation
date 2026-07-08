# ZXFoundation build configuration

if (NOT DEFINED OPT_LEVEL)
    set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
endif()

if (NOT DEFINED DSYM_LEVEL)
    set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
endif()

if (NOT DEFINED MAX_CPUS)
    set(MAX_CPUS "768" CACHE STRING "Maximum supported cpus")
endif()

# 7a2f41726368 is "z/Arch".encode('ascii').hex()
if (NOT DEFINED DASD_SERIAL)
    set(DASD_SERIAL "7a2f41726368" CACHE STRING "Dasd serial")
endif()

# LTO (Link-Time Optimization)
if (NOT DEFINED ENABLE_LTO)
    set(ENABLE_LTO OFF CACHE BOOL "Enable LTO")
endif()

message(STATUS "zxfoundation::build: optimization level ${OPT_LEVEL} (-O${OPT_LEVEL})")
message(STATUS "zxfoundation::build: debug symbol level ${DSYM_LEVEL} (-g${DSYM_LEVEL})")
message(STATUS "zxfoundation::build: default target triple: ${COMMON_TARGET_TRIPLE}")
message(STATUS "zxfoundation::build: dasd serial registered: ${DASD_SERIAL}")
message(STATUS "zxfoundation::build: compiling for target level ${MARCH_MODE} with ${TARGET_EMULATION_MODE} target emulattion for ${MAX_CPUS} cpu(s)")