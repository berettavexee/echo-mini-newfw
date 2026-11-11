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
    /* The overlays' modules (ovl.h, ETUDE §137): their code and constants not here but in their
     * overlay below; their data and BSS stay with the others */
    .text : {
        KEEP(*(.text.reset))
        *(EXCLUDE_FILE(*usb.o *msc.o *uac.o *usbmode.o) .text*)
        *(EXCLUDE_FILE(*usb.o *msc.o *uac.o *usbmode.o) .rodata*)
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

    /* Core B's input ring (hifi_proto.h, HP_IN_BASE), 96 KiB, 0x03020000-0x03037fff, also the
     * USB sound card's DAC ring (no codec runs then; our codecs' DAC ring is core B's output
     * ring in HIGHRAM1): not loaded, not cleared by the loader nor by the start-up code; over
     * the loader's copy of itself, dead once we run. 0x03038000-0x0304ffff (96 KiB): free, for
     * the screens (rework step 4, ETUDE §103) */
    .hpinring 0x03020000 (NOLOAD) : {
        . = . + 0x18000;
    }

    /* The screens' buffers in 0x03038000-0x0304ffff (0.99r37: the installer's list of long
     * names): not loaded, not cleared */
    .screenram 0x03038000 (NOLOAD) : {
        KEEP(*(.screenram))
    }
    ASSERT(SIZEOF(.screenram) <= 0x18000, "screen buffers larger than 96 KiB")

    /* The directory listing, 96 KiB above the code up to the loader's data and stack and over
     * them (dead once we run): not loaded, not cleared (ETUDE §25). The play queue's listing was
     * at 0x03070000 until 0.102 (ETUDE §127: in .screenram since, the code 32 KiB more).
     * The USB overlay (ovl.h, ETUDE §137) takes its place during a USB session (the listing read
     * again after): stored at 0x20000000 (no memory: tools/mkimg.py takes it by name into the
     * image, the loader never sees it); NOCROSSREFS: the USB code never touches the listing */
    OVERLAY 0x03078000 : NOCROSSREFS AT (0x20000000)
    {
        .dirbuf { KEEP(*(.dirbuf)) }
        .ovl_usb { *usb.o(.text* .rodata*) *msc.o(.text* .rodata*) *uac.o(.text* .rodata*)
                   *usbmode.o(.text* .rodata*) }
    }
    /* In a USB session: the overlay's code in the first 16 KiB, msc.c's transfer buffer (64 KiB)
     * after it; in an installation, update.c's buffers (2 x 8 KiB); ovl.c reads its sectors into
     * the last 512 bytes (shared.h) */
    __ovl_usb_start = ADDR(.ovl_usb);
    __ovl_usb_end = 0x03090000;
    __shared_usb = 0x03078000 + 0x4000;
    __shared_upd = 0x03078000 + 0x14000;
    ASSERT(SIZEOF(.ovl_usb) <= 0x4000, "the USB overlay reaches msc.c's buffer (shared.h)")
    ASSERT(SIZEOF(.dirbuf) <= 0x18000, "directory listing larger than 96 KiB")
    /* The queue's folders (ETUDE §79), 48 KiB of PMU-SRAM up to the log ring (0x0309c000): not
     * loaded, not cleared */
    .pmusram 0x03090000 (NOLOAD) : {
        KEEP(*(.pmusram))
    }
    ASSERT(SIZEOF(.pmusram) <= 0xc000, "queue folders reach the log ring (0x0309c000)")
    ASSERT(_sidata + SIZEOF(.data) <= 0x03078000, "code reaches the directory listing (0x03078000)")

    /* The log's formats (log.h, ETUDE §133): never loaded, at address 0 (a format's address is
     * its offset, the token the ring keeps); tools/mkimg.py stores them in the image */
    logfmt 0 (INFO) : {
        __start_logfmt = .;
        KEEP(*(logfmt))
    }
    ASSERT(SIZEOF(logfmt) < 0x10000, "log formats over 64 KiB (16-bit tokens)")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
