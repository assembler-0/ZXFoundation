# ZXFoundation build configuration

if (NOT DEFINED OPT_LEVEL)
    set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
endif()

if (NOT DEFINED DSYM_LEVEL)
    set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
endif()

# 01842095440 is "00" + "int(0xafad088)" + "0" = 12 characters
# where afad088 is the first ever commit of the project.
if (NOT DEFINED DASD_SERIAL)
    set(DASD_SERIAL "001842095440" CACHE STRING "Dasd serial")
endif()

message(STATUS "zxfoundation::build: optimization level ${OPT_LEVEL} (-O${OPT_LEVEL})")
message(STATUS "zxfoundation::build: debug symbol level ${DSYM_LEVEL} (-g${DSYM_LEVEL})")
message(STATUS "zxfoundation::build: default target triple: ${COMMON_TARGET_TRIPLE}")
message(STATUS "zxfoundation::build: dasd serial registered: ${DASD_SERIAL}")
message(STATUS "zxfoundation::build: compiling for target level ${MARCH_MODE} with ${TARGET_EMULATION_MODE} target emulattion")
