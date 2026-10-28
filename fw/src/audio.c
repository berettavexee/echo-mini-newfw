/* Audio output, step 5a. The sequence and register values are those RECHO 0.10.0 uses on this
 * device (observed in emulation of 0x030562cc at 44.1 kHz, values only; ETUDE §3.5, §4, §4.1,
 * §15); the code is ours. Every step is logged.
 *
 * Chain: DMAC1 channel 5 reads the ring through a circular list of 8 blocks (6 KiB each) and
 * feeds the I2S1 TX FIFO (request 2); I2S1 sends 32-bit slots, 64 SCLK per frame, MCLK =
 * 256 fs from the ACODEC PLL; the CS43131 at 0x60 and 0x66 (8-bit addresses) are set up over
 * I2C0. */
#include "audio.h"
#include "board.h"
#include "i2c.h"
#include "log.h"
#include "regs.h"

#define DAC0 0x60
#define DAC1 0x66

#define ACODEC   0x40090000u
#define I2S1     0x40030000u
#define I2S_TXCR   (I2S1 + 0x00)
#define I2S_RXCR   (I2S1 + 0x04)
#define I2S_CKR    (I2S1 + 0x08)
#define I2S_FIFOLR (I2S1 + 0x0c)
#define I2S_DMACR  (I2S1 + 0x10)
#define I2S_INTCR  (I2S1 + 0x14)
#define I2S_INTSR  (I2S1 + 0x18)
#define I2S_XFER   (I2S1 + 0x1c)
#define I2S_CLR    (I2S1 + 0x20)
#define I2S_TXDR   (I2S1 + 0x24)

#define DMAC1      0x60000000u
#define CH5        (DMAC1 + 5 * 0x58)
#define CH_SAR     (CH5 + 0x00)
#define CH_DAR     (CH5 + 0x08)
#define CH_LLP     (CH5 + 0x10)
#define CH_CTL     (CH5 + 0x18)
#define CH_CTLH    (CH5 + 0x1c)
#define CH_CFG     (CH5 + 0x40)
#define CH_CFGH    (CH5 + 0x44)
#define MASK_TFR   (DMAC1 + 0x310)
#define MASK_BLOCK (DMAC1 + 0x318)
#define CLEAR_TFR  (DMAC1 + 0x338)
#define CLEAR_BLOCK (DMAC1 + 0x340)
#define DMA_CFG    (DMAC1 + 0x398)
#define CH_EN      (DMAC1 + 0x3a0)

/* CTL: 32-bit source and destination, bursts of 8, source incrementing, destination fixed,
 * memory to peripheral, list chaining on both sides; 0x600 words (6 KiB) per block */
#define DMA_CTL    0x18909124u
#define MAX_BLOCKS 24
#define GUARD      4096                         /* bytes kept free ahead of the DMA */
#define CLEAR_ERR  (DMAC1 + 0x358)

struct lli {
    uint32_t sar, dar, llp, ctl, ctlh;
};

struct audio_state audio;
static struct lli lli[MAX_BLOCKS] __attribute__((aligned(8)));
static uint32_t block_words, blocks;

/* Rates: ACODEC PLL parameters (registers +0x204..+0x210, the last one shifted by 4), I2S CKR
 * (MCLK = 256 fs up to 96 kHz, 128 fs above) and the DACs' 0x01000b value; RECHO's table at
 * 0x0305fc88, values identical to the stock firmware's (ETUDE §4.1) */
static const struct rate {
    uint32_t hz;
    uint8_t p[4];
    uint32_t ckr;
    uint8_t idx;
} rates[] = {
    { 44100, { 0x06, 0x24, 0x19, 0x03 }, 0x00033f3f, 1 },
    { 48000, { 0x00, 0x30, 0x19, 0x0a }, 0x00033f3f, 2 },
    { 88200, { 0x06, 0x24, 0x19, 0x03 }, 0x00013f3f, 3 },
    { 96000, { 0x00, 0x30, 0x19, 0x09 }, 0x00033f3f, 4 },
    { 176400, { 0x04, 0x49, 0x19, 0x03 }, 0x00013f3f, 5 },
    { 192000, { 0x00, 0x30, 0x19, 0x09 }, 0x00013f3f, 6 },
    /* The other rates of the stock firmware (ETUDE §60), checked on the device with NEWFWRT2: its
     * ACODEC PLL tables (HIFIEC39 0x48c8c) and DAC index (function 0x0306185e: 0 for the 8 kHz
     * family, 1 for 11.025-44.1, 2 for 12-48), MCLK = PLL / 5. Its entries at 28.224 MHz (11.025,
     * 22.05 kHz) and 81.92 MHz (64, 128 kHz) gave no clock or a far too slow one: proven PLLs
     * instead, with another I2S MCLK/SCLK ratio (CKR bits 23:16 + 1) */
    { 8000, { 0x00, 0x30, 0x32, 0x08 }, 0x00073f3f, 0 },
    { 11025, { 0x06, 0x24, 0x19, 0x03 }, 0x000f3f3f, 1 },     /* 56.448 MHz, MCLK 1024 fs */
    { 12000, { 0x00, 0x18, 0x19, 0x0a }, 0x00073f3f, 2 },
    { 16000, { 0x00, 0x30, 0x32, 0x08 }, 0x00033f3f, 0 },
    { 22050, { 0x06, 0x24, 0x19, 0x03 }, 0x00073f3f, 1 },     /* 56.448 MHz, MCLK 512 fs */
    { 24000, { 0x00, 0x18, 0x19, 0x0a }, 0x00033f3f, 2 },
    { 32000, { 0x00, 0x60, 0x32, 0x08 }, 0x00033f3f, 0 },
    { 64000, { 0x00, 0x60, 0x32, 0x08 }, 0x00013f3f, 0 },     /* 40.96 MHz, MCLK 128 fs */
    { 128000, { 0x00, 0x30, 0x19, 0x09 }, 0x00023f3f, 0 },    /* 122.88 MHz, MCLK 192 fs */
};
static const struct rate *cur_rate = &rates[0];
static int standby;                             /* paused, headphone stage and ASP powered down */

static uint32_t sysram_ring[0x30000 / 4] __attribute__((section(".dacring"), aligned(32)));

static int both(uint32_t reg, uint8_t val)
{
    int r = i2c_reg_write(DAC0, reg, val);
    r |= i2c_reg_write(DAC1, reg, val);
    return r;
}

/* Clears 'mask' in PWDN_CTL (0x020000) of each DAC: read, modify, write */
static void pwdn_clear(uint8_t mask)
{
    static const uint8_t dev[2] = { DAC0, DAC1 };
    for (int i = 0; i < 2; i++) {
        uint8_t v;
        udelay(100);
        if (!i2c_reg_read(dev[i], 0x020000, &v))
            i2c_reg_write(dev[i], 0x020000, v & ~mask);
        udelay(100);
    }
}

static void read_status(const char *what, uint32_t reg)
{
    uint8_t a = 0, b = 0;
    int ra = i2c_reg_read(DAC0, reg, &a), rb = i2c_reg_read(DAC1, reg, &b);
    log_printf("audio: %s %x: dac0 %02x (%d) dac1 %02x (%d)", what, reg, a, ra, b, rb);
}

static void gpio_out(int bank, int pin, int val)
{
    iomux(bank, pin, 0);
    gpio_set(bank, pin, val);
    gpio_dir(bank, pin, 1);
}

/* DAC rails and reset: GPIO2_0, 2_14, 2_2 to 1, then GPIO2_1 low 1 ms, high 8 ms, low */
static void dac_power(void)
{
    gpio_out(2, 0, 1);
    gpio_out(2, 14, 1);
    gpio_out(2, 2, 1);
    mdelay(2);
    gpio_out(2, 1, 0);
    mdelay(1);
    gpio_set(2, 1, 1);
    mdelay(8);
    gpio_set(2, 1, 0);
    mdelay(2);
}

static void dac_setup(void)
{
    static const uint8_t dev[2] = { DAC0, DAC1 };
    static const struct { uint32_t reg; uint8_t val; } asp[] = {
        { 0x040010, 0x01 }, { 0x040011, 0x00 }, { 0x040012, 0x08 }, { 0x040013, 0x00 },
        { 0x040014, 0x1f }, { 0x040015, 0x00 }, { 0x040016, 0x3f }, { 0x040017, 0x00 },
        { 0x040018, 0x0c }, { 0x040019, 0x0a },
    }, block18[] = {
        { 0x180009, 0x2f }, { 0x18000a, 0xb4 }, { 0x18000b, 0x25 }, { 0x18000c, 0x80 },
        { 0x18000f, 0x55 }, { 0x180010, 0xfa }, { 0x180019, 0x2f }, { 0x18001a, 0xb4 },
        { 0x18001b, 0x25 }, { 0x18001c, 0x80 }, { 0x18001f, 0x4e }, { 0x180020, 0xfd },
        { 0x180029, 0x2f }, { 0x18002a, 0xb4 }, { 0x18002b, 0x25 }, { 0x18002c, 0x80 },
        { 0x18002f, 0x7c }, { 0x180030, 0xfd }, { 0x180039, 0x2f }, { 0x18003a, 0xb4 },
        { 0x18003b, 0x25 }, { 0x18003c, 0x80 }, { 0x18003f, 0x52 }, { 0x180040, 0xfe },
    };

    for (int d = 0; d < 2; d++) {
        audio.id_err[d] = 0;
        for (int i = 0; i < 6; i++)
            audio.id_err[d] |= i2c_reg_read(dev[d], 0x010000 + i, &audio.id[d][i]);
        log_printf("audio: dac%d id %02x %02x %02x %02x %02x %02x (%d)", d, audio.id[d][0],
                   audio.id[d][1], audio.id[d][2], audio.id[d][3], audio.id[d][4],
                   audio.id[d][5], audio.id_err[d]);
    }
    both(0x0b0000, 0x1f);                       /* CLASS_H_CTL */
    mdelay(80);
    gpio_out(1, 4, 1);
    both(0x020052, 0x02);                       /* CRYSTAL_SET */
    read_status("int status 1", 0x0f0000);
    both(0x0f0010, 0xe7);                       /* INT_MASK_1 */
    both(0x020000, 0xf6);                       /* PWDN_CTL */
    both(0x01000c, 0x04);                       /* SP_BITSIZE */
    for (unsigned i = 0; i < sizeof asp / sizeof *asp; i++)
        both(asp[i].reg, asp[i].val);           /* ASP: 64-bit frames, 32-bit slots */
    both(0x050000, 0x00);                       /* ASP_CH_1/2_LOC */
    both(0x050001, 0x00);
    i2c_reg_write(DAC0, 0x05000a, 0x07);        /* ASP_CH_1/2_SZ_EN: per DAC */
    i2c_reg_write(DAC0, 0x05000b, 0x07);
    i2c_reg_write(DAC1, 0x05000a, 0x0f);
    i2c_reg_write(DAC1, 0x05000b, 0x0f);
    both(0x090000, 0x02);                       /* PCM_FILT_OPT */
    both(0x090001, 0xff);                       /* PCM_VOL_B: mute */
    both(0x090002, 0xff);                       /* PCM_VOL_A: mute */
    both(0x090003, 0xec);                       /* PCM_PATH_CTL_1 */
    both(0x090004, 0x00);                       /* PCM_PATH_CTL_2 */
    for (unsigned i = 0; i < sizeof block18 / sizeof *block18; i++)
        both(block18[i].reg, block18[i].val);   /* undocumented block (both firmwares) */
    both(0x0b0000, 0x1f);
    mdelay(80);
    both(0x080000, 0x10);                       /* HP_OUT_CTL_1 */
    both(0x0d0000, 0x04);                       /* HP_DETECT */
    both(0x0d0001, 0x00);
    read_status("int status 1", 0x0f0000);
    read_status("int status 2", 0x0f0001);
    both(0x0f0010, 0x87);                       /* INT_MASK_1 */
    both(0x0f0011, 0x07);                       /* INT_MASK_2 */
    read_status("int status 1", 0x0f0000);
    both(0x010006, 0x04);                       /* SYS_CLK_CTL_1 */
    both(0x01000d, 0x02);                       /* PAD_INT_CFG */
    both(0x010010, 0x99);                       /* DXD1: undocumented registers open */
    both(0x180000, 0x45);
    both(0x010010, 0x00);
    pwdn_clear(0x40);
    both(0x010010, 0x99);
    both(0x080032, 0x20);                       /* DXD13 */
    pwdn_clear(0x10);
    mdelay(10);
    both(0x080032, 0x00);
    both(0x010010, 0x00);
    gpio_out(1, 4, 1);
    both(0x01000b, cur_rate->idx);              /* sample rate index */
    i2c_reg_write(DAC0, 0x05000a, 0x07);
    i2c_reg_write(DAC0, 0x05000b, 0x07);
    i2c_reg_write(DAC1, 0x05000a, 0x0f);
    i2c_reg_write(DAC1, 0x05000b, 0x0f);
    read_status("power down", 0x020000);
}

/* I2S1 stopped and cleared, then frame format (16 packed or 32 bits) and clock ratio */
static void i2s_config(void)
{
    REG(I2S_XFER) = 0;
    REG(I2S_CLR) = 1;
    for (uint32_t t0 = ticks(); (REG(I2S_CLR) & 1) && ticks() - t0 < 10000;)
        ;
    REG(I2S_TXCR) = audio.bits16 ? 0x000f : 0x401f;   /* VDW 16 packed (HWT 0), or 32 bits */
    REG(I2S_RXCR) = 0x401f;
    REG(I2S_CKR) = cur_rate->ckr;
    REG(I2S_INTCR) = 0x01f00102;
}

/* ACODEC PLL at the current rate (the parameters of the rate table) */
static void acodec_pll(void)
{
    REG(CRU_MODE_CON) = 0x00200020;             /* ACODEC clock from its own PLL */
    REG(ACODEC + 0x200) = 0x00;
    REG(ACODEC + 0x214) = 0x04;
    REG(ACODEC + 0x200) = 0x10;
    REG(ACODEC + 0x204) = cur_rate->p[0];
    REG(ACODEC + 0x208) = cur_rate->p[1];
    REG(ACODEC + 0x20c) = cur_rate->p[2];
    REG(ACODEC + 0x210) = (uint32_t)cur_rate->p[3] << 4;
    REG(ACODEC + 0x214) = 0x04;
    REG(ACODEC + 0x214) = 0x00;
    mdelay(5);
    REG(ACODEC + 0x200) = 0x14;
}

/* ACODEC PLL for the 44.1 kHz family (parameters 06 24 19 30, as both firmwares), I2S1 pads
 * (GPIO1_0-1_3, function 1, group A), I2S clock, then I2S1: 32-bit words, 64 SCLK per frame,
 * MCLK = 256 fs */
static void clocks_i2s(void)
{
    clk_gate(9 * 16 + 5, 1);
    for (int p = 0; p < 4; p++)
        iomux(1, p, 1);
    REG(GRF_BASE + 0xbc) = 0x20000000;
    clk_gate(0 * 16 + 14, 1);
    clk_gate(9 * 16 + 6, 1);
    acodec_pll();
    REG(CRU_BASE + 0x20) = 0x80008000;          /* CLKSEL: I2S clock source and dividers */
    REG(CRU_BASE + 0x20) = 0x007f0004;
    REG(CRU_BASE + 0x20) = 0x03000000;
    REG(CRU_BASE + 0x20) = 0x10000000;
    clk_gate(1 * 16 + 0, 1);
    clk_gate(1 * 16 + 2, 1);
    clk_gate(1 * 16 + 3, 1);
    clk_gate(8 * 16 + 2, 1);
    soft_reset(0 * 16 + 4, 0);
    i2s_config();
}

static void dma_start(void)
{
    REG(CH_EN) = 1u << (8 + 5);                 /* channel 5 off while it is reprogrammed */
    REG(CRU_CLKGATE(7)) = 0xffff0000;           /* hclk_dma and the rest of gate register 7 */
    REG(CRU_SOFTRST(1)) = 0x00010001;           /* DMAC1 reset */
    udelay(10);
    REG(CRU_SOFTRST(1)) = 0x00010000;
    udelay(10);
    /* blocks of 6 KiB for 48 KiB (as RECHO), 8 KiB for 64 KiB */
    block_words = audio.ring_size % 0x2000 ? 0x600 : 0x800;
    blocks = audio.ring_size / (block_words * 4);
    for (uint32_t i = 0; i < blocks; i++) {
        lli[i].sar = audio.ring + i * block_words * 4;
        lli[i].dar = I2S_TXDR;
        lli[i].llp = (uint32_t)&lli[(i + 1) % blocks];
        lli[i].ctl = DMA_CTL;
        lli[i].ctlh = block_words;
    }
    REG(CLEAR_TFR) = 1u << 5;
    REG(CLEAR_BLOCK) = 1u << 5;
    REG(MASK_TFR) = 1u << (8 + 5);              /* channel 5 interrupts masked: polled */
    REG(MASK_BLOCK) = 1u << (8 + 5);
    REG(CLEAR_ERR) = 1u << 5;
    REG(CH_SAR) = audio.ring;
    REG(CH_DAR) = I2S_TXDR;
    REG(CH_CFG) = 0x840;
    REG(CH_CFGH) = 0x1007;                      /* destination peripheral 2: i2s1 tx */
    REG(CH_CTL) = DMA_CTL;
    REG(CH_CTLH) = block_words;
    REG(CH_LLP) = (uint32_t)&lli[0];
    REG(DMA_CFG) = 1;
    REG(CH_EN) = 1u << (8 + 5) | 1u << 5;
}

uint32_t audio_dma_pos(void)
{
    return REG(CH_SAR) - audio.ring;
}

static uint32_t i2s_fifo(void)
{
    return REG(I2S_FIFOLR) & 0x3f;
}

static void ring_setup(void)
{
    audio.ring = (uint32_t)sysram_ring;
    audio.ring_size = sizeof sysram_ring;
    for (uint32_t i = 0; i < audio.ring_size / 4; i++)
        sysram_ring[i] = 0;
    log_printf("audio: ring %x + %u", audio.ring, audio.ring_size);
}

void audio_reset_stats(void)
{
    audio.ring_ur = audio.i2s_ur = audio.ring_lost = 0;
    audio.in_ur = 0;
    audio.min_fill = 0x7fffffff;
}

int32_t audio_fill(void)
{
    return (int32_t)(audio.wr - audio.rd);
}

void (*ramp_hook)(void);

/* Read position from the DMA, underrun counters */
void audio_update(void)
{
    uint32_t pos = audio_dma_pos(), size = audio.ring_size;

    if (pos >= size || !audio.on)
        return;
    audio.rd += (pos + size - audio.last_pos) % size;
    audio.last_pos = pos;
    if (REG(I2S_INTSR) & 2) {                   /* TX underrun */
        audio.i2s_ur++;
        audio.i2s_all++;
        REG(I2S_INTCR) |= 4;
    }
    int32_t fill = audio_fill();
    if (fill < 0) {                             /* the DMA overtook the producer */
        if (!audio.in_ur)
            audio.ring_ur++, audio.ur_all++;    /* one episode, however many polls it lasts */
        audio.in_ur = 1;
        audio.ring_lost += (uint32_t)-fill;
        audio.lost_all += (uint32_t)-fill;
        audio.wr = audio.rd;
        fill = 0;
    } else if (fill > 0)
        audio.in_ur = 0;
    if (fill < audio.min_fill)
        audio.min_fill = fill;
}

uint32_t audio_free(void)
{
    int32_t f = (int32_t)(audio.ring_size - GUARD) - audio_fill();
    return f > 0 ? (uint32_t)f & ~3u : 0;
}

/* Copies n bytes (whole frames, n <= audio_free()) at the write position */
/* ---- beeps mixed into what is written (battery warnings, ETUDE §43): while a pattern runs,
 * the music is lowered by 12 dB and a sine at -12 dBFS (5 ms fades) is added; the DAC volume
 * then applies to both. Heard about 1.1 s later (the ring ahead of the DMA). ---- */
static const int16_t beep_sine[64] = {
    0, 803, 1598, 2378, 3135, 3862, 4551, 5197, 5793, 6333, 6811, 7225, 7568, 7839, 8035, 8153,
    8192, 8153, 8035, 7839, 7568, 7225, 6811, 6333, 5793, 5197, 4551, 3862, 3135, 2378, 1598, 803,
    0, -803, -1598, -2378, -3135, -3862, -4551, -5197, -5793, -6333, -6811, -7225, -7568, -7839,
    -8035, -8153, -8192, -8153, -8035, -7839, -7568, -7225, -6811, -6333, -5793, -5197, -4551,
    -3862, -3135, -2378, -1598, -803,
};
static const struct { uint16_t hz, on_ms, off_ms, count; } beeps[] = {
    [BEEP_WARN] = { 1000, 120, 120, 3 },
    [BEEP_REMIND] = { 1000, 120, 0, 1 },
    [BEEP_OFF] = { 500, 200, 150, 2 },
};
static struct { int on, mix; uint32_t frame, on_frames, period, total, step, phase, fade; } bp;
static int32_t bp_t;                            /* tone of the 32-bit frame being written */

void audio_beep(int kind)
{
    if (!audio.on)
        return;
    bp.on_frames = beeps[kind].on_ms * audio.rate / 1000;
    bp.period = (uint32_t)(beeps[kind].on_ms + beeps[kind].off_ms) * audio.rate / 1000;
    bp.total = bp.period * beeps[kind].count;
    bp.step = (uint32_t)(((uint64_t)beeps[kind].hz << 32) / audio.rate);
    bp.fade = audio.rate / 200;
    bp.frame = bp.phase = 0;
    bp.on = 1;
}

/* The tone of the next frame (16-bit scale), 0 between the beeps */
static int32_t beep_sample(void)
{
    uint32_t f = bp.frame % bp.period;
    int32_t v = 0;
    if (f < bp.on_frames) {
        uint32_t edge = f < bp.on_frames - f ? f : bp.on_frames - f;
        v = beep_sine[bp.phase >> 26];
        if (edge < bp.fade)
            v = v * (int32_t)edge / (int32_t)bp.fade;
        bp.phase += bp.step;
    }
    if (++bp.frame >= bp.total)
        bp.on = 0;
    return v;
}

static uint32_t beep_mix(uint32_t w)
{
    if (audio.bits16) {                         /* one word: left low, right high */
        int32_t t = beep_sample();
        int32_t l = (int16_t)(w & 0xffff) / 4 + t, r = (int16_t)(w >> 16) / 4 + t;
        return ((uint32_t)l & 0xffff) | (uint32_t)r << 16;
    }
    if (!(audio.wr & 4)) {                      /* left word: a new frame */
        bp.mix = bp.on;
        if (bp.mix)
            bp_t = beep_sample();
    }
    return bp.mix ? (uint32_t)(((int32_t)w >> 2) + (bp_t << 16)) : w;
}

void audio_write(const void *src, uint32_t n)
{
    const uint32_t *s = src;
    volatile uint32_t *ring = (volatile uint32_t *)audio.ring;
    uint32_t size = audio.ring_size;
    for (uint32_t i = 0; i < n / 4; i++) {
        ring[audio.wr % size / 4] = bp.on || bp.mix ? beep_mix(s[i]) : s[i];
        audio.wr += 4;
    }
    if (!bp.on && !(audio.wr & 4))
        bp.mix = 0;
}

/* Silence ahead of the DMA up to the guard: a volume ramp with no player to feed the ring */
static void fill_silence(void)
{
    audio_update();
    if (!audio.on)
        return;
    while (audio_fill() + 4 <= (int32_t)(audio.ring_size - GUARD)) {
        sysram_ring[audio.wr % audio.ring_size / 4] = 0;
        audio.wr += 4;
    }
}

/* PCM_VOL_B and PCM_VOL_A of both DACs; with ramp, steps of 3 dB every 2 ms as RECHO */
void audio_volume(uint8_t vol, int ramp)
{
    uint8_t v = audio.vol;
    if (ramp) {
        while (v != vol) {
            if (v > vol)
                v = v - vol > 6 ? v - 6 : vol;
            else
                v = vol - v > 6 ? v + 6 : vol;
            both(0x090001, v);
            both(0x090002, v);
            for (uint32_t t0 = ticks(); ticks() - t0 < 2000;)
                if (ramp_hook)
                    ramp_hook();                /* the player keeps the ring fed */
                else
                    fill_silence();
        }
    } else {
        both(0x090001, vol);
        both(0x090002, vol);
    }
    audio.vol = vol;
}

int audio_start(uint32_t rate, int bits16)
{
    bp.on = bp.mix = 0;
    audio.bits16 = bits16;
    audio.frame_bytes = bits16 ? 4 : 8;
    uint32_t e0 = i2c_errors;

    cur_rate = &rates[0];
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++)
        if (rates[i].hz == rate)
            cur_rate = &rates[i];
    audio.rate = cur_rate->hz;
    audio.vol = 0xff;
    log_printf("audio: start at %u Hz, %u-bit frames in the ring", audio.rate, bits16 ? 16 : 32);
    dac_power();
    i2c_init();
    dac_setup();
    log_printf("audio: dac set-up done, %u i2c errors", i2c_errors - e0);
    clocks_i2s();
    log_printf("audio: acodec pll %x %x %x %x %x, i2s ckr %x txcr %x", REG(ACODEC + 0x200),
               REG(ACODEC + 0x204), REG(ACODEC + 0x208), REG(ACODEC + 0x20c),
               REG(ACODEC + 0x210), REG(I2S_CKR), REG(I2S_TXCR));
    ring_setup();
    audio.rd = audio.last_pos = 0;
    audio_paused = standby = 0;
    audio.wr = audio.ring_size - GUARD;         /* the zeroed ring: silence ahead */
    audio_reset_stats();
    dma_start();
    REG(I2S_DMACR) = 0x110;                     /* TX DMA on, watermark 16 */
    mdelay(80);
    REG(I2S_XFER) = 1;                          /* TX start */
    mdelay(20);
    gpio_out(2, 0, 0);                          /* as RECHO once running (unmute, maybe) */
    REG(I2S_INTCR) |= 4;                        /* underrun flag from the start-up cleared */
    log_printf("audio: running: dma sar %x ch_en %x cfg %x, i2s fifo %u intsr %x, %u i2c errors",
               REG(CH_SAR), REG(CH_EN), REG(DMA_CFG), i2s_fifo(), REG(I2S_INTSR),
               i2c_errors - e0);
    audio.i2c_errors = i2c_errors - e0;
    audio.on = 1;
    return audio.i2c_errors ? -1 : 0;
}

static void dac_standby(int on);

void audio_stop(void)
{
    if (!audio.on)
        return;
    audio_volume(0xff, 1);
    gpio_set(2, 0, 1);
    REG(I2S_XFER) = 0;
    REG(I2S_DMACR) = 0;
    REG(CH_EN) = 1u << (8 + 5);                 /* channel 5 off */
    if (!standby)
        dac_standby(1);                         /* audio_start() sets them up from scratch */
    audio.on = 0;
    log_printf("audio: stopped, %u i2c errors in total", i2c_errors);
}

/* Another rate or frame size with the DACs left powered and set up (ETUDE §22): only what
 * depends on the rate changes, as between two full start-ups (the ACODEC PLL, the I2S ratio
 * and format, the DACs' rate index 0x01000b), muted around it; the ring restarts from its
 * zeroed start.
 * Returns 0, or -1 when the output is off (then audio_start is needed). */
int audio_change_rate(uint32_t rate, int bits16)
{
    if (!audio.on)
        return -1;
    uint32_t t0 = ticks(), e0 = i2c_errors;
    uint8_t vol = audio.vol;
    both(0x090001, 0xff);                       /* mute */
    both(0x090002, 0xff);
    REG(I2S_DMACR) = 0;
    REG(CH_EN) = 1u << (8 + 5);                 /* channel 5 off */
    REG(I2S_XFER) = 0;
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++)
        if (rates[i].hz == rate)
            cur_rate = &rates[i];
    audio.rate = cur_rate->hz;
    audio.bits16 = bits16;
    audio.frame_bytes = bits16 ? 4 : 8;
    uint32_t t1 = ticks();
    acodec_pll();
    i2s_config();
    both(0x01000b, cur_rate->idx);              /* the DACs' sample rate index */
    uint32_t t2 = ticks();
    for (uint32_t i = 0; i < 8192 / 4; i++)     /* the start only: the producer writes 1 KiB */
        ((volatile uint32_t *)audio.ring)[i] = 0;   /* ahead of the DMA, at once */
    audio.rd = audio.last_pos = 0;
    audio.wr = audio.ring_size - GUARD;
    audio_reset_stats();
    dma_start();
    REG(I2S_DMACR) = 0x110;
    for (uint32_t t = ticks(); i2s_fifo() < 16 && ticks() - t < 5000;)
        ;                                       /* TX FIFO filled by the DMA */
    REG(I2S_XFER) = 1;
    REG(I2S_INTCR) |= 4;
    uint32_t t3 = ticks();
    both(0x090001, vol);
    both(0x090002, vol);
    audio.vol = vol;
    log_printf("audio: rate changed to %u Hz, %u-bit frames in %u us (mute %u, pll+i2s+dac %u, "
               "ring+dma %u us), %u i2c errors", audio.rate, bits16 ? 16 : 32, ticks() - t0,
               t1 - t0, t2 - t1, t3 - t2, i2c_errors - e0);
    read_status("int status 1", 0x0f0000);
    read_status("int status 2", 0x0f0001);
    return 0;
}

/* Pause: the DACs muted, then the I2S stops asking for data, so the DMA waits where it is and
 * nothing in the ring is lost; resume in the reverse order */
int audio_paused;
static uint32_t t_paused;

/* Headphone amplifier and serial port of both DACs powered down (PWDN_CTL a6 -> f6), or back
 * up with the sequence of dac_setup() (DXD13 around the HP power-up). The I2S stays stopped,
 * the DMA waits in place in the ring: playing goes on exactly where it stopped. */
static void dac_standby(int on)
{
    if (on) {
        gpio_set(2, 0, 1);                      /* as audio_stop() */
        both(0x020000, 0xf6);
    } else {
        both(0x010010, 0x99);
        both(0x180000, 0x45);
        both(0x010010, 0x00);
        pwdn_clear(0x40);
        both(0x010010, 0x99);
        both(0x080032, 0x20);
        pwdn_clear(0x10);
        mdelay(10);
        both(0x080032, 0x00);
        both(0x010010, 0x00);
    }
    standby = on;
    log_printf("audio: dacs %s", on ? "powered down (long pause)" : "powered up");
}

/* Called from the UI loop: 10 s into a pause, the DACs are powered down */
void audio_power_service(void)
{
    if (audio.on && audio_paused && !standby && ticks() - t_paused > 10000000)
        dac_standby(1);
}

void audio_pause(int on)
{
    if (!audio.on || on == audio_paused)
        return;
    uint8_t vol = audio.vol;
    if (on) {
        t_paused = ticks();
        both(0x090001, 0xff);
        both(0x090002, 0xff);
        mdelay(2);
        REG(I2S_XFER) = 0;
    } else {
        if (standby)
            dac_standby(0);
        REG(I2S_XFER) = 1;
        REG(I2S_INTCR) |= 4;                    /* the underrun flag of the stop cleared */
        gpio_set(2, 0, 0);
        both(0x090001, vol);
        both(0x090002, vol);
    }
    audio.vol = vol;
    audio_paused = on;
    log_printf("audio: %s", on ? "paused" : "resumed");
}

/* A skip: what is queued for the DACs is dropped. 64 KiB after the DMA are zeroed (370 ms at
 * 44.1 kHz / 16 bits): while the next track opens, the DMA plays silence, not what is left of
 * older laps of the ring; the next frames go 2 KiB after the DMA. The underrun counters start
 * again (a skip is not an underrun). */
void audio_flush(void)
{
    if (!audio.on)
        return;
    bp.on = bp.mix = 0;                         /* a beep queued is dropped with the rest */
    audio_update();
    uint32_t wr = (audio.rd + 256) & ~7u;
    uint32_t n = audio.ring_size / 2 < 65536 ? audio.ring_size / 2 : 65536;
    for (uint32_t i = 0; i < n; i += 4)
        ((volatile uint32_t *)audio.ring)[(wr + i) % audio.ring_size / 4] = 0;
    audio.wr = wr + 2048;
    audio_reset_stats();
}
