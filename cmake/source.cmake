# SPDX-License-Identifier: Apache-2.0

set(ZX_SOURCES_64 "")
set(ZX_SOURCES_MODULES_64 "")
set(ZX_MANIFEST_LIST "")

zx_discover_nucleus(ZX_SOURCES_64 ZX_SOURCES_MODULES_64 ZX_MANIFEST_LIST "${CMAKE_CURRENT_SOURCE_DIR}")

include_directories(SYSTEM
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/s390x/init/zxfl/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)