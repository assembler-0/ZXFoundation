# compile flags and discovery

include(CheckCompilerFlag)
include(CheckLinkerFlag)

set(ZX_CXX_FLAGS "")
set(ZX_CXX_LINK_FLAGS "")
set(ZX_C_FLAGS "")
set(ZX_C_LINK_FLAGS "")

# _zx_check_flags(<COMPILE|LINK> <C|CXX> <OUT_VAR> [flag-spec...])
function(_zx_check_flags CHECK_KIND LANG OUT_VAR)
    if(NOT CHECK_KIND STREQUAL "COMPILE" AND NOT CHECK_KIND STREQUAL "LINK")
        message(FATAL_ERROR "zxfoundation::build: internal error: unknown check kind '${CHECK_KIND}'")
    endif()

    set(_ACCUM "")

    foreach(ITEM IN LISTS ARGN)
        if(ITEM STREQUAL "")
            continue()
        endif()

        string(REPLACE "|" ";" PAIR "${ITEM}")
        list(LENGTH PAIR PAIR_LEN)
        if(NOT PAIR_LEN EQUAL 2)
            message(FATAL_ERROR "zxfoundation::build: malformed flag spec '${ITEM}' (expected 'FLAG|REQUIRED' or 'FLAG|OPTIONAL')")
        endif()

        list(GET PAIR 0 FLAG)
        list(GET PAIR 1 STATUS)

        if(NOT STATUS STREQUAL "REQUIRED" AND NOT STATUS STREQUAL "OPTIONAL")
            message(FATAL_ERROR "zxfoundation::build: unknown status '${STATUS}' for flag '${FLAG}' (expected REQUIRED or OPTIONAL)")
        endif()

        string(MAKE_C_IDENTIFIER "HAS_${LANG}_${CHECK_KIND}_${FLAG}" VAR_NAME)

        if(CHECK_KIND STREQUAL "COMPILE")
            check_compiler_flag(${LANG} "${FLAG}" ${VAR_NAME})
            set(KIND_LABEL "flag")
            set(TOOL "${CMAKE_${LANG}_COMPILER}")
        else()
            check_linker_flag(${LANG} "${FLAG}" ${VAR_NAME})
            set(KIND_LABEL "link flag")
            set(TOOL "${CMAKE_LINKER}")
        endif()

        if(${VAR_NAME})
            if(NOT OUT_VAR STREQUAL "")
                list(APPEND _ACCUM "${FLAG}")
            endif()
            message(STATUS "zxfoundation::build: ${LANG} ${KIND_LABEL} '${FLAG}' supported")
        elseif(STATUS STREQUAL "REQUIRED")
            message(FATAL_ERROR "zxfoundation::build: required ${LANG} ${KIND_LABEL} '${FLAG}' is not supported by ${TOOL}")
        else()
            message(STATUS "zxfoundation::build: optional ${LANG} ${KIND_LABEL} '${FLAG}' skipped (unsupported)")
        endif()
    endforeach()

    if(NOT OUT_VAR STREQUAL "")
        set(${OUT_VAR} ${_ACCUM} PARENT_SCOPE)
    endif()
endfunction()

function(zx_check_cxx_flags)
    _zx_check_flags(COMPILE CXX ZX_CXX_FLAGS ${ARGN})
    set(ZX_CXX_FLAGS ${ZX_CXX_FLAGS} PARENT_SCOPE)
endfunction()

function(zx_check_cxx_flags_only)
    _zx_check_flags(COMPILE CXX "" ${ARGN})
endfunction()
function(zx_check_c_flags)
    _zx_check_flags(COMPILE C ZX_C_FLAGS ${ARGN})
    set(ZX_C_FLAGS ${ZX_C_FLAGS} PARENT_SCOPE)
endfunction()

set(_ZX_NEEDED_CXX_FLAGS
    "-ffreestanding|REQUIRED"
    "-fno-builtin|REQUIRED"
    "-Wall|REQUIRED"
    "-Wextra|REQUIRED"
    "-Wpedantic|REQUIRED"
    "-Werror|REQUIRED"
    "-fno-strict-aliasing|REQUIRED"
    "-fno-common|REQUIRED"
    "-fwrapv|REQUIRED"
    "-ftrivial-auto-var-init=pattern|REQUIRED"
    "-fno-omit-frame-pointer|REQUIRED"
    "-mbackchain|OPTIONAL"
    "-fstack-protector-strong|OPTIONAL"
    "-pipe|OPTIONAL"
    "-mno-packed-stack|OPTIONAL"
    "-mhard-float|REQUIRED"
    "-mvx|OPTIONAL"
    "-fno-exceptions|REQUIRED"
    "-fno-rtti|REQUIRED"
    "-nostdlib|REQUIRED"
    "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=|REQUIRED"
    "-march=${MARCH_MODE}|OPTIONAL"
    "-mtune=${MARCH_MODE}|OPTIONAL"
    "-O${OPT_LEVEL}|OPTIONAL"
    "-g${DSYM_LEVEL}|OPTIONAL"
    "-m64|REQUIRED"
)

set(_ZX_NEEDED_CXX_LTO_FLAGS "")
if(COMPILER_ID STREQUAL "gcc")
    list(APPEND _ZX_NEEDED_CXX_FLAGS
        "-static-libgcc|REQUIRED"
        "-mzarch|REQUIRED"
    )
endif()

if(COMPILER_ID STREQUAL "clang")
    list(APPEND _ZX_NEEDED_CXX_FLAGS
        "-nostdlib++|REQUIRED"
        "--target=${COMMON_TARGET_TRIPLE}|REQUIRED"
    )
    if(ENABLE_LTO)
        list(APPEND _ZX_NEEDED_CXX_LTO_FLAGS
            "-flto=full|REQUIRED"
            "-funified-lto|REQUIRED"
        )
    endif()
    list(APPEND _ZX_NEEDED_CXX_FLAGS ${_ZX_NEEDED_CXX_LTO_FLAGS})
endif()

set(_ZX_NEEDED_CXX_LINK_FLAGS
    -static
    --no-dynamic-linker
    -ztext
    --build-id=sha1
    --no-pie
    -m${TARGET_EMULATION_MODE}
    -g
)

set(_ZX_NEEDED_CXX_CLANG_LTO_LINK_FLAGS "")
if(COMPILER_ID STREQUAL "clang")
    if(ENABLE_LTO)
        list(APPEND _ZX_NEEDED_CXX_CLANG_LTO_LINK_FLAGS
            --lto=full
            --plugin=${ZX_LLVM_LTO_PLUGIN}
            -plugin-opt=3
            -plugin-opt=lto-partitions=1
            --lto-whole-program-visibility
        )
    endif()
    list(APPEND _ZX_NEEDED_CXX_LINK_FLAGS ${_ZX_NEEDED_CXX_CLANG_LTO_LINK_FLAGS})
endif()

if(COMPILER_ID STREQUAL "gcc")
    list(APPEND _ZX_NEEDED_CXX_LINK_FLAGS
        --no-warn-rwx-segments
    )
endif()

list(APPEND ZX_CXX_LINK_FLAGS ${_ZX_NEEDED_CXX_LINK_FLAGS})

# ---------------------------------------------------------------------------
# C
# ---------------------------------------------------------------------------

set(_ZX_NEEDED_C_FLAGS
    "-ffreestanding|REQUIRED"
    "-nostdlib|OPTIONAL"
    "-fno-builtin|OPTIONAL"
    "-Wall|OPTIONAL"
    "-Wextra|OPTIONAL"
    "-Wpedantic|OPTIONAL"
    "-Werror|OPTIONAL"
    "-fno-strict-aliasing|OPTIONAL"
    "-fno-common|OPTIONAL"
    "-fwrapv|OPTIONAL"
    "-ftrivial-auto-var-init=pattern|OPTIONAL"
    "-fno-stack-protector|REQUIRED"
    "-fno-omit-frame-pointer|OPTIONAL"
    "-m64|REQUIRED"
    "-msoft-float|REQUIRED"
    "-mno-vx|REQUIRED"
    "-march=${MARCH_MODE}|OPTIONAL"
    "-mtune=${MARCH_MODE}|OPTIONAL"
    "-O${OPT_LEVEL}|OPTIONAL"
    "-g${DSYM_LEVEL}|OPTIONAL"
)

set(_ZX_NEEDED_C_LTO_FLAGS "")
if(COMPILER_ID STREQUAL "clang")
    list(APPEND _ZX_NEEDED_C_FLAGS
        "-Wno-gnu-statement-expression-from-macro-expansion|REQUIRED"
        "--target=${COMMON_TARGET_TRIPLE}|REQUIRED"
    )
    if(ENABLE_LTO)
        list(APPEND _ZX_NEEDED_C_LTO_FLAGS
            "-flto=full|REQUIRED"
            "-funified-lto|REQUIRED"
        )
    endif()
    list(APPEND _ZX_NEEDED_C_FLAGS ${_ZX_NEEDED_C_LTO_FLAGS})
endif()

set(_ZX_NEEDED_C_LINK_FLAGS
    -static
    --no-dynamic-linker
    -ztext
    --build-id=sha1
    --no-pie
    -zmax-page-size=0x1000
    -g
    -m${TARGET_EMULATION_MODE}
)

if(COMPILER_ID STREQUAL "clang")
    set(_ZX_NEEDED_C_CLANG_LTO_LINK_FLAGS "")
    if(ENABLE_LTO)
        list(APPEND _ZX_NEEDED_C_CLANG_LTO_LINK_FLAGS
            --lto=full
            --plugin=${ZX_LLVM_LTO_PLUGIN}
            -plugin-opt=3
            -plugin-opt=lto-partitions=1
            --lto-whole-program-visibility
        )
    endif()
    list(APPEND _ZX_NEEDED_C_LINK_FLAGS ${_ZX_NEEDED_C_CLANG_LTO_LINK_FLAGS})
endif()

if(COMPILER_ID STREQUAL "gcc")
    set(_ZX_NEEDED_C_GCC_LINK_FLAGS "")
    list(APPEND _ZX_NEEDED_C_GCC_LINK_FLAGS
        --no-warn-rwx-segments
    )
    list(APPEND _ZX_NEEDED_C_LINK_FLAGS ${_ZX_NEEDED_C_GCC_LINK_FLAGS})
endif()

list(APPEND ZX_C_LINK_FLAGS ${_ZX_NEEDED_C_LINK_FLAGS})

if(COMPILER_ID STREQUAL "clang")
    set(CMAKE_REQUIRED_FLAGS "--target=${COMMON_TARGET_TRIPLE}")
endif()

if(DEFINED _ZX_DETECTED_CHECK_FLAGS)
    zx_check_cxx_flags_only(${_ZX_DETECTED_CHECK_FLAGS})
endif()

zx_check_cxx_flags(${_ZX_NEEDED_CXX_FLAGS})
zx_check_c_flags(${_ZX_NEEDED_C_FLAGS})

if(COMPILER_ID STREQUAL "clang")
    unset(CMAKE_REQUIRED_FLAGS)
endif()