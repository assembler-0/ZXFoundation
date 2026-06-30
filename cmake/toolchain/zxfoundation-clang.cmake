# ZXFoundation clang toolchain for s390x

if (NOT DEFINED ENV{CLANG_VERSION})
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    set(CMAKE_ASM_COMPILER clang)
else()
    set(CMAKE_ENV_CLANG_VERSION "$ENV{CLANG_VERSION}")
    set(CMAKE_C_COMPILER clang-${CMAKE_ENV_CLANG_VERSION})
    set(CMAKE_CXX_COMPILER clang++-${CMAKE_ENV_CLANG_VERSION})
    set(CMAKE_ASM_COMPILER clang-${CMAKE_ENV_CLANG_VERSION})
endif()

set(CMAKE_LINKER ld.lld)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_OBJCOPY llvm-objcopy)
set(ZX_NM llvm-nm)
set(ZX_CXXFILT llvm-cxxfilt)
set(ZX_HOST_CC clang)

set(COMPILER_ID "clang")

set(EXTRA_KERNEL_FLAGS "")
set(EXTRA_LOADER_FLAGS "")

set(OPT_LEVEL "2" CACHE STRING "Optimization level (0, 1, 2, 3, s, z)")
set(DSYM_LEVEL "0" CACHE STRING "Debug symbol level (0, 1, 2, 3)")
set(MARCH_MODE "z13" CACHE STRING "Architecture mode, for -march and -mtune")
set(DASD_SERIAL "001842095440" CACHE STRING "Dasd serial")