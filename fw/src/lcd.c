/* 320 x 170 panel on the VOP (MCU i8080 interface, 8-bit bus).
 *
 * Bus pins: D0-D7 = GPIO0_16..23, WR = GPIO0_24, RS = GPIO0_25, CS = GPIO0_15 (VOP function 1),
 * RD = GPIO0_12 (GPIO, only used to read the panel id). Reset = GPIO2_15.
 * Commands go through the VOP in bypass mode (every write to VOP_DATA sends one byte); pixels
 * through its FIFO, fed by DMA from strips drawn in RAM, or by the CPU in bypass mode (the
 * fallback; high byte first). */
#include "lcd.h"
#include "board.h"
#include "font.h"
#include "regs.h"
#include "log.h"
#ifdef ECHO_LCDCAL
#include "i18n.h"
#include "keys.h"
#endif

static uint32_t lcd_id;
static int lcd_variant;
int lcd_ready;

#define ROW_OFFSET 35   /* the 170 visible lines start at line 35 of the controller */

/* ---- the output (ETUDE §134): DMAC1 feeding the VOP's FIFO, or the CPU (bypass) ----
 * A transfer is started and left to run: the next one, any command to the panel, a change of
 * clock and the DAC's DMAC1 reset first wait for its end (lcd_sync). DMAC1 channel 0 (the DAC's
 * is 5, of higher priority), request 14 (VOP), 32-bit words of two RGB565 pixels; the VOP sends
 * each word as four bytes in the order its mode says (lcd_dma_mode, VOP_CON). A transfer not
 * done in time switches the output to the CPU for good (lcd_lost: the screen redrawn). */
#define DMAC1       0x60000000u
#define DCH         0
#define D_SAR       (DMAC1 + DCH * 0x58 + 0x00)
#define D_DAR       (DMAC1 + DCH * 0x58 + 0x08)
#define D_LLP       (DMAC1 + DCH * 0x58 + 0x10)
#define D_CTL       (DMAC1 + DCH * 0x58 + 0x18)
#define D_CTLH      (DMAC1 + DCH * 0x58 + 0x1c)
#define D_CFG       (DMAC1 + DCH * 0x58 + 0x40)
#define D_CFGH      (DMAC1 + DCH * 0x58 + 0x44)
#define D_MASK_TFR  (DMAC1 + 0x310)
#define D_MASK_BLK  (DMAC1 + 0x318)
#define D_MASK_ERR  (DMAC1 + 0x330)
#define D_CLR_TFR   (DMAC1 + 0x338)
#define D_CLR_BLK   (DMAC1 + 0x340)
#define D_CLR_ERR   (DMAC1 + 0x358)
#define D_DMA_CFG   (DMAC1 + 0x398)
#define D_CH_EN     (DMAC1 + 0x3a0)
/* CTL: 32-bit words both sides, source incremented (D_SFIX: fixed), destination fixed, memory
 * to peripheral with the DMAC as flow controller, blocks chained; the burst length and the AHB
 * masters from the variant (dma_var) */
#define D_CTL_BASE  0x00100124u
#define D_MSIZE(m)  ((uint32_t)(m) << 11 | (uint32_t)(m) << 14)   /* bursts of 1 << (m + 1) */
#define D_SMS(m)    ((uint32_t)(m) << 25)                         /* source on master m + 1 */
#define D_DMS(m)    ((uint32_t)(m) << 23)                         /* destination: master m + 1 */
#define D_SFIX      0x00000400u
#define D_LLP_EN    0x18000000u
#define BLOCK_WORDS 2048u
#define DMA_TIMEOUT 50000u                      /* us: a whole frame takes 8 ms */

/* the VOP's modes for the FIFO: four phases (a word, four bytes), with the half-words swapped or
 * not, the bytes swapped or not (TRM 15.6.2: two pixels a word, half-words swapped). 0: no DMA,
 * the CPU writes each byte (bypass); 1: the one checked on the device (ETUDE §135), the default;
 * 2 to 4: for the test build's screen test only (ECHO_LCDCAL) */
static const uint32_t dma_con[5] = { 0, 0x16, 0x06, 0x36, 0x26 };

/* How a transfer is set up (ETUDE §135): VOP_START written or not, the AHB masters of the source
 * and of the destination, the burst length. The first, checked on the device: no VOP_START, the
 * source on master 2, the destination on master 1, bursts of 16 (as RECHO, read in its image).
 * The others only in the test build, which logs them all (dma_diag); 0.103d1 used the last. */
struct dma_var {
    uint8_t start, sms, dms, msize;
};
static const struct dma_var dma_vars[] = {
    { 0, 1, 0, 3 },
#ifdef ECHO_LCDCAL
    { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 0, 0, 0, 1 }, { 1, 1, 0, 3 }, { 1, 0, 1, 1 },
#endif
};
#define DMA_VARS    (int)(sizeof dma_vars / sizeof dma_vars[0])
static int dma_var;
int lcd_dma_mode = 1;
int lcd_lost;
struct lcd_dma_stats lcd_dma;

struct lli {
    uint32_t sar, dar, llp, ctl, ctlh;
};
static struct lli lli[(LCD_W * LCD_H / 2 + BLOCK_WORDS - 1) / BLOCK_WORDS];
static uint32_t fill_word;                      /* a fill's two pixels, read again and again */
static int dma_busy;
static uint32_t dma_t0;

static void lcd_set_timing(uint32_t hclk);

static void dma_fail(const char *why)
{
    uint32_t vs = REG(VOP_STATUS), vi = REG(VOP_INT_STATUS), en = REG(D_CH_EN), sar = REG(D_SAR);
    REG(D_CH_EN) = 1u << (8 + DCH);             /* our channel off */
    REG(VOP_SRT) = 1;
    lcd_set_timing(cpu_hz);
    REG(VOP_CON) = VOP_CON_BYPASS;
    lcd_dma.fails++;
    log_printf("lcd: DMA mode %d failed (%s; VOP status %x int %x, DMA en %x sar %x): the CPU "
               "writes the screen from now on", lcd_dma_mode, why, vs, vi, en, sar);
    lcd_dma_mode = 0;
    dma_busy = 0;
    lcd_lost = 1;
}

/* The transfer in flight, if any, finished; the VOP back in bypass for the CPU */
void lcd_sync(void)
{
    if (!dma_busy)
        return;
    uint32_t t = ticks();
    while (REG(D_CH_EN) & 1u << DCH)            /* the channel's last word into the FIFO */
        if (ticks() - dma_t0 > DMA_TIMEOUT) {
            dma_fail("DMA still running");
            return;
        }
    /* The FIFO still drains into the panel: without VOP_START the VOP's "working" bit never
     * rises (0.103d2c, lcddiag), so a fixed time before commands go out in bypass mode (RECHO
     * waits 12 us, ETUDE §135); then "working" for the test build's variants with VOP_START */
    udelay(20);
    while (REG(VOP_STATUS) & 1)
        if (ticks() - dma_t0 > DMA_TIMEOUT) {
            dma_fail("VOP still working");
            return;
        }
    uint32_t w = ticks() - t;
    lcd_dma.wait_us += w;
    if (w > lcd_dma.wait_max)
        lcd_dma.wait_max = w;
    REG(VOP_CON) = VOP_CON_BYPASS;
    dma_busy = 0;
}

static inline void cmd(uint8_t c)
{
    lcd_sync();
    REG(VOP_CMD) = c;
}

static inline void dat(uint8_t d)
{
    REG(VOP_DATA) = d;
}

/* Panel set-up tables: command, number of data bytes, data. 0xff = delay of 120 ms. */
static const uint8_t init_a[] = {
    0x11, 0xff,
    0x36, 1, 0x60,
    0x3a, 1, 0x05,
    0xa4, 2, 0x44, 0x44,
    0xb2, 5, 0x0c, 0x0c, 0x00, 0x33, 0x33,
    0xb7, 1, 0x35,
    0xbb, 1, 0x1a,
    0xc0, 1, 0x2c,
    0xc2, 1, 0x01,
    0xc3, 1, 0x0b,
    0xc4, 1, 0x20,
    0xc6, 1, 0x0f,
    0xd0, 2, 0xa4, 0xa1,
    0xd6, 1, 0xa1,
    0xe0, 14, 0xf0, 0x06, 0x0b, 0x07, 0x07, 0x24, 0x2e, 0x32, 0x46, 0x37, 0x13, 0x13, 0x2d, 0x33,
    0xe1, 14, 0xf0, 0x02, 0x06, 0x09, 0x08, 0x05, 0x29, 0x44, 0x42, 0x38, 0x14, 0x14, 0x2a, 0x30,
    0x2a, 4, 0x00, 0x00, 0x00, 0xef,
    0x2b, 4, 0x00, 0x23, 0x00, 0xcc,
    0x21, 0,
    0x29, 0,
    0x2c, 0,
};

static const uint8_t init_b[] = {
    0x11, 0xff,
    0xdf, 4, 0x5a, 0x69, 0x02, 0x01,
    0x36, 1, 0x60,
    0x3a, 1, 0x05,
    0xb2, 5, 0x0b, 0x0b, 0x00, 0x33, 0x33,
    0xb7, 1, 0x75,
    0xbb, 1, 0x28,
    0xc0, 1, 0x2c,
    0xc2, 1, 0x01,
    0xc3, 1, 0x1f,
    0xc5, 1, 0x2a,
    0xc6, 1, 0x13,
    0xd0, 1, 0xa7,
    0xd0, 2, 0xa4, 0xa1,
    0xd6, 1, 0xa1,
    0xe0, 14, 0xf0, 0x05, 0x0a, 0x06, 0x06, 0x03, 0x2b, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2b, 0x32,
    0xe1, 14, 0xf0, 0x08, 0x0c, 0x0b, 0x09, 0x24, 0x2b, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2f, 0x37,
    0x21, 0,
    0x29, 0,
    0x2c, 0,
};

static void send_table(const uint8_t *t, unsigned len)
{
    for (unsigned i = 0; i < len;) {
        cmd(t[i++]);
        unsigned n = t[i++];
        if (n == 0xff) {
            mdelay(120);
            continue;
        }
        while (n--)
            dat(t[i++]);
    }
}

static void panel_reset(uint32_t settle_ms)
{
    iomux(PIN_LCD_RESET, 0);
    gpio_dir(PIN_LCD_RESET, 1);
    gpio_set(PIN_LCD_RESET, 1);
    mdelay(50);
    gpio_set(PIN_LCD_RESET, 0);
    mdelay(150);
    gpio_set(PIN_LCD_RESET, 1);
    mdelay(settle_ms);
}

/* Bus pins as GPIO: RDDID (0x04), then a dummy byte and 3 id bytes, read with RD low. */
static uint32_t panel_read_id(void)
{
    uint32_t id = 0;

    iomux(0, 15, 0);                            /* CS low for the whole exchange */
    gpio_dir(0, 15, 1);
    gpio_set(0, 15, 0);
    for (int p = 16; p < 24; p++) {
        iomux(0, p, 0);
        gpio_dir(0, p, 1);
    }
    iomux(0, 12, 0);                            /* RD high */
    gpio_dir(0, 12, 1);
    gpio_set(0, 12, 1);
    iomux(0, 24, 0);                            /* WR low, RS low: command */
    gpio_dir(0, 24, 1);
    gpio_set(0, 24, 0);
    iomux(0, 25, 0);
    gpio_dir(0, 25, 1);
    gpio_set(0, 25, 0);
    REG(GPIO0_BASE + GPIO_DR) = (REG(GPIO0_BASE + GPIO_DR) & ~0x00ff0000u) | (0x04u << 16);
    mdelay(1);
    gpio_set(0, 24, 1);                         /* WR rising edge latches the command */

    for (int p = 16; p < 24; p++)
        gpio_dir(0, p, 0);
    gpio_set(0, 25, 1);                         /* RS high: data */
    mdelay(1);
    for (int i = 0; i < 4; i++) {
        gpio_set(0, 12, 0);
        mdelay(1);
        id = id << 8 | ((REG(GPIO0_BASE + GPIO_EXT) >> 16) & 0xff);
        gpio_set(0, 12, 1);
    }
    gpio_dir(0, 12, 0);
    return id;
}

static void bus_to_vop(void)
{
    for (int p = 16; p < 26; p++)
        iomux(0, p, 1);
    iomux(0, 15, 1);
    iomux(0, 12, 0);
    gpio_dir(0, 12, 0);
}

/* Timing of the MCU interface from the bus clock, as the stock firmware computes it:
 * n = hclk / 8 MHz, CS-to-strobe = n / 8, strobe width = n / 6, strobe-to-CS = n / 8. */
static void lcd_set_timing(uint32_t hclk)
{
    uint32_t n = hclk / 8000000, csrw = n >> 3, rwpw = n / 6, rwcs = n >> 3;

    if (csrw > 0x3f)
        csrw = 0xf;
    if (rwpw == 0)
        rwpw = 1;
    if (rwpw > 0x3f)
        rwpw = 0x3f;
    if (rwcs == 0)
        rwcs = 1;
    if (rwcs > 0x1f)
        rwcs = 0x1f;
    REG(VOP_TIMING) = csrw << 12 | rwpw << 5 | rwcs;
}

static void window(int x0, int y0, int x1, int y1)
{
    y0 += ROW_OFFSET;
    y1 += ROW_OFFSET;
    cmd(0x2a);
    dat(x0 >> 8);
    dat(x0);
    dat(x1 >> 8);
    dat(x1);
    cmd(0x2b);
    dat(y0 >> 8);
    dat(y0);
    dat(y1 >> 8);
    dat(y1);
    cmd(0x2c);
}

/* w x h pixels into the window (x, y): n words from src (fixed: the same word each time), by
 * DMA; returns 0, or -1 when the DMA is off (the caller writes them) */
static int dma_send(int x, int y, int w, int h, const void *src, int fixed)
{
    uint32_t words = (uint32_t)(w * h) / 2;
    if (!lcd_dma_mode || (w * h) & 1 || !words)
        return -1;
    window(x, y, x + w - 1, y + h - 1);         /* (waits for the transfer before) */
    if (!lcd_dma_mode)                          /* (it failed meanwhile) */
        return -1;
    const struct dma_var *v = &dma_vars[dma_var];
    REG(VOP_INT_CLEAR) = 7;
    REG(VOP_FIFO_WM) = 8u << 8 | 8u;
    /* The VOP counts the pixels in lines of LCD_SIZE's width; a line of an odd number of pixels
     * ends in the middle of a word, and every pixel after it went one place off (0.103d2c on the
     * device: names drawn with their pixels swapped in pairs). The panel wraps the rows itself
     * (its window): an odd width is told as lines of h pixels, h even here (w * h is) */
    uint32_t lw = (uint32_t)(w & 1 ? h : w), lh = (uint32_t)(w & 1 ? w : h);
    REG(VOP_LCD_SIZE) = (lh - 1) << 12 | (lw - 1);
    for (uint32_t i = 0; (REG(VOP_STATUS) & 1) && i < 100000; i++)
        ;                                       /* its mode changed only idle (TRM 15.6.1) */
    udelay(1);
    REG(VOP_CON) = dma_con[lcd_dma_mode];
    if (v->start)
        REG(VOP_START) = 1;
    uint32_t a = (uint32_t)(uintptr_t)src, n = 0;
    uint32_t ctl = D_CTL_BASE | D_MSIZE(v->msize) | D_SMS(v->sms) | D_DMS(v->dms);
    while (words) {
        uint32_t k = words < BLOCK_WORDS ? words : BLOCK_WORDS;
        words -= k;
        lli[n].sar = a;
        lli[n].dar = VOP_DATA;
        lli[n].ctl = ctl | (fixed ? D_SFIX : 0) | (words ? D_LLP_EN : 0);
        lli[n].ctlh = k;
        lli[n].llp = words ? (uint32_t)(uintptr_t)&lli[n + 1] : 0;
        if (!fixed)
            a += 4 * k;
        n++;
    }
    REG(CRU_CLKGATE(7)) = 0xffff0000;           /* DMAC1's clock (as the DAC's start) */
    REG(D_CH_EN) = 1u << (8 + DCH);
    REG(D_CLR_TFR) = 1u << DCH;
    REG(D_CLR_BLK) = 1u << DCH;
    REG(D_CLR_ERR) = 1u << DCH;
    REG(D_MASK_TFR) = 1u << (8 + DCH);          /* polled */
    REG(D_MASK_BLK) = 1u << (8 + DCH);
    REG(D_MASK_ERR) = 1u << (8 + DCH);
    REG(D_SAR) = lli[0].sar;
    REG(D_DAR) = VOP_DATA;
    REG(D_CFG) = 0x800;                         /* lowest priority; source: software */
    REG(D_CFGH) = 0x7007;                       /* destination: request 14 (VOP) */
    REG(D_CTL) = lli[0].ctl | D_LLP_EN;
    REG(D_CTLH) = lli[0].ctlh;
    REG(D_LLP) = (uint32_t)(uintptr_t)&lli[0];
    REG(D_DMA_CFG) = 1;
    REG(D_CH_EN) = 1u << (8 + DCH) | 1u << DCH;
    dma_busy = 1;
    dma_t0 = ticks();
    lcd_dma.xfers++;
    lcd_dma.bytes += (uint32_t)(w * h) * 2;
    return 0;
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;
    lcd_sync();                                 /* (fill_word may be in use) */
    fill_word = (uint32_t)color << 16 | color;
    if (!dma_send(x, y, w, h, &fill_word, 1))
        return;
    window(x, y, x + w - 1, y + h - 1);
    for (int n = w * h; n; n--) {
        dat(color >> 8);
        dat(color);
    }
}

/* ---- strips: a line of text is drawn into a strip of RAM (8 rows at most), then sent; one
 * strip drawn while the other is sent ---- */
#define STRIP_H 8
static uint16_t strip_buf[2][STRIP_H * LCD_W] __attribute__((aligned(4)));
static int strip_cur;
static struct {
    int x0, x1, y0, y1;                         /* the strip: [x0, x1) x [y0, y1) */
    uint16_t *p;
} st;

static void strip_begin(int x0, int x1, int y0, int y1, uint16_t bg)
{
    st.x0 = x0;
    st.x1 = x1;
    st.y0 = y0;
    st.y1 = y1;
    st.p = strip_buf[strip_cur];
    uint32_t n = (uint32_t)((x1 - x0) * (y1 - y0)), v = (uint32_t)bg << 16 | bg;
    uint32_t *q = (uint32_t *)(void *)st.p;
    for (uint32_t i = 0; i < (n + 1) / 2; i++)  /* (its pixels all drawn after: a guard) */
        q[i] = v;
}

static void strip_end(void)
{
    int w = st.x1 - st.x0, h = st.y1 - st.y0;
    if (!dma_send(st.x0, st.y0, w, h, st.p, 0)) {
        strip_cur ^= 1;                         /* the other one, while this one goes */
        return;
    }
    window(st.x0, st.y0, st.x1 - 1, st.y1 - 1);
    const uint16_t *p = st.p;
    for (int n = w * h; n; n--) {
        uint16_t c = *p++;
        dat(c >> 8);
        dat(c);
    }
}

static int glyph_width(const uint8_t *g)
{
    return g[0];                                /* the advance */
}

/* A glyph record (font.h) in a line of 16 px times 'sc' (1 or 2: each pixel a square): baseline
 * at row font_ascent; only the columns in [cl, cr) of the screen, and the strip's part. Only the
 * glyph's pixels are written: the strip was filled with the background (strip_begin, the same
 * colour as the text's), and the cells of a line never overlap. A row of the glyph is read as one
 * 32-bit word, then only its set bits are visited (count leading zeros). ETUDE §136: until
 * 0.103d4 every pixel of the cell was written, bg or fg, a sixth of the cost of a line. */
static void draw_glyph_cols(int x, int y, const uint8_t *g, uint16_t fg, int sc, int cl, int cr)
{
    int adv = g[0], w = g[1], h = g[2], xo = (int8_t)g[3], yo = (int8_t)g[4];
    int bpr = (w + 7) >> 3, top = font_ascent - yo - h;  /* row of the glyph's first line */
    int c0 = x, c1 = x + adv * sc;
    if (c0 < cl)
        c0 = cl;
    if (c1 > cr)
        c1 = cr;
    if (c0 < st.x0)
        c0 = st.x0;
    if (c1 > st.x1)
        c1 = st.x1;
    int r0 = y + top * sc, r1 = r0 + h * sc;    /* the rows with glyph lines, in the cell */
    if (r0 < y)
        r0 = y;
    if (r1 > y + FONT_LINE * sc)
        r1 = y + FONT_LINE * sc;
    if (r0 < st.y0)
        r0 = st.y0;
    if (r1 > st.y1)
        r1 = st.y1;
    if (c1 <= c0 || r1 <= r0)
        return;
    uint32_t wmask = w >= 32 ? 0xffffffffu : ~(0xffffffffu >> w);
    int stride = st.x1 - st.x0, gx0 = x + xo * sc;   /* screen column of bit 31 */
    const uint8_t *bits = g + 5;
    uint16_t *d = st.p + (r0 - st.y0) * stride - st.x0;  /* d[c]: column c of row r */
    for (int r = r0; r < r1; r++, d += stride) {
        int gy = (sc == 1 ? r - y : (r - y) >> 1) - top;
        const uint8_t *row = bits + gy * bpr;
        uint32_t v = (uint32_t)row[0] << 24;
        if (bpr > 1) {
            v |= (uint32_t)row[1] << 16;
            if (bpr > 2)
                v |= (uint32_t)row[2] << 8 | (bpr > 3 ? row[3] : 0);
        }
        v &= wmask;
        while (v) {
            int i = __builtin_clz(v);
            v &= ~(0x80000000u >> i);
            int c = gx0 + i * sc;
            if (c >= c0 && c < c1)
                d[c] = fg;
            if (sc == 2 && c + 1 >= c0 && c + 1 < c1)
                d[c + 1] = fg;
        }
    }
}

static int draw_glyph(int x, int y, const uint8_t *g, uint16_t fg, int sc)
{
    int adv = g[0] * sc;
    if (!adv || x + adv > LCD_W)
        return 0;
    draw_glyph_cols(x, y, g, fg, sc, x, x + adv);
    return adv;
}

/* Runs 'body' once a strip over [x0, x1) x [y, y + h) (each run draws what falls in it) */
#define STRIPS(x0, x1, y, h, bg, body)                                                          \
    do {                                                                                        \
        int s_x0_ = (x0) < 0 ? 0 : (x0), s_x1_ = (x1) > LCD_W ? LCD_W : (x1);                 \
        int s_y1_ = (y) + (h) > LCD_H ? LCD_H : (y) + (h);                                      \
        for (int s_y_ = (y) < 0 ? 0 : (y); s_x1_ > s_x0_ && s_y_ < s_y1_; s_y_ += STRIP_H) {   \
            strip_begin(s_x0_, s_x1_, s_y_, s_y_ + STRIP_H < s_y1_ ? s_y_ + STRIP_H : s_y1_,   \
                        bg);                                                                    \
            body;                                                                               \
            strip_end();                                                                        \
        }                                                                                       \
    } while (0)

/* A line laid out once (its code points and their x), then drawn into each of its strips
 * (ETUDE §136: until 0.103d4 each strip decoded the UTF-8 and measured the line again). Code
 * points, not glyph pointers: a glyph met later in the line may take an earlier one's place in
 * the font's cache. A line of more glyphs than LAY_MAX: the rest left out (320 pixels hold
 * fewer). */
#define LAY_MAX 160
static struct {
    uint32_t cp;
    int16_t x;
} lay[LAY_MAX];
static int lay_n;

static void lay_add(uint32_t cp, int x)
{
    if (lay_n < LAY_MAX) {
        lay[lay_n].cp = cp;
        lay[lay_n].x = (int16_t)x;
        lay_n++;
    }
}

/* each glyph in its cell (cl < 0), or all clipped to [cl, cr) */
static void lay_draw(int y, uint16_t fg, int sc, int cl, int cr)
{
    for (int i = 0; i < lay_n; i++) {
        const uint8_t *g = font_glyph(lay[i].cp);
        int x = lay[i].x;
        draw_glyph_cols(x, y, g, fg, sc, cl < 0 ? x : cl, cl < 0 ? x + g[0] * sc : cr);
    }
}

/* s scrolled left by 'off' pixels in [x, xmax): s, a gap of 'gap' pixels, s again */
static void scroll_layout(int x, const char *s, int xmax, int off, int gap, int sc)
{
    int pos = x - off;
    lay_n = 0;
    for (int copy = 0; copy < 2 && pos < xmax; copy++) {
        for (const char *t = s; *t && pos < xmax;) {
            uint32_t c = utf8_next(&t);
            int adv = font_glyph(c)[0] * sc;
            if (pos + adv > x)
                lay_add(c, pos);
            pos += adv;
        }
        pos += gap;
    }
}

void lcd_text_scroll(int x, int y, const char *s, int xmax, int off, int gap, int sc,
                     uint16_t fg, uint16_t bg)
{
    scroll_layout(x, s, xmax, off, gap, sc);
    STRIPS(x, xmax, y, FONT_LINE * sc, bg, lay_draw(y, fg, sc, x, xmax));
}

int lcd_glyph(int x, int y, uint32_t cp, uint16_t fg, uint16_t bg)
{
    const uint8_t *g = font_glyph(cp);
    int adv = g[0];
    if (!adv || x + adv > LCD_W)
        return 0;
    STRIPS(x, x + adv, y, FONT_LINE, bg, draw_glyph(x, y, g, fg, 1));
    return adv;
}

/* A 1-bit picture in a glyph record (font.h), in a cell of its advance by FONT_LINE: the icons
 * of the header (uidraw.c); every pixel written once */
int lcd_icon(int x, int y, const uint8_t *rec, uint16_t fg, uint16_t bg)
{
    int adv = rec[0];
    if (!adv || x + adv > LCD_W)
        return 0;
    STRIPS(x, x + adv, y, FONT_LINE, bg, draw_glyph(x, y, rec, fg, 1));
    return adv;
}

uint32_t utf8_next(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !n)
        c = 0xfffd;                             /* a stray continuation byte */
    else if (n)
        c &= 0x3f >> n;
    while (n-- > 0) {
        if ((*s & 0xc0) != 0x80) {
            c = 0xfffd;
            break;
        }
        c = c << 6 | (*s++ & 0x3f);
    }
    *ps = (const char *)s;
    return c;
}

int lcd_text_width_x(const char *s, int sc)
{
    int w = 0;
    while (*s)
        w += glyph_width(font_glyph(utf8_next(&s))) * sc;
    return w;
}

int lcd_text_width(const char *s)
{
    return lcd_text_width_x(s, 1);
}

/* a glyph of advance 'adv' at x: laid out when it fits before the screen's edge (as drawn) */
static int lay_glyph(uint32_t c, int x, int adv)
{
    if (!adv || x + adv > LCD_W)
        return 0;
    lay_add(c, x);
    return adv;
}

/* s from x, cut with an ellipsis before xmax when it does not fit; returns the x reached */
static int clip_layout(int x, const char *s, int xmax, int sc)
{
    int ell = glyph_width(font_glyph(0x2026)) * sc;
    int fits = x + lcd_text_width_x(s, sc) <= xmax;
    lay_n = 0;
    while (*s) {
        const char *t = s;
        uint32_t c = utf8_next(&t);
        int w = glyph_width(font_glyph(c)) * sc;
        if (!fits && x + w + ell > xmax) {
            x += lay_glyph(0x2026, x, ell);
            break;
        }
        x += lay_glyph(c, x, w);
        s = t;
    }
    return x;
}

/* UTF-8 text from x up to xmax (excluded), cut with an ellipsis when it does not fit; the rest
 * of the band up to xmax is filled with bg. Returns the x reached. */
int lcd_text_clip_x(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg, int sc)
{
    int r = clip_layout(x, s, xmax, sc);        /* (an ellipsis may end past xmax) */
    STRIPS(x, r > xmax ? r : xmax, y, FONT_LINE * sc, bg, lay_draw(y, fg, sc, -1, 0));
    return r;
}

int lcd_text_clip(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg)
{
    return lcd_text_clip_x(x, y, s, xmax, fg, bg, 1);
}

int lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int e = x;
    lay_n = 0;
    while (*s) {                                /* (a glyph past the edge left out) */
        uint32_t c = utf8_next(&s);
        e += lay_glyph(c, e, glyph_width(font_glyph(c)));
    }
    STRIPS(x, e, y, FONT_LINE, bg, lay_draw(y, fg, 1, -1, 0));
    return e;
}

/* Short transfers (black, the top of the screen: no byte order to get right) on their own clock:
 * two full lines, then a 22 x 12 square, then the two lines again (0.103d1: the first passed, the
 * second never ended). Each done when the channel stopped and the VOP went idle. */
static int dma_try(int var)
{
    int mode = lcd_dma_mode;
    uint32_t fails = lcd_dma.fails;
    dma_var = var;
    lcd_dma_mode = 1;
    lcd_fill(0, 0, LCD_W, 2, 0);
    lcd_fill(0, 2, 22, 12, 0);
    lcd_fill(0, 0, LCD_W, 2, 0);
    if (lcd_dma_mode)
        lcd_sync();
    int ok = lcd_dma.fails == fails && lcd_dma_mode;
    lcd_dma.fails = fails;
    lcd_lost = 0;
    lcd_dma_mode = ok ? mode : 0;
    return ok;
}

#ifdef ECHO_LCDCAL
static void dma_diag(void);
#endif

static void dma_init(void)
{
    if (!lcd_dma_mode)
        return;
#ifdef ECHO_LCDCAL
    dma_diag();
#endif
    int mode = lcd_dma_mode, v = 0;
    while (v < DMA_VARS && (lcd_dma_mode = mode, !dma_try(v)))
        v++;
    if (v < DMA_VARS)
        log_printf("lcd: DMA to the VOP: channel %u, variant %d (start %u, source master %u, "
                   "destination master %u, bursts of %u), mode %d (VOP_CON %x)", DCH, v,
                   dma_vars[v].start, dma_vars[v].sms + 1u, dma_vars[v].dms + 1u,
                   2u << dma_vars[v].msize, lcd_dma_mode, dma_con[lcd_dma_mode]);
    else
        log_printf("lcd: DMA to the VOP does not run: the CPU writes the screen");
}

#ifdef ECHO_LCDCAL
/* Test build (ETUDE §135): every variant on transfers of several shapes, each one followed: the
 * VOP's state before, when the channel stopped and when the VOP went idle (or not, 20 ms) */
static void dma_diag(void)
{
    static const struct {
        int16_t x, y, w, h;
        uint8_t strip;
    } r[] = {
        { 0, 0, LCD_W, 2, 0 }, { 0, 2, 22, 12, 0 }, { 0, 0, LCD_W, STRIP_H, 1 },
        { 100, 20, 22, 12, 0 }, { 120, 40, 60, STRIP_H, 1 }, { 0, 0, LCD_W, LCD_H, 0 },
    };
    uint16_t *buf = strip_buf[0];
    for (int i = 0; i < LCD_W * STRIP_H; i++)
        buf[i] = 0;
    int first = -1;
    for (int v = 0; v < DMA_VARS; v++) {
        int all = 1;
        for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++) {
            wdt_kick();
            lcd_dma_mode = 1;
            dma_var = v;
            fill_word = 0;
            uint32_t s0 = REG(VOP_STATUS), i0 = REG(VOP_INT_STATUS), t0 = ticks();
            dma_send(r[k].x, r[k].y, r[k].w, r[k].h, r[k].strip ? (const void *)buf : &fill_word,
                     !r[k].strip);
            while ((REG(D_CH_EN) & 1u << DCH) && ticks() - t0 < 50000)
                ;
            uint32_t td = ticks() - t0, s1 = REG(VOP_STATUS), i1 = REG(VOP_INT_STATUS);
            int dma_ok = !(REG(D_CH_EN) & 1u << DCH);
            uint32_t t1 = ticks();
            while ((REG(VOP_STATUS) & 1) && ticks() - t1 < 20000)
                ;
            uint32_t ti = ticks() - t1, s2 = REG(VOP_STATUS), i2 = REG(VOP_INT_STATUS);
            int ok = dma_ok && !(s2 & 1);
            log_printf("lcddiag: variant %d, %dx%d %s: before st %x int %x; DMA %s in %u us, st %x "
                       "int %x; VOP %s in %u us, st %x int %x", v, r[k].w, r[k].h,
                       r[k].strip ? "strip" : "fill", s0, i0, dma_ok ? "done" : "NOT DONE", td,
                       s1, i1, s2 & 1 ? "STILL WORKING" : "idle", ti, s2, i2);
            if (!ok) {                          /* (as dma_fail, without leaving the DMA) */
                REG(D_CH_EN) = 1u << (8 + DCH);
                REG(VOP_SRT) = 1;
                lcd_set_timing(cpu_hz);
            }
            REG(VOP_CON) = VOP_CON_BYPASS;
            dma_busy = 0;
            all &= ok;
        }
        if (all && first < 0)
            first = v;
    }
    log_printf("lcddiag: the first variant with every transfer done: %d (-1: none)", first);
    lcd_dma_mode = 1;
}

/* Test build (EXTRA=-DECHO_LCDCAL, ETUDE §134): the user picks the VOP mode whose bars and text
 * look like the reference drawn by the CPU (VOL+ / VOL- and PLAY; 30 s without a key: mode 1) */
static void cal_row(int y, int mode, const char *label)
{
    static const uint16_t col[7] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0xffe0, 0xf81f, 0x07ff };
    lcd_dma_mode = 0;
    lcd_text_clip(0, y, label, 24, 0xffff, 0x0000);
    lcd_dma_mode = mode;
    for (int i = 0; i < 7; i++)
        lcd_fill(24 + 22 * i, y + 2, 22, 12, col[i]);
    /* (139 pixels wide: an odd width, 0.103d3c) */
    lcd_text_clip(180, y, EN("Abc 123 \xe6\x9d\xb1\xe4\xba\xac"), LCD_W - 1, 0xffe0, 0x001f);
    if (lcd_dma_mode)
        lcd_sync();
}

void lcd_calibrate(void)
{
    static const char *const label[6] = { "ref", "1", "2", "3", "4", "5" };
    int ok[5] = { 1, 0, 0, 0, 0 }, sel = 1, k = 0;
    if (!lcd_dma_mode)
        return;
    lcd_dma_mode = 0;
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_text_clip(0, 0, EN("Screen test: which line looks like ref?"), LCD_W, 0xffff, 0);
    cal_row(18, 0, label[0]);
    for (int m = 1; m <= 4; m++) {
        uint32_t f = lcd_dma.fails;
        cal_row(18 + 22 * m, m, label[m]);
        ok[m] = lcd_dma.fails == f;
        lcd_dma.fails = f;
        lcd_lost = 0;
    }
    lcd_dma_mode = 0;
    lcd_text_clip(0, 128, EN("5  none of them (the CPU draws)"), LCD_W, 0xffff, 0);
    lcd_text_clip(0, 152, EN("VOL+/VOL-: choose   PLAY: confirm"), LCD_W, 0x8410, 0);
    log_printf("lcd: screen test: DMA ran in modes 1 %d, 2 %d, 3 %d, 4 %d", ok[1], ok[2], ok[3],
               ok[4]);
    uint32_t t0 = millis();
    for (;;) {
        for (int m = 1; m <= 5; m++)            /* the mark */
            lcd_text_clip(310, m < 5 ? 18 + 22 * m : 128, m == sel ? "<" : " ", LCD_W, 0xf800, 0);
        do {
            wdt_kick();
            k = key_edge();
        } while (!k && millis() - t0 < 30000);
        if (!k || k == KEY_PLAY)
            break;
        t0 = millis();
        if (k == KEY_VOLDOWN)
            sel = sel % 5 + 1;
        if (k == KEY_VOLUP)
            sel = (sel + 3) % 5 + 1;
    }
    lcd_dma_mode = sel < 5 && ok[sel] ? sel : 0;
    log_printf("lcd: screen test: line %d chosen%s, mode %d", sel, k ? "" : " (no key: the first)",
               lcd_dma_mode);
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    struct key_ev e;                            /* the PLAY that confirmed: not for the UI */
    while (key_poll(&e) != EV_NONE || key_held())
        wdt_kick();
}
#endif

#ifdef ECHO_LCDPERF
/* Test build (make EXTRA=-DECHO_LCDPERF, ETUDE §132, §134): what a frame costs core A, with
 * the CPU writing each byte and with DMA, measured 3 times each */
static void perf_line(const char *what, uint32_t t[3])
{
    log_printf("lcdperf: %s: %u, %u, %u us", what, t[0], t[1], t[2]);
}

void lcd_perf_probe(void)
{
    static const char *const text[2] = {
        "The quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJ",
        "\xe6\x9d\xb1\xe4\xba\xac\xe3\x81\xae\xe5\xa4\x9c \xe9\xbe\x8d\xe8\x88\x87\xe9\xb3\xb3"
        "\xe5\x87\xb0 \xec\x84\x9c\xec\x9a\xb8\xec\x9d\x98 \xeb\xb0\xa4 \xe3\x81\xb2\xe3\x82\x89"
        "\xe3\x81\x8c\xe3\x81\xaa\xe3\x83\xbb\xe3\x82\xab\xe3\x82\xbf\xe3\x82\xab\xe3\x83\x8a",
    };
    uint32_t t[3], t0, mode = (uint32_t)lcd_dma_mode;
    log_printf("lcdperf: VOP timing %x, a frame is %u bytes, DMA mode %u", REG(VOP_TIMING),
               LCD_W * LCD_H * 2, mode);
    for (uint32_t d = 0; d < 2; d++) {          /* the CPU, then the DMA (when it runs) */
        if (d && !mode)
            break;
        lcd_dma_mode = (int)(d ? mode : 0);
        const char *how = d ? "DMA" : "CPU";
        for (int i = 0; i < 3; i++) {           /* a fill: the core's time, then the end */
            t0 = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, i == 1 ? 0xf800 : 0x001f);
            t[i] = ticks() - t0;
        }
        perf_line(d ? "fill 320x170, DMA, core A" : "fill 320x170, CPU", t);
        if (d) {
            for (int i = 0; i < 3; i++) {
                t0 = ticks();
                lcd_fill(0, 0, LCD_W, LCD_H, 0x07e0);
                lcd_sync();
                t[i] = ticks() - t0;
            }
            perf_line("fill 320x170, DMA, until sent", t);
        }
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 3; i++) {       /* 10 lines of text, as the screens draw them */
                t0 = ticks();
                for (int l = 0; l < 10; l++)
                    lcd_text_clip(0, l * FONT_LINE, text[k], LCD_W, 0xffff, 0x0000);
                t[i] = ticks() - t0;
            }
            log_printf("lcdperf: text 10 lines %s, %s: %u, %u, %u us", k ? "CJK" : "Latin", how,
                       t[0], t[1], t[2]);
        }
        lcd_sync();
    }
    for (int i = 0; i < 3; i++) {               /* the strips drawn only (sent nowhere) */
        t0 = ticks();
        for (int l = 0; l < 10; l++)
            for (int sy = 0; sy < FONT_LINE; sy += STRIP_H) {
                strip_begin(0, LCD_W, sy, sy + STRIP_H, 0);
                clip_layout(0, text[0], LCD_W, 1);
                lay_draw(0, 0xffff, 1, -1, 0);
            }
        t[i] = ticks() - t0;
    }
    perf_line("text 10 lines Latin, strips drawn only", t);
    lcd_dma_mode = (int)mode;
    lcd_fill(0, 0, LCD_W, LCD_H, 0x0000);
    lcd_sync();
}
#endif

/* The backlight: at 100 % (and off) a GPIO, below on PWM0 channel 0 at 10 kHz (pin function 1),
 * as the stock firmware does (ETUDE §3.3). PCLK is the core clock / 4 (board.c); the screen is
 * never on while the clocks are on the crystal (sys_screen_set). The steps in the stock
 * firmware's order (0.96 set the period with the channel stopped: no effect on the device):
 * clock, reset, channel on, pin; then duty, counter, period, control, the channel running.
 * DUTY is the time the backlight is off: 0.96b wrote the time on, and 90 % was the dimmest. */
static int bl_pct = 100, bl_on, pwm_on;

void lcd_backlight(int on)
{
    bl_on = on;
    if (!on || bl_pct >= 100) {
        iomux(PIN_BACKLIGHT, 0);
        gpio_dir(PIN_BACKLIGHT, 1);
        gpio_set(PIN_BACKLIGHT, on);
        if (pwm_on) {
            REG(PWM_CTRL(0)) &= ~1u;
            clk_gate(9 * 16 + 0, 0);
            pwm_on = 0;
        }
        return;
    }
    if (!pwm_on) {
        clk_gate(9 * 16 + 0, 1);                /* pclk_pwm0 */
        soft_reset(1 * 16 + 7, 1);
        udelay(1000);
        soft_reset(1 * 16 + 7, 0);
        REG(PWM_CTRL(0)) |= 1;
        iomux(PIN_BACKLIGHT, 1);
        gpio_dir(PIN_BACKLIGHT, 1);
        pwm_on = 1;
    }
    uint32_t period = cpu_hz / 4 / 10000;
    REG(PWM_DUTY(0)) = period * (100u - (uint32_t)bl_pct) / 100;
    REG(PWM_CNT(0)) = 0;
    REG(PWM_PERIOD(0)) = period;
    REG(PWM_CTRL(0)) = PWM_CTRL_ON;
    log_printf("lcd: backlight %d %%: period %u, duty %u, ctrl %x", bl_pct, REG(PWM_PERIOD(0)),
               REG(PWM_DUTY(0)), REG(PWM_CTRL(0)));
}

void lcd_brightness(int pct)
{
    bl_pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
    if (bl_on)
        lcd_backlight(1);
}

/* Panel sleep (ST7789 DISPOFF + SLPIN, then SLPOUT + 120 ms + DISPON): the controller keeps its
 * memory, and the interface still writes it, so drawing may go on while it sleeps. Since 0.99r5
 * (ETUDE §99) nothing waits here: lcd_service(), called by the loops, sends the next command once
 * the controller's delay has passed (120 ms after SLPOUT, 5 ms after SLPIN), then DISPON and the
 * backlight when asked: the first frame is drawn meanwhile, and the audio is served. */
static int lcd_asleep;                          /* the last command sent: SLPIN 1, SLPOUT 0 */
static int lcd_want, lcd_busy, lcd_light;       /* asked; waiting its delay; backlight on wake */
static uint32_t lcd_t;                          /* ticks() of the last SLPIN or SLPOUT */

void lcd_service(void)
{
    if (lcd_busy) {
        if (ticks() - lcd_t < (lcd_asleep ? 5000u : 120000u))
            return;
        lcd_busy = 0;
        if (!lcd_asleep) {
            cmd(0x29);                          /* DISPON: the frame drawn meanwhile shows */
            if (lcd_light && !lcd_want)
                lcd_backlight(1);
            lcd_light = 0;
        }
    }
    if (lcd_want == lcd_asleep)
        return;
    if (lcd_want) {
        cmd(0x28);
        cmd(0x10);
    } else
        cmd(0x11);
    lcd_asleep = lcd_want;
    lcd_t = ticks();
    lcd_busy = 1;
}

void lcd_sleep(int on)
{
    lcd_want = on;
    if (on)
        lcd_light = 0;
    lcd_service();
}

void lcd_wake_lit(void)
{
    lcd_light = 1;
    lcd_sleep(0);
    if (!lcd_busy && !lcd_asleep) {             /* awake already: the backlight at once */
        lcd_backlight(1);
        lcd_light = 0;
    }
}

static int lcd_waking(void)       /* a sleep or wake command still waits its delay */
{
    return lcd_busy || lcd_want != lcd_asleep;
}

/* The fault screen: no SysTick there, a counted wait instead of mdelay() */
void lcd_wake_fault(void)
{
    if (dma_busy) {                             /* a transfer cut: the VOP reset, the CPU's */
        REG(D_CH_EN) = 1u << (8 + DCH);
        REG(VOP_SRT) = 1;
        lcd_set_timing(cpu_hz);
        REG(VOP_CON) = VOP_CON_BYPASS;
        dma_busy = 0;
    }
    lcd_dma_mode = 0;                           /* the fault screen: the CPU only */
    if (!lcd_asleep && !lcd_busy)
        return;
    cmd(0x11);
    for (volatile uint32_t i = 0; i < 6000000; i++)
        ;
    cmd(0x29);
    lcd_asleep = lcd_want = lcd_busy = 0;
}

void lcd_settle(void)
{
    while (lcd_waking())
        lcd_service();
}

void lcd_init(void)
{
    clk_gate(GATE_HCLK_VOP, 1);
    soft_reset(SRST_VOP, 0);
    lcd_set_timing(cpu_hz);

    panel_reset(30);
    iomux(PIN_LCD_SEL, 0);                      /* variant pin, read before power-up */
    gpio_dir(PIN_LCD_SEL, 0);
    udelay(10);
    int sel = gpio_get(PIN_LCD_SEL);
    gpio_dir(PIN_LCD_SEL, 1);
    gpio_set(PIN_LCD_SEL, 0);
    lcd_id = panel_read_id();
    if (!sel)
        lcd_id = 0x66666666;
    bus_to_vop();

    panel_reset(120);
    REG(VOP_CON) = VOP_CON_BYPASS;
    lcd_variant = (lcd_id & 0xff0000) == 0xe80000;
    if (lcd_variant)
        send_table(init_b, sizeof init_b);
    else
        send_table(init_a, sizeof init_a);
    lcd_dma_mode = 0;
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
#ifndef LCD_NO_DMA                              /* (test builds: the CPU only) */
    lcd_dma_mode = 1;
    dma_init();
#endif
    lcd_ready = 1;
}
