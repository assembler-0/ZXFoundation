# SPDX-License-Identifier: Apache-2.0
# cmake/zx-discovery.cmake — Manifest-based source discovery DSL (Refactored)

function(_zx_parse_compiler_options INPUT_LINE OUTPUT_LIST)
    # Match !compiler_id:id(opts) OR any token that doesn't have spaces
    string(REGEX MATCHALL "(!compiler_id:[a-zA-Z0-9_]+\\([^\\)]+\\)|[^ ]+)" _PARTS "${INPUT_LINE}")
    
    set(_RESULT "")
    foreach(PART ${_PARTS})
        if(PART MATCHES "^!compiler_id:([a-zA-Z0-9_]+)\\((.+)\\)$")
            set(CID "${CMAKE_MATCH_1}")
            set(COPTS "${CMAKE_MATCH_2}")
            if(CID STREQUAL "${COMPILER_ID}")
                string(REPLACE " " ";" _COPTS_LIST "${COPTS}")
                list(APPEND _RESULT ${_COPTS_LIST})
            endif()
        else()
            list(APPEND _RESULT "${PART}")
        endif()
    endforeach()
    set(${OUTPUT_LIST} "${_RESULT}" PARENT_SCOPE)
endfunction()

function(zx_discover_nucleus SOURCES MODULE_SOURCES MANIFEST_LIST ROOT_DIR)
    message(STATUS "zxfoundation::build: scanning for manifests in ${ROOT_DIR}")

    file(GLOB_RECURSE _FOUND_MANIFESTS "${ROOT_DIR}/manifest.zxd")
    
    # Ensure CMake re-runs if any manifest changes
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_FOUND_MANIFESTS})

    set(_LOCAL_SOURCES "")
    set(_LOCAL_MODULES "")
    set(_LOCAL_CHECK_FLAGS "")

    foreach(MANIFEST ${_FOUND_MANIFESTS})
        get_filename_component(MANIFEST_DIR ${MANIFEST} DIRECTORY)
        file(STRINGS "${MANIFEST}" LINES)

        set(SECTION "NONE")
        set(CURRENT_GROUP "auto")
        set(GLOBAL_DEFINES "")
        set(GLOBAL_OPTIONS "")
        set(COND_STACK "T") # T for True, F for False. Base is always True.

        message(VERBOSE "zxfoundation::build: processing ${MANIFEST}")

        foreach(LINE ${LINES})
            string(STRIP "${LINE}" LINE)

            # Skip comments and empty lines
            if(LINE MATCHES "^#" OR LINE STREQUAL "")
                continue()
            endif()

            # Get current evaluation state
            list(GET COND_STACK -1 CURRENT_COND)

            # --- Block Openers (Keywords) ---

            # !if !?VAR {
            if(LINE MATCHES "^!if[ ]*(!?)([A-Za-z0-9_]+)[ ]*\{")
                set(NEGATE "${CMAKE_MATCH_1}")
                set(VAR_NAME "${CMAKE_MATCH_2}")
                
                set(VAL FALSE)
                if(${VAR_NAME})
                    set(VAL TRUE)
                endif()
                
                if(NEGATE STREQUAL "!")
                    if(VAL)
                        set(VAL FALSE)
                    else()
                        set(VAL TRUE)
                    endif()
                endif()

                # Inherit from parent: only True if parent was True AND current is True
                if(CURRENT_COND STREQUAL "T" AND VAL)
                    list(APPEND COND_STACK "T")
                else()
                    list(APPEND COND_STACK "F")
                endif()
                continue()

            # !defines {
            elseif(LINE MATCHES "^!defines[ ]*\{")
                list(APPEND COND_STACK "${CURRENT_COND}")
                if(CURRENT_COND STREQUAL "T")
                    set(SECTION "DEFINES")
                endif()
                continue()

            # !options {
            elseif(LINE MATCHES "^!options[ ]*\{")
                list(APPEND COND_STACK "${CURRENT_COND}")
                if(CURRENT_COND STREQUAL "T")
                    set(SECTION "OPTIONS")
                endif()
                continue()

            # !files : !compile_group -> [group] {
            elseif(LINE MATCHES "^!files[ ]*:[ ]*!compile_group[ ]*->[ ]*\\[([a-z]+)\\][ ]*\{")
                set(NEW_GROUP "${CMAKE_MATCH_1}")
                list(APPEND COND_STACK "${CURRENT_COND}")
                
                if(CURRENT_COND STREQUAL "T")
                    set(SECTION "FILES")
                    set(CURRENT_GROUP "${NEW_GROUP}")
                    if(NOT CURRENT_GROUP MATCHES "^(modules|standard|auto)$")
                        message(FATAL_ERROR "zxfoundation::build: invalid compile group '${CURRENT_GROUP}' in ${MANIFEST}")
                    endif()
                endif()
                continue()

            # --- Closing Brace ---
            elseif(LINE MATCHES "^\}")
                list(LENGTH COND_STACK STACK_LEN)
                if(STACK_LEN GREATER 1)
                    list(REMOVE_AT COND_STACK -1)
                endif()
                set(SECTION "NONE")
                continue()
            endif()

            # --- Data Processing (Only if current condition is True) ---
            if(CURRENT_COND STREQUAL "F")
                continue()
            endif()

            if(SECTION STREQUAL "DEFINES")
                list(APPEND GLOBAL_DEFINES ${LINE})

            elseif(SECTION STREQUAL "OPTIONS")
                _zx_parse_compiler_options("${LINE}" _PARSED_OPTIONS)
                list(APPEND GLOBAL_OPTIONS ${_PARSED_OPTIONS})
                foreach(FLAG ${_PARSED_OPTIONS})
                    list(APPEND _LOCAL_CHECK_FLAGS "${FLAG}:REQUIRED")
                endforeach()

            elseif(SECTION STREQUAL "FILES")
                set(FILE_NAME "")
                set(FILE_FLAGS "")

                # Regex for: filename !compile_options -> [ flags ]
                if(LINE MATCHES "^([^ ]+)[ ]+!compile_options[ ]*->[ ]*\\[(.+)\\]")
                    set(FILE_NAME "${CMAKE_MATCH_1}")
                    _zx_parse_compiler_options("${CMAKE_MATCH_2}" FILE_FLAGS)
                else()
                    set(FILE_NAME "${LINE}")
                endif()

                set(FULL_PATH "${MANIFEST_DIR}/${FILE_NAME}")

                if(NOT EXISTS "${FULL_PATH}")
                    message(FATAL_ERROR "zxfoundation::build: file '${FILE_NAME}' not found in ${MANIFEST_DIR} (referenced by ${MANIFEST})")
                endif()

                # Classification by group and extension
                set(TARGET_LIST "NONE")

                if(CURRENT_GROUP STREQUAL "modules")
                    set(TARGET_LIST "MODULES")
                elseif(CURRENT_GROUP STREQUAL "standard")
                    set(TARGET_LIST "SOURCES")
                else() # auto
                    if(FULL_PATH MATCHES "\\.cxxm$")
                        set(TARGET_LIST "MODULES")
                    else()
                        set(TARGET_LIST "SOURCES")
                    endif()
                endif()

                if(TARGET_LIST STREQUAL "MODULES")
                    list(APPEND _LOCAL_MODULES "${FULL_PATH}")
                else()
                    list(APPEND _LOCAL_SOURCES "${FULL_PATH}")
                endif()

                # Apply properties
                if(GLOBAL_DEFINES)
                    set_source_files_properties("${FULL_PATH}" PROPERTIES COMPILE_DEFINITIONS "${GLOBAL_DEFINES}")
                endif()
                
                if(GLOBAL_OPTIONS)
                    set_source_files_properties("${FULL_PATH}" PROPERTIES COMPILE_OPTIONS "${GLOBAL_OPTIONS}")
                endif()

                if(FILE_FLAGS)
                    string(STRIP "${FILE_FLAGS}" FILE_FLAGS)
                    set_source_files_properties("${FULL_PATH}" PROPERTIES COMPILE_OPTIONS "${FILE_FLAGS}")
                endif()
            endif()
        endforeach()
    endforeach()

    # Propagate results
    set(${SOURCES} "${_LOCAL_SOURCES}" PARENT_SCOPE)
    set(${MODULE_SOURCES} "${_LOCAL_MODULES}" PARENT_SCOPE)
    set(${MANIFEST_LIST} "${_FOUND_MANIFESTS}" PARENT_SCOPE)

    list(LENGTH _LOCAL_SOURCES _LEN_SOURCES)
    list(LENGTH _LOCAL_MODULES _LEN_MODULES)
    list(LENGTH _FOUND_MANIFESTS _LEN_MANIFESTS)
    set(_ZX_DETECTED_CHECK_FLAGS "${_LOCAL_CHECK_FLAGS}" PARENT_SCOPE)
    message(STATUS "zxfoundation::build: found ${_LEN_SOURCES} sources and ${_LEN_MODULES} modules across ${_LEN_MANIFESTS} manifests")
endfunction()
