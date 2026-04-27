# ZXFoundation emulation
if(QEMU_SYSTEM_S390X)
    add_custom_target(run
        COMMAND ${QEMU_SYSTEM_S390X}
        -m 256M
        -M s390-ccw-virtio
        -cpu qemu
        -kernel $<TARGET_FILE:zxfoundation.krnl>
        -nographic
        DEPENDS zxfoundation.krnl
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "zxfoundation::build: Running zxfoundation kernel in QEMU"
    )
endif()