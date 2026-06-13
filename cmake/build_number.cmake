# SPDX-License-Identifier: Apache-2.0
#
# Invoked at build time via:
#   cmake -DSOURCE_DIR=<project root>
#         -DZXFoundation_Release=<release>
#         -DZXFoundation_Copyright_Date=<date>
#         -DZXFoundation_Host_Build_Platform=<platform>
#         -P cmake/build_number.cmake

foreach(_var SOURCE_DIR ZXFoundation_Release ZXFoundation_Copyright_Date ZXFoundation_Host_Build_Platform)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "build_number.cmake: required variable -D${_var} not provided")
    endif()
endforeach()

set(_bn_file "${SOURCE_DIR}/BUILD_NUMBER")

if(EXISTS "${_bn_file}")
    file(READ "${_bn_file}" _bn_raw)
    string(STRIP "${_bn_raw}" _bn_str)
    if(NOT _bn_str MATCHES "^[0-9]+$")
        message(WARNING "build_number.cmake: BUILD_NUMBER '${_bn_str}' is non-integer; resetting to 0")
        set(_bn_str "0")
    endif()
    math(EXPR ZXFoundation_Build "${_bn_str} + 1")
else()
    set(ZXFoundation_Build "1")
endif()

file(WRITE "${_bn_file}" "${ZXFoundation_Build}")
message(STATUS "zxfoundation::build: build number: ${ZXFoundation_Build}")

file(READ "${SOURCE_DIR}/zxfoundation/base/config.cxxm.in" _tmpl)
string(CONFIGURE "${_tmpl}" _out @ONLY)

set(_out_path "${SOURCE_DIR}/zxfoundation/base/config.cxxm")
if(EXISTS "${_out_path}")
    file(READ "${_out_path}" _existing)
    if(_existing STREQUAL _out)
        return()
    endif()
endif()

file(WRITE "${_out_path}" "${_out}")
