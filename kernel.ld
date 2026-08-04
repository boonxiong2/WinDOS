OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS {
    . = 0;
    .text.start : { *(.text.start) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : {
        . = ALIGN(16);
        _bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }
}
