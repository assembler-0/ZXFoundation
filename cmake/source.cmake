# SPDX-License-Identifier: Apache-2.0
# @file cmake/source.cmake
# @brief Kernel source discovery and aggregation.
#
# Output variables:
#   ZX_SOURCES_64          — regular source files (no stub, no generated data)
#   ZX_SOURCES_MODULES_64  — C++ module interface files
#   ZX_SOURCES_64_NO_STUB  — regular + module sources (for pass1 target)
#
# NOTE: Symbol table source (zxallsyms_stub.cxx or zxallsyms_data.cxx)
#       is added by each kernel target individually.  Do not append
#       generated data here.

set(ZX_SOURCES_64 "")
set(ZX_SOURCES_MODULES_64 "")
set(ZX_MANIFEST_LIST "")

zx_discover_nucleus(ZX_SOURCES_64 ZX_SOURCES_MODULES_64 ZX_MANIFEST_LIST
    "${CMAKE_CURRENT_SOURCE_DIR}")

# Remove the two-pass stub from the main source list (it is used explicitly
# by the pass1 target or the single-pass fallback kernel).
list(REMOVE_ITEM ZX_SOURCES_64
    "${CMAKE_SOURCE_DIR}/zxfoundation/sys/zxallsyms_stub.cxx"
)

# ZX_SOURCES_64_NO_STUB = all regular sources + modules (used by pass1).
# The final kernel target adds its symbol table separately.
set(ZX_SOURCES_64_NO_STUB ${ZX_SOURCES_64})
list(APPEND ZX_SOURCES_64_NO_STUB ${ZX_SOURCES_MODULES_64})

list(LENGTH ZX_SOURCES_64         _len_src)
list(LENGTH ZX_SOURCES_MODULES_64 _len_mod)
list(LENGTH ZX_SOURCES_64_NO_STUB _len_nostub)
if(_len_src EQUAL 0 OR _len_mod EQUAL 0 OR _len_nostub EQUAL 0)
    message(FATAL_ERROR "zxfoundation::build: no sources found for nucleus target")
endif()