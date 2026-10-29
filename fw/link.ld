/* Main core (Cortex-M3) memory map, see ETUDE.md section 9.
 * Code is loaded at 0x03050000 (overlay entry 0). The IDB loader runs from 0x03000000, then from
 * its own copy at 0x0304a000-0x0304dfff, with its data and stack at 0x03088000-0x0308ffff, while
 * it loads us and clears our BSS: data and BSS must end before 0x0304a000 (0.34 cleared the
 * running loader, ETUDE §23.2), the code before 0x03080000. Large buffers used only once we run
 * (the DAC ring) go to NOLOAD sections the loader does not clear.
 * Data and BSS start at 0x03004000 with the vector table first; the stack is the 16 KiB of
 * SYSRAM0 below 0x03004000 (where the loader itself ran). */
MEMORY
{
    CODE (rx)  : ORIGIN = 0x03050000, LENGTH = 0x30000
    RAM  (rwx) : ORIGIN = 0x03004000, LENGTH = 0x4C000
}

ENTRY(Reset_Handler)
_estack = 0x03004000;

SECTIONS
{
    .text : {
        KEEP(*(.text.reset))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE

    .data : {
        _sdata = .;
        KEEP(*(.vectors))
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > CODE
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
    ASSERT(_ebss <= 0x03020000, "BSS reaches the DAC ring (0x03020000)")

    /* DAC ring, 192 KiB, 0x03020000-0x0304ffff: not loaded, not cleared by the loader nor by
     * the start-up code (audio.c zeroes what it uses); over the loader's copy of itself, which
     * is dead once we run */
    .dacring 0x03020000 (NOLOAD) : {
        KEEP(*(.dacring))
    }
    ASSERT(SIZEOF(.dacring) <= 0x30000, "DAC ring larger than 0x03020000-0x0304ffff")

    /* The play queue's listing, 32 KiB above the code, and the directory listing, 96 KiB up to the loader's
     * data and stack and over them (dead once we run): not loaded, not cleared (ETUDE §25) */
    .queuebuf 0x03070000 (NOLOAD) : {
        KEEP(*(.queuebuf))
    }
    ASSERT(SIZEOF(.queuebuf) <= 0x8000, "play queue larger than 32 KiB")
    .dirbuf 0x03078000 (NOLOAD) : {
        KEEP(*(.dirbuf))
    }
    ASSERT(SIZEOF(.dirbuf) <= 0x18000, "directory listing larger than 96 KiB")
    /* The queue's folders (ETUDE §79), 48 KiB of PMU-SRAM up to the log ring (0x0309c000): not
     * loaded, not cleared */
    .pmusram 0x03090000 (NOLOAD) : {
        KEEP(*(.pmusram))
    }
    ASSERT(SIZEOF(.pmusram) <= 0xc000, "queue folders reach the log ring (0x0309c000)")
    ASSERT(_sidata + SIZEOF(.data) <= 0x03070000, "code reaches the play queue (0x03070000)")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
