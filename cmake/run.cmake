# ZXFoundation run targets

if(DASDLOAD)
    add_custom_command(
        OUTPUT sysres.3390
        COMMAND ${CMAKE_COMMAND} -E rm -f sysres.3390
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/etc.zxfoundation.parm etc.zxfoundation.parm
        COMMAND ${DASDLOAD} -z ${CMAKE_SOURCE_DIR}/scripts/sysres.conf sysres.3390
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/hercules.cnf hercules.cnf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS core.zxfoundation.nucleus zxfl_stage1.elf zxfl_stage2.elf ${CMAKE_SOURCE_DIR}/scripts/sysres.conf
        COMMENT "zxfoundation::build: generating sysres.3390"
    )
    add_custom_target(dasd ALL DEPENDS sysres.3390)
endif()