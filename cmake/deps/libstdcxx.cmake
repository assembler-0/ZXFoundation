set(LIBSTDCXX_BASE "${CMAKE_SOURCE_DIR}/deps/libstdcxx" CACHE PATH "")
set(LIBSTDCXX_INCLUDE "${LIBSTDCXX_BASE}/include")
set(LIBSTDCXX_MODS
    "${LIBSTDCXX_BASE}/src/std.cc"
    "${LIBSTDCXX_BASE}/src/std.compat.cc"
)