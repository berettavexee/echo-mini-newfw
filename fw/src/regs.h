/* RKNanoD peripheral addresses and register offsets used by the firmware.
 * Names follow the RKNanoD TRM; values were checked against the stock firmware and RECHO
 * (re/newfw/ETUDE.md). Registers of the CRU and the GRF take a write mask in bits 31:16. */
#ifndef REGS_H
#define REGS_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* Clock and reset unit */
#define CRU_BASE            0x40180000u
#define CRU_PLL_CON0        (CRU_BASE + 0x00)
#define CRU_PLL_CON1        (CRU_BASE + 0x04)
#define CRU_PLL_CON2        (CRU_BASE + 0x08)
#define CRU_MODE_CON        (CRU_BASE + 0x10)
#define CRU_CLKSEL0         (CRU_BASE + 0x14)
#define CRU_CLKSEL1         (CRU_BASE + 0x18)
#define CRU_CLKGATE(n)      (CRU_BASE + 0x80 + 4 * (n))
#define CRU_SOFTRST(n)      (CRU_BASE + 0xc0 + 4 * (n))
#define PLL_LOCK            (1u << 10)          /* in PLL_CON1 (bit 15 is rstmode) */

/* General register file: iomux at +0x20 * bank + 4 * (pin >> 3), 2 bits per pin */
#define GRF_BASE            0x50010000u
#define GRF_IOMUX(bank, pin) (GRF_BASE + 0x20 * (bank) + 4 * ((pin) >> 3))
/* Bit at 1 = no pull resistor (at 0, the pad's own pull-up or pull-down is enabled) */
#define GRF_PULL(bank, pin)  (GRF_BASE + 0x20 * (bank) + 0x10 + 4 * ((pin) >> 3))
#define GRF_INTER_CON0      (GRF_BASE + 0xc4)
#define GRF_SOC_STATUS      (GRF_BASE + 0xe8)   /* bit 1: VBUS present */
#define GRF_VBUS            (1u << 1)

/* PMU: PMU_SYS_REG3 survives a system reset; the IDB loader reads it at boot */
#define PMU_BASE            0x50020000u
#define PMU_SYS_REG3        (PMU_BASE + 0x4c)
#define BOOTTAG_LOADER_USB  0x18afu             /* loader stays in its Rockusb mode */

/* GPIO (DesignWare APB GPIO) */
#define GPIO0_BASE          0x40160000u
#define GPIO1_BASE          0x40170000u
#define GPIO2_BASE          0x50030000u
#define GPIO_DR             0x00
#define GPIO_DDR            0x04
#define GPIO_EXT            0x50

/* SARADC, 10 bits */
#define SARADC_BASE         0x400d0000u
#define SARADC_DATA         (SARADC_BASE + 0x00)
#define SARADC_STAS         (SARADC_BASE + 0x04)  /* bit 0: busy */
#define SARADC_CTRL         (SARADC_BASE + 0x08)  /* 2:0 channel, 4 power (starts a conversion) */

/* PWM0, channel n at +0x10 n (ETUDE §3.3): the backlight on channel 0 */
#define PWM0_BASE           0x400f0000u
#define PWM_CNT(n)          (PWM0_BASE + 0x10 * (n) + 0x0)
#define PWM_PERIOD(n)       (PWM0_BASE + 0x10 * (n) + 0x4)  /* in PCLK cycles */
#define PWM_DUTY(n)         (PWM0_BASE + 0x10 * (n) + 0x8)  /* the part on */
#define PWM_CTRL(n)         (PWM0_BASE + 0x10 * (n) + 0xc)
#define PWM_CTRL_ON         0x00010003u                     /* the stock firmware's value */

/* Watchdog (DesignWare WDT) */
#define WDT_BASE            0x40120000u
#define WDT_CRR             (WDT_BASE + 0x0c)
#define WDT_KICK            0x76u

/* VOP (MCU LCD interface) */
#define VOP_BASE            0x60070000u
#define VOP_CON             (VOP_BASE + 0x00)
#define VOP_TIMING          (VOP_BASE + 0x08)
#define VOP_CMD             (VOP_BASE + 0x28)
#define VOP_DATA            (VOP_BASE + 0x2c)
#define VOP_CON_BYPASS      (1u << 11)          /* one byte per DATA write */
#define VOP_CON_SPLIT2      (1u << 1)           /* one RGB565 pixel per DATA write, 2 phases */
#define GATE_HCLK_VOP       (7 * 16 + 15)       /* clock gate number (CLKGATE7 bit 15) */
#define SRST_VOP            (0 * 16 + 13)       /* soft reset number (SOFTRST0 bit 13) */

/* Cortex-M system control */
#define SCB_AIRCR           0xe000ed0cu
#define SCB_VTOR            0xe000ed08u
#define NVIC_ISER           0xe000e100u
#define NVIC_ICER           0xe000e180u
#define NVIC_ICPR           0xe000e280u
#define SYST_CSR            0xe000e010u
#define SYST_RVR            0xe000e014u
#define SYST_CVR            0xe000e018u

/* PMU-SRAM words kept across a reset */
#define PMUSRAM_FAST_MARK   0x0309ffe0u          /* set while trying 200 MHz */
#define FAST_MAGIC          0x54534146u          /* "FAST" */

#endif
