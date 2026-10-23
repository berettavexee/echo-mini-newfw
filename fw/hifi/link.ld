/* Core B (HiFi Cortex-M3) codec image: code and constants in HIGHRAM0, which core B sees at
 * address 0 (vector table first); data and BSS in HIGHRAM1 after the control block and the
 * stack (hifi_proto.h). The data image is copied by core A at its run address. */
MEMORY
{
    CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x20000
    RAM  (rwx) : ORIGIN = 0x01023000, LENGTH = 0x2D000
}

ENTRY(hp_reset)

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE

    .data : {
        *(.data*)
        . = ALIGN(4);
    } > RAM

    .noinit (NOLOAD) : {                /* not cleared by hp_reset() (heap.c) */
        . = ALIGN(8);
        *(.noinit*)
        . = ALIGN(4);
    } > RAM

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
