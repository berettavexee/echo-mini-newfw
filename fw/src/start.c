/* Vector table, reset and fault handlers.
 *
 * The IDB loader copies code, data and BSS of overlay entries 0 to 2, then sets VTOR, SP and
 * PC from the vector table found at the start of the data of entry 0 (0x03004000), with
 * r0 = vector table, r1 = SP, r2 = reset address. The table therefore lives in .vectors, at
 * the start of .data. The data are copied again from their load address inside the code, and
 * the BSS cleared, so that the firmware does not depend on what the loader did. */
#include <stdint.h>
#include "board.h"
#include "regs.h"

extern uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
extern int main(void);
void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

uint32_t boot_regs[4];          /* r0-r3 at entry, from the loader */
uint32_t boot_tag;              /* PMU_SYS_REG3 at entry */

__attribute__((section(".vectors"), used))
void (*const vectors[48])(void) = {
    (void (*)(void))_estack,
    Reset_Handler,
    Default_Handler,            /* NMI */
    HardFault_Handler,
    HardFault_Handler,          /* MemManage */
    HardFault_Handler,          /* BusFault */
    HardFault_Handler,          /* UsageFault */
    0, 0, 0, 0,
    Default_Handler,            /* SVCall */
    Default_Handler,            /* DebugMon */
    0,
    Default_Handler,            /* PendSV */
    SysTick_Handler,
    [16 ... 20] = Default_Handler,
    [16 + 5] = OTG_IRQHandler,  /* the USB controller (interrupt mode: usb_irq) */
    [22 ... 47] = Default_Handler,
};

__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    __asm volatile (
        "cpsid i\n"
        "ldr   sp, =_estack\n"
        "mov   r4, r0\n"
        "mov   r5, r1\n"
        "mov   r6, r2\n"
        "mov   r7, r3\n"
        /* copy .data from its load address, clear .bss */
        "ldr   r0, =_sidata\n"
        "ldr   r1, =_sdata\n"
        "ldr   r2, =_edata\n"
        "1: cmp r1, r2\n"
        "bhs   2f\n"
        "ldr   r3, [r0], #4\n"
        "str   r3, [r1], #4\n"
        "b     1b\n"
        "2: ldr r1, =_sbss\n"
        "ldr   r2, =_ebss\n"
        "movs  r3, #0\n"
        "3: cmp r1, r2\n"
        "bhs   4f\n"
        "str   r3, [r1], #4\n"
        "b     3b\n"
        "4: ldr r0, =boot_regs\n"
        "stmia r0, {r4-r7}\n"
        "b     reset_c\n");
}

static __attribute__((noreturn, used)) void reset_c(void)
{
    REG(SCB_VTOR) = (uint32_t)vectors;
    boot_tag = REG(PMU_SYS_REG3);
    REG(SYST_CSR) = 0;
    for (int i = 0; i < 8; i++) {               /* whatever the loader left enabled or pending */
        REG(NVIC_ICER + 4 * i) = 0xffffffff;
        REG(NVIC_ICPR + 4 * i) = 0xffffffff;
    }
    __asm volatile ("cpsie i");
    main();
    for (;;)
        ;
}

/* An unexpected interrupt is disabled and ignored; other exceptions show the fault screen */
void Default_Handler(void)
{
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr >= 16) {
        uint32_t irq = ipsr - 16;
        REG(NVIC_ICER + 4 * (irq >> 5)) = 1u << (irq & 31);
        return;
    }
    fault_screen(0, ipsr);
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "tst   lr, #4\n"
        "ite   eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mrs   r1, ipsr\n"
        "b     fault_screen\n");
}
