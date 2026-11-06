/* Core B: power, loading of our codec images, start, clock (see hifi.h).
 *
 * Power and start follow RECHO 0.10.0 (0x03064c90 and 0x03064d9c, emulated; values only):
 * PMU_PWRDN_CON = 0 powers PD_HIGH, then the HIGHRAM and HiFi clocks and resets; core B is held
 * in reset by SOFTRST3 bit 2 while its image is copied, then released. The stock codec modules,
 * their parameter block and their mailbox protocol were dropped in 0.61 (ETUDE §9). */
#include "hifi.h"
#include "board.h"
#include "log.h"
#include "regs.h"

#define MBOX       0x40110000u
#define A2B_CMD(c) (MBOX + 0x08 + 8 * (c))
#define A2B_DATA(c) (MBOX + 0x0c + 8 * (c))
#define B2A_INTEN  (MBOX + 0x28)
#define B2A_STATUS (MBOX + 0x2c)

#define HIGHRAM0   0x01000000u
#define HIGHRAM1   0x01020000u

static uint32_t hifi_data_size;
uint32_t hifi_hz;
static int powered;

static uint32_t hifi_div = 4;                   /* core B clock divider, see hifi_set_div() */
static uint32_t hifi_own_forget(void);

static void hifi_power(void)
{
    if (powered)
        return;
    REG(PMU_BASE + 0x0c) = 0;                   /* PMU_PWRDN_CON: PD_HIGH on */
    clk_gate(9 * 16 + 2, 1);
    udelay(5);
    REG(CRU_CLKGATE(4)) = 0x170f0000;           /* HIGHRAM clocks */
    udelay(5);
    REG(CRU_CLKGATE(3)) = 0x01ff0000;
    udelay(5);
    REG(CRU_SOFTRST(3)) = 0x00040004;           /* core B held in reset */
    udelay(5);
    REG(CRU_SOFTRST(1)) = 0x40000000;           /* HiFi side resets released */
    REG(CRU_SOFTRST(3)) = 0x00400000;
    REG(CRU_SOFTRST(2)) = 0x00070000;
    clk_gate(0 * 16 + 2, 1);                    /* hclk_cal_core */
    clk_gate(6 * 16 + 14, 1);                   /* hclk_synth (MP3 accelerator) */
    clk_gate(7 * 16 + 14, 1);                   /* hclk_imdct (MP3 accelerator) */
    REG(CRU_SOFTRST(0)) = 0x40000000;           /* imdct and synth out of reset */
    REG(CRU_SOFTRST(0)) = 0x80000000;
    REG(B2A_STATUS) = 0xf;
    REG(B2A_INTEN) = 0xf;
    /* Core B at PLL / 4 (100 MHz; the stock MP3 module needs about 31), or what the player set
     * already */
    hifi_set_div(hifi_div);
    udelay(5);
    powered = 1;
    log_printf("hifi: PD_HIGH on, core B in reset, %u MHz", hifi_hz / 1000000);
}

void hifi_prepare(void)
{
    hifi_own_forget();
    hifi_power();
    hifi_stop();
    /* Fresh HiFi side for every codec, as the stock firmware stops core B (resets 0x20,
     * 0x21, 0x22, 0x32, 0x1e: PD_HIGH matrix, HIGHRAM1, HIGHRAM0, core B, mailbox), plus DMAC2
     * and HIFIACC: a module started again on what it left itself (mailbox interrupt enables,
     * DMAC2 channels) hung after its first file read (0.20 on the device). The audio output
     * is stopped by then (its ring is in HIGHRAM). */
    REG(CRU_SOFTRST(2)) = 0x000f000f;           /* matrix, HIGHRAM1, HIGHRAM0, HIFIACC */
    udelay(10);
    REG(CRU_SOFTRST(2)) = 0x000f0000;
    REG(CRU_SOFTRST(3)) = 0x00800000;           /* DMAC2 out of reset; core B stays in */
    REG(CRU_SOFTRST(1)) = 0x40000000;           /* mailbox out of reset */
    udelay(10);
}

/* Core B, DMAC2, HIFIACC and the mailbox in reset (HIGHRAM left alone: the audio ring may
 * still be playing from it) */
void hifi_stop(void)
{
    REG(CRU_SOFTRST(3)) = 0x00840084;           /* core B, DMAC2 */
    REG(CRU_SOFTRST(2)) = 0x00080008;           /* HIFIACC */
    REG(CRU_SOFTRST(1)) = 0x40004000;           /* mailbox */
}

/* A long pause (ui.c): core B in reset, the clocks of core B, its accelerators, the HIGHRAM and
 * the PD_HIGH bus gated (the RAM keeps its contents). Nothing on core A touches PD_HIGH until
 * hifi_power(), which starts over; our codec is loaded again. */

void hifi_sleep(void)
{
    if (!powered)
        return;
    hifi_stop();
    clk_gate(0 * 16 + 2, 0);                    /* hclk_cal_core */
    clk_gate(6 * 16 + 14, 0);                   /* hclk_synth */
    clk_gate(7 * 16 + 14, 0);                   /* hclk_imdct */
    REG(CRU_CLKGATE(3)) = 0x01ff01ff;
    REG(CRU_CLKGATE(4)) = 0x170f170f;           /* HIGHRAM clocks and PD_HIGH bus */
    powered = 0;
    hifi_own_forget();
    log_printf("hifi: asleep (core B in reset, PD_HIGH clocks gated)");
}

static void hifi_send(int ch, uint32_t cmd, uint32_t data)
{
    REG(A2B_CMD(ch)) = cmd;
    REG(A2B_DATA(ch)) = data;                   /* the data write sends the message */
}

/* ---- our codec images, in the pack of our own firmware image (FW1, else FW2) ---- */
#include "emmc.h"
#include "hifi_proto.h"

void *memcpy(void *d, const void *s, unsigned n);

static uint32_t sec[128];
static uint32_t own_codec, own_data_addr;
static uint8_t data_copy[1024];

uint32_t hifi_own_loaded(void)
{
    return own_codec;
}

/* Same codec again: core B, DMAC2, HIFIACC and the mailbox were reset by hifi_stop(); the code
 * in HIGHRAM0 is still there, only the initialised data is put back (the runtime clears its
 * BSS). The HIGHRAM and the matrix are not reset. */
int hifi_restart_own(void)
{
    if (!own_codec)
        return HIFI_EIMG;
    hifi_stop();
    REG(CRU_SOFTRST(3)) = 0x00800000;           /* DMAC2 out of reset; core B stays in */
    REG(CRU_SOFTRST(2)) = 0x00080000;           /* HIFIACC */
    REG(CRU_SOFTRST(1)) = 0x40000000;           /* mailbox */
    udelay(10);
    memcpy((void *)own_data_addr, data_copy, hifi_data_size);
    return 0;
}

int hifi_load_own(uint32_t codec)
{
    own_codec = 0;
    for (int k = 0; k < 2; k++) {
        uint32_t base = EMMC_FW1 + (k ? emmc.r : 0);    /* FW1, then FW2 */
        const uint8_t *h = (const uint8_t *)sec;
        if (emmc_read(base, 1, sec) || h[0x1f8] != 'R' || h[0x1ff] != 'W' || sec[0x148 / 4] != 1)
            continue;
        uint32_t off = sec[0x14c / 4], len = sec[0x150 / 4];
        if (off % 512 || len < 512 || emmc_read(base + off / 512, 1, sec) ||
            sec[0] != HP_PACK_MAGIC || sec[1] > 16)
            continue;
        for (uint32_t i = 0; i < sec[1]; i++) {
            const struct hp_pack_entry *e = (const struct hp_pack_entry *)&sec[2 + 6 * i];
            if (e->codec != codec)
                continue;
            uint32_t code_off = off + e->offset, code_sec = (e->code_size + 511) / 512;
            uint32_t d_addr = e->data_addr, d_size = e->data_size, bss_end = e->bss_end;
            if (e->code_size > 0x20000 || d_addr < HP_DATA_START || bss_end > HP_DATA_END ||
                d_addr + d_size > HP_DATA_END || e->offset % 512 || e->offset + code_sec * 512 +
                d_size > len)
                return HIFI_EIMG;
            hifi_prepare();
            /* code: whole sectors straight into HIGHRAM0; data: whole sectors too (BSS follows
             * the data and is cleared by the runtime) */
            if (emmc_read(base + code_off / 512, code_sec, (void *)HIGHRAM0) ||
                (d_size && emmc_read(base + code_off / 512 + code_sec, (d_size + 511) / 512,
                                     (void *)d_addr)))
                return HIFI_EREAD;
            hifi_data_size = d_size;
            /* initialised data kept for a restart without reloading (hifi_restart_own), when
             * it fits (Musepack's 4,992 bytes do not: reloaded each time; until 0.92 they were
             * copied all the same, over the BSS that follows, ETUDE §77) */
            own_codec = d_size <= sizeof data_copy ? codec : 0;
            own_data_addr = d_addr;
            if (own_codec)
                memcpy(data_copy, (const void *)d_addr, d_size);
            log_printf("hifi: our codec %u from %s: code %u bytes, data %u bytes", codec,
                       k ? "FW2" : "FW1", e->code_size, d_size);
            return 0;
        }
        return HIFI_EIMG;                       /* a pack, without this codec */
    }
    return HIFI_EIMG;
}

#ifdef ECHO_BPROBE
/* The probe of the test build (0.99r17, ETUDE §102): words core B reads and writes in SYSRAM
 * (BSS, and a constant of the code area) and in PMU-SRAM; the answers logged after HP_READY */
static volatile uint32_t bp_sys[2];
static volatile uint32_t bp_pmu[2] __attribute__((section(".pmusram")));
static const uint32_t bp_const = 0x434f4e53u;   /* "CONS" */
static unsigned bp_runs;

static void bp_prepare(volatile struct hp_ctl *c)
{
    bp_sys[0] = 0x5359534du;                    /* "SYSM" */
    bp_sys[1] = 0;
    bp_pmu[0] = 0x504d5553u;                    /* "PMUS" */
    bp_pmu[1] = 0;
    c->bp.sys_addr = (uint32_t)bp_sys;
    c->bp.sys_word = bp_sys[0];
    c->bp.dram_addr = (uint32_t)&bp_const;
    c->bp.dram_word = bp_const;
    c->bp.pmu_addr = (uint32_t)bp_pmu;
    c->bp.pmu_word = bp_pmu[0];
    c->bp.b_div = hifi_div;
    c->bp.req = BP_REQ;
}

static const char *bp_ok(uint32_t got, uint32_t want, int faulted)
{
    return faulted ? "FAULT" : got == want ? "ok" : "WRONG";
}

static void bp_report(volatile struct hp_ctl *c, int ready)
{
    uint32_t f = c->bp.faults, sar_a = REG(0x60000000u + 5 * 0x58);
    bp_runs++;
    if (!ready || c->bp.step != BP_DONE) {
        log_printf("bprobe: core B stopped at step %u (faults %x, cfsr %x bfar %x)", c->bp.step,
                   f, c->bp.cfsr, c->bp.bfar);
        return;
    }
    if (bp_runs == 1) {
        log_printf("bprobe: SYSRAM %08x read %s (%08x), write %s (core A reads %08x), code area "
                   "%08x read %s (%08x)", c->bp.sys_addr,
                   bp_ok(c->bp.got[BP_SYS_RD], c->bp.sys_word, f >> BP_SYS_RD & 1),
                   c->bp.got[BP_SYS_RD],
                   bp_ok(bp_sys[1], ~c->bp.sys_word, f >> BP_SYS_WR & 1), bp_sys[1],
                   c->bp.dram_addr,
                   bp_ok(c->bp.got[BP_DRAM_RD], c->bp.dram_word, f >> BP_DRAM_RD & 1),
                   c->bp.got[BP_DRAM_RD]);
        log_printf("bprobe: PMU-SRAM %08x read %s (%08x), write %s (core A reads %08x)",
                   c->bp.pmu_addr, bp_ok(c->bp.got[BP_PMU_RD], c->bp.pmu_word, f >> BP_PMU_RD & 1),
                   c->bp.got[BP_PMU_RD],
                   bp_ok(bp_pmu[1], ~c->bp.pmu_word, f >> BP_PMU_WR & 1), bp_pmu[1]);
        log_printf("bprobe: SysTick calib %08x, in 1 ms: %u on the core clock (core B at %u MHz, "
                   "div %u), %u on the reference; %u interrupts in 20 ms (reload %u); faults %x "
                   "(cfsr %x bfar %x hfsr %x)", c->bp.st_calib, c->bp.st_core,
                   hifi_hz / 1000000, c->bp.b_div, c->bp.st_ref, c->bp.st_irqs,
                   c->bp.st_load, f, c->bp.cfsr, c->bp.bfar, c->bp.hfsr);
    }
    if (bp_runs <= 40)
        log_printf("bprobe: DMA SAR read by core B %08x then %08x %u us later%s; core A now %08x",
                   c->bp.sar0, c->bp.sar1, c->bp.sar_dt / 24, f >> BP_SAR & 1 ? " (FAULT)" : "",
                   sar_a);
}
#endif

/* Releases core B with our runtime and waits for its control block to say ready */
int hifi_start_own(void)
{
    volatile struct hp_ctl *c = HP_CTL;
#ifdef ECHO_HWTEST
    uint32_t trial = c->hws.trial;              /* the SYNTH bench's trial, kept (§69) */
#endif
    for (unsigned i = 0; i < sizeof *c / 4; i++)
        ((volatile uint32_t *)c)[i] = 0;
#ifdef ECHO_HWTEST
    c->hws.trial = trial;
#endif
    c->in_end = 0xffffffffu;
#ifdef ECHO_BPROBE
    bp_prepare(c);
#endif
    REG(MBOX + 0x04) = 0xf;
    REG(B2A_STATUS) = 0xf;
    REG(CRU_SOFTRST(3)) = 0x00040000;           /* core B runs */
    for (uint32_t t0 = ticks(); ticks() - t0 < 500000;) {
        wdt_kick();
        if (c->magic == HP_MAGIC && c->state == HP_READY) {
            log_printf("hifi: our runtime ready after %u us (codec %u, version %u)", ticks() - t0,
                       c->codec, c->version);
#ifdef ECHO_BPROBE
            bp_report(c, 1);
#endif
            return 0;
        }
    }
    log_printf("hifi: our runtime not ready (magic %x state %u)", c->magic, c->state);
#ifdef ECHO_BPROBE
    bp_report(c, 0);
#endif
    return HIFI_ETIMEOUT;
}

void hifi_ring(void)
{
    hifi_send(1, 1, 0);                         /* A2B channel 1: the doorbell */
}

/* hifi_prepare() resets the HIGHRAM: our codec image is gone */
static uint32_t hifi_own_forget(void)
{
    uint32_t c = own_codec;
    own_codec = 0;
    return c;
}

/* Core B clock = PLL (400 MHz) / div, 1 <= div <= 8 (cal_core_div, CLKSEL0 bits 14:12), changed
 * on the fly (ETUDE §24). Core B's two RAMs run without their wait state at every level (GRF
 * inter_con0 bits 14 caliram = HIGHRAM0, 15 caldram = HIGHRAM1, cleared by clock_init and
 * clock_slow; the stock firmware sets it only from 300 MHz): about 22 % less load. Switching
 * that wait state while core B runs gave its bus faults (0.72, ETUDE §51). */

void hifi_set_div(uint32_t div)
{
    if (div < 1)
        div = 1;
    if (div > 8)
        div = 8;
    REG(CRU_CLKSEL0) = 0x70000000u | (div - 1) << 12;
    hifi_div = div;
    hifi_hz = (cpu_hz > 24000000u ? 400000000u : 24000000u) / div;   /* PLL on, or the crystal */
}
