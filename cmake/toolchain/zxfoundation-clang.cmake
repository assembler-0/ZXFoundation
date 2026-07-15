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
set(CMAKE_OBJDUMP llvm-objdump)
set(ZX_NM llvm-nm)
set(ZX_CXXFILT llvm-cxxfilt)
set(ZX_HOST_CC clang)

set(COMPILER_ID "clang")

set(EXTRA_KERNEL_FLAGS "")
set(EXTRA_LOADER_FLAGS "")