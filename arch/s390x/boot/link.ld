ENTRY(_start);
SECTIONS {
    . = 0x10000; /* Standard entry point for many s390x systems */

    .text : {
        KEEP(*(.text._start)); /* Ensure _start is at the very beginning */
        *(.text);
        *(.text.*);
    }

    .rodata : {
        *(.rodata);
        *(.rodata.*);
    }

    .data : {
        *(.data);
        *(.data.*);
    }

    .bss : ALIGN(4K) {
        *(COMMON);
        *(.bss);
        *(.bss.*);
    }
}
