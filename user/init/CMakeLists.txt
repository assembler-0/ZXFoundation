# SPDX-License-Identifier: Apache-2.0
# user/init/CMakeLists.txt — ZXFoundation user-space init binary

add_executable(zxf.init init.S)

target_compile_options(zxf.init PRIVATE
    -ffreestanding
    -nostdlib
    -nostdinc
    -m64
    -march=${MARCH_MODE}
)

if (COMPILER_ID STREQUAL "clang")
    target_compile_options(zxf.init PRIVATE
        --target=${COMMON_TARGET_TRIPLE}
    )
endif()

if(ENABLE_LTO)
    if(COMPILER_ID STREQUAL "clang")
        target_compile_options(zxf.init PRIVATE -flto=thin -funified-lto)
        target_link_options(zxf.init PRIVATE --lto=thin)
    endif()
endif()

set_target_properties(zxf.init PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/link.ld"
)

target_link_options(zxf.init PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    -static
    --no-dynamic-linker
    -nostdlib
    --no-pie
    -m${TARGET_EMULATION_MODE}
)
