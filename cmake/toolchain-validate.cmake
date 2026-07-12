# SPDX-License-Identifier: Apache-2.0
# @file cmake/toolchain-validate.cmake

# _zx_tool_exists(VARNAME DESC)
#   Check that the tool stored in ${VARNAME} is set and executable.
#   FATAL_ERROR on failure.
function(_zx_tool_exists _var _desc)
    execute_process(
        COMMAND ${${_var}} --version
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "zxfoundation::validate: ${_desc} — ${${_var}}\n"
            "  returned exit code ${_rc}.  Check that the tool is installed\n"
            "  and marked executable."
        )
    endif()
    message(STATUS "zxfoundation::validate: tool ${_var} = ${${_var}}")
endfunction()

# _zx_find_optional(VAR DESC NAMES...)
#   find_program with the given NAMES.  Sets ${VAR} to the found location
function(_zx_find_optional _var _desc)
    find_program(${_var} NAMES ${ARGN} DOC "${_desc}")
    if (NOT ${_var})
        message(STATUS "zxfoundation:validate: ${_desc}: not found")
    endif()
endfunction()

_zx_tool_exists(CMAKE_C_COMPILER     "C compiler")
_zx_tool_exists(CMAKE_CXX_COMPILER   "C++ compiler")
_zx_tool_exists(CMAKE_ASM_COMPILER   "ASM compiler")
_zx_tool_exists(CMAKE_LINKER         "Linker")
_zx_tool_exists(CMAKE_AR             "Archiver (ar)")
_zx_tool_exists(CMAKE_RANLIB         "Ranlib")
_zx_tool_exists(CMAKE_OBJCOPY        "Objcopy")
_zx_tool_exists(CMAKE_OBJDUMP        "Objdump")

if (
    CMAKE_GENERATOR STREQUAL "Ninja" OR
    CMAKE_GENERATOR STREQUAL "Ninja Multi-Config" OR
    CMAKE_GENERATOR STREQUAL "Visual Studio 18 2026" OR
    CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022"
)
    message(STATUS "zxfoundation::validate: cmake generator = ${CMAKE_GENERATOR}")
else()
    message(WARNING "zxfoundation::validate: cmake generator = ${CMAKE_GENERATOR} may not support C++20 modules.")
endif()

# --- LTO plugin detection ---
if(ENABLE_LTO AND COMPILER_ID STREQUAL "gcc")
    message(WARNING "zxfoundation::build: gcc LTO is not supported")
    set(ENABLE_LTO OFF)
elseif(ENABLE_LTO AND COMPILER_ID STREQUAL "clang")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=LLVMgold.so
        OUTPUT_VARIABLE _zx_llvm_lto_plugin
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_zx_llvm_lto_plugin AND EXISTS "${_zx_llvm_lto_plugin}")
        set(ZX_LLVM_LTO_PLUGIN "${_zx_llvm_lto_plugin}" CACHE INTERNAL "GCC liblto_plugin path")
        message(STATUS "zxfoundation::build: llvm LTO plugin: ${ZX_LLVM_LTO_PLUGIN}")
    else()
        message(WARNING "zxfoundation::build: llvm LTO plugin not found at '${_zx_llvm_lto_plugin}' — forcing ENABLE_LTO=OFF")
        set(ENABLE_LTO OFF)
    endif()
endif()

# ZX_NM / ZX_CXXFILT — set by the toolchain file, verify they exist
set(ZX_FOUND_NM      FALSE CACHE BOOL "nm binary was found" FORCE)
set(ZX_FOUND_CXXFILT FALSE CACHE BOOL "c++filt binary was found" FORCE)

if(ZX_NM)
    execute_process(
        COMMAND ${ZX_NM} --version
        RESULT_VARIABLE _rc_nm
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_rc_nm EQUAL 0)
        set(ZX_FOUND_NM TRUE CACHE BOOL "nm binary was found" FORCE)
        message(STATUS "zxfoundation::validate: nm = ${ZX_NM}")
    else()
        message(WARNING "zxfoundation::validate: nm = ${ZX_NM} — not executable")
    endif()
else()
    message(WARNING "zxfoundation::validate: nm — ZX_NM not set in toolchain")
endif()

if(ZX_CXXFILT)
    execute_process(
        COMMAND ${ZX_CXXFILT} --version
        RESULT_VARIABLE _rc_cxxf
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_rc_cxxf EQUAL 0)
        set(ZX_FOUND_CXXFILT TRUE CACHE BOOL "c++filt binary was found" FORCE)
        message(STATUS "zxfoundation::validate: c++filt = ${ZX_CXXFILT}")
    else()
        message(WARNING "zxfoundation::validate: c++filt = ${ZX_CXXFILT} — not executable")
    endif()
else()
    message(WARNING "zxfoundation::validate: c++filt — ZX_CXXFILT not set in toolchain")
endif()

# ZX_HOST_CC — set by toolchain file, verify it exists
set(ZX_FOUND_HOST_CC FALSE CACHE BOOL "Host C compiler was found" FORCE)

if(ZX_HOST_CC)
    execute_process(
        COMMAND ${ZX_HOST_CC} --version
        RESULT_VARIABLE _rc_hcc
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_rc_hcc EQUAL 0)
        set(ZX_FOUND_HOST_CC TRUE CACHE BOOL "Host C compiler was found" FORCE)
        message(STATUS "zxfoundation::validate: host CC = ${ZX_HOST_CC}")
    else()
        message(WARNING "zxfoundation::validate: host CC = ${ZX_HOST_CC} — not executable")
    endif()
else()
    message(WARNING "zxfoundation::validate: host CC — ZX_HOST_CC not set in toolchain")
endif()

# DASDLOAD
set(ZX_FOUND_DASDLOAD FALSE CACHE BOOL "dasdload tool was found" FORCE)
_zx_find_optional(ZX_DASDLOAD_PROGRAM "dasdload" dasdload64 dasdload)
if(ZX_DASDLOAD_PROGRAM)
    set(ZX_FOUND_DASDLOAD TRUE CACHE BOOL "dasdload tool was found" FORCE)
    set(DASDLOAD "${ZX_DASDLOAD_PROGRAM}" CACHE FILEPATH "dasdload program path" FORCE)
endif()

# CCKDCSDK
set(ZX_FOUND_CCKDCDSK FALSE CACHE BOOL "cckdcdsk tool was found" FORCE)
_zx_find_optional(ZX_DCCKDCDSK_PROGRAM "cckdcdsk" cckdcdsk64 cckdcdsk)
if(ZX_DCCKDCDSK_PROGRAM)
    set(ZX_FOUND_CCKDCDSK TRUE CACHE BOOL "cckdcdsk tool was found" FORCE)
    set(CCKDCDSK "${ZX_DCCKDCDSK_PROGRAM}" CACHE FILEPATH "cckdcdsk program path" FORCE)
endif()

# DASDSER
set(ZX_FOUND_DASDSER FALSE CACHE BOOL "dasdser tool was found" FORCE)
_zx_find_optional(ZX_DASDSER_PROGRAM "dasdser" dasdser)
if(ZX_DASDSER_PROGRAM)
    set(ZX_FOUND_DASDSER TRUE CACHE BOOL "dasdser tool was found" FORCE)
    set(DASDSER "${ZX_DASDSER_PROGRAM}" CACHE FILEPATH "dasdser program path" FORCE)
endif()

# CCache
set(ZX_FOUND_CCACHE FALSE CACHE BOOL "ccache was found" FORCE)
_zx_find_optional(ZX_CCACHE_PROGRAM "ccache" ccache)
if(ZX_CCACHE_PROGRAM)
    set(ZX_FOUND_CCACHE TRUE CACHE BOOL "ccache was found" FORCE)
    set(CCACHE_PROGRAM "${ZX_CCACHE_PROGRAM}" CACHE FILEPATH "ccache program path" FORCE)
    message(STATUS "zxfoundation::validate: ccache max size: 12G")
endif()

if(ZX_FOUND_NM AND CMAKE_OBJDUMP)
    set(ZX_CAN_DUMP_ARTIFACTS TRUE CACHE BOOL "Dump artifacts" FORCE)
    message(STATUS "zxfoundation::validate: artifact-dump target available")
else()
    set(ZX_CAN_DUMP_ARTIFACTS FALSE CACHE BOOL "Dump artifacts" FORCE)
endif()

if(ZX_FOUND_NM AND ZX_FOUND_CXXFILT AND ZX_FOUND_HOST_CC)
    set(ZX_CAN_BUILD_SYMRES TRUE
        CACHE BOOL "Two-pass symbol table generation" FORCE)
    message(STATUS "zxfoundation::validate: Symbol table: nm = ${CMAKE_NM} c++filt = ${ZX_CXXFILT}")
else()
    set(ZX_CAN_BUILD_SYMRES FALSE
        CACHE BOOL "Two-pass symbol table generation" FORCE)
    message(STATUS "zxfoundation::validate: Symbol table: nm, c++filt, or host CC missing")
endif()

if(ZX_FOUND_HOST_CC)
    set(ZX_CAN_SIGN_KERNEL TRUE
        CACHE BOOL "Sign kernel segments post-link" FORCE)
    message(STATUS "zxfoundation::validate: Kernel signing: available")
else()
    set(ZX_CAN_SIGN_KERNEL FALSE
        CACHE BOOL "Sign kernel segments post-link" FORCE)
    message(STATUS "zxfoundation::validate: Kernel signing: host CC missing")
endif()

if(CMAKE_OBJCOPY AND ZX_FOUND_HOST_CC)
    set(ZX_CAN_PACK_LOADER TRUE
        CACHE BOOL "Convert loader ELF -> .sys format" FORCE)
    message(STATUS "zxfoundation::validate: Loader packaging: objcopy = ${CMAKE_OBJCOPY}")
else()
    set(ZX_CAN_PACK_LOADER FALSE
        CACHE BOOL "Convert loader ELF -> .sys format" FORCE)
    message(STATUS "zxfoundation::validate: Loader packaging: objcopy or host CC missing")
endif()

if(ZX_FOUND_DASDLOAD AND DASD_SERIAL)
    set(ZX_CAN_BUILD_DASD TRUE
        CACHE BOOL "Build sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD image: dasdload available = ${DASDLOAD}")
else()
    set(ZX_CAN_BUILD_DASD FALSE
        CACHE BOOL "Build sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD image: dasdload missing")
endif()

if(ZX_FOUND_DASDLOAD AND ZX_FOUND_CCKDCDSK)
    set(ZX_CAN_CHECK_DASD TRUE
            CACHE BOOL "Validate sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD check: cckdcdsk available = ${CCKDCDSK}")
else()
    set(ZX_CAN_CHECK_DASD FALSE
            CACHE BOOL "Validate sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD check: cckdcdsk missing")
endif()

if(ZX_FOUND_DASDLOAD AND ZX_FOUND_DASDSER)
    set(ZX_CAN_SERIAL_DASD TRUE
            CACHE BOOL "Serial sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD serial: dasdser available = ${DASDSER}")
else()
    set(ZX_CAN_SERIAL_DASD FALSE
            CACHE BOOL "Serial sysres.3390 disk image" FORCE)
    message(STATUS "zxfoundation::validate: DASD serial: dasdser missing")
endif()

if(ZX_FOUND_HOST_CC)
    set(ZX_CAN_BUILD_HOST_TOOLS TRUE
        CACHE BOOL "Build host tools (bin2rec, zxsign, zxallsyms_gen)" FORCE)
    message(STATUS "zxfoundation::validate: Host tools: available")
else()
    set(ZX_CAN_BUILD_HOST_TOOLS FALSE
        CACHE BOOL "Build host tools (bin2rec, zxsign, zxallsyms_gen)" FORCE)
    message(STATUS "zxfoundation::validate: Host tools: host CC missing")
endif()
