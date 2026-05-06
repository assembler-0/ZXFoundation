# ============================================================================
# SPDX-License-Identifier: Apache-2.0 
#
# ZXFoundation (R) 26h1 kernel
#
# @file CMakeLists.txt
# @brief Kernel build configuration
# @copyright (C) 2025-2026 assembler-0
# ============================================================================

cmake_minimum_required(VERSION 3.10)
project(ZXFoundation
    LANGUAGES C ASM
)

if (NOT CMAKE_TOOLCHAIN_FILE)
	message(FATAL_ERROR "zxfoundation::build: no toolchain file specified")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/toolchain")

include(dependencies) 		  # Host dependencies
include(ccache)				  # CCache configuration
include(configuration) 		  # Build options
include(platform)       	  # Platform checks
include(standard)   	      # C/C++ Standards
include(hosttools)			  # Host tools
include(source)        		  # Core kernel sources
include(zxfoundation-compile) # Full kernel compilation
include(run)       		      # Run targets (qemu, etc.)