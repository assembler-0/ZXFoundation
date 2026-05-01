# ZXFoundation emulation

if(QEMU_SYSTEM_S390X)
    add_custom_target(run
        COMMAND ${QEMU_SYSTEM_S390X}
        -m 256M
        -M s390-ccw-virtio
        -cpu z900
        -drive file=sysres.3390,if=none,id=d0,format=raw
        -device virtio-blk-ccw,drive=d0,devno=fe.0.0001,bootindex=1
        -nographic
        DEPENDS zxfoundation.krnl
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "zxfoundation::build: Running zxfoundation kernel in QEMU"
    )
endif()

if(DASDLOAD)
    add_custom_command(
        OUTPUT sysres.3390
        COMMAND ${CMAKE_COMMAND} -E rm -f sysres.3390
        COMMAND ${DASDLOAD} -z ${CMAKE_SOURCE_DIR}/scripts/sysres.conf sysres.3390
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/scripts/hercules.cnf hercules.cnf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        DEPENDS zxfoundation.krnl zxfl.elf
        COMMENT "zxfoundation::build: generating sysres.3390"
    )
    add_custom_target(dasd ALL DEPENDS sysres.3390)
endif()