# ZXFoundation build configuration

if(NOT DEFINED OPT_LEVEL)
    set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
endif()

if(NOT DEFINED DSYM_LEVEL)
    set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
endif()

# ---------------------------------------------------------------------------
# Sanitizer options
# ---------------------------------------------------------------------------

option(CONFIG_UBSAN
    "Enable Undefined Behavior Sanitizer instrumentation for the kernel"
    ON)
