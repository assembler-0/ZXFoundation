# ZXFoundation build configuration

if (NOT DEFINED OPT_LEVEL)
    set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
endif()

if (NOT DEFINED DSYM_LEVEL)
    set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
endif()

message(STATUS "zxfoundation::build: optimization level ${OPT_LEVEL} (-O${OPT_LEVEL})")
message(STATUS "zxfoundation::build: debug symbol level ${DSYM_LEVEL} (-g${DSYM_LEVEL})")
message(STATUS "zxfoundation::build: default target triple: ${COMMON_TARGET_TRIPLE}")
message(STATUS "zxfoundation::build: compiling for target level ${MARCH_MODE} with ${TARGET_EMULATION_MODE} target emulattion")
