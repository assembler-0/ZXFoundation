# ============================================================================
# SPDX-License-Identifier: Apache-2.0 
#
# ZXFoundation (TM) 26h1 kernel
#
# @file CMakeLists.txt
# @brief Kernel build configuration
# copyright (C) 2025-2026 assembler-0
# ============================================================================

cmake_minimum_required(VERSION 3.28...3.30)
include(cmake/meta.cmake)
project(ZXFoundation
    VERSION        26.2
    HOMEPAGE_URL   "https://zxfoundation-devlog.vercel.app"
    DESCRIPTION    "Experimental C++ DGP kernel for IBM z/Architecture"
    LANGUAGES      C CXX ASM
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/toolchain")

include(configuration) 		  # Build options
include(preconf)              # Pre-configuration
include(toolchain-validate)	  # Host dependencies
include(ccache)				  # CCache configuration
include(platform)       	  # Platform checks
include(standard)   	      # C/C++ Standards
include(hosttools)			  # Host tools
include(zx-discovery)         # zx-discovery DSL interpreter
include(source)        		  # Core kernel sources
include(zxfoundation-compile) # Full kernel compilation
include(run)       		      # Run targets (mostly post-build targets)
add_subdirectory(user/init)   # Tester only, this is a kernel, remember?
