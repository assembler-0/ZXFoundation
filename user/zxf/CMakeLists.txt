# SPDX-License-Identifier: Apache-2.0
# user/zxf/CMakeLists.txt - user-facing library

add_library(libzxf INTERFACE)
target_include_directories(libzxf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

include(cmake/nuclbase.cmake)