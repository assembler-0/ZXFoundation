/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/preprocessor/symalias.hxx
/// @brief symbol aliasing
/// @note requires __zxmacro_want_symalias=1

#pragma once

/// @name Handy macros for aliasing symbols
/// @{
#define symaliasname(fn) concat(__symalias_,fn)
#define symalias_reference(fn) __asm__(stringify(symaliasname(fn)))
#define symalias(fn) declscope(C) auto symaliasname(fn)
/// @}