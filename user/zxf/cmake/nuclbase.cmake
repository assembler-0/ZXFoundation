# SPDX-License-Identifier: Apache-2.0
# user/zxf/cmake/nuclbase.cmake - user-facing library (system calls)

add_library(libzxf.nuclbase INTERFACE)

target_sources(libzxf.nuclbase INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/sys/inc64/nuclbase.h"
)
