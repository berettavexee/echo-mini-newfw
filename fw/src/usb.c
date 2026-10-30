/* USB device on the Synopsys DWC2 OTG core (TRM chapter 14 and the public DWC2 programming
 * model), polled: the core flags its events in GINTSTS, DAINT and DIEPINTn / DOEPINTn whatever
 * the interrupt masks, and the RX FIFO is popped through GRXSTSP.
 *
 * Core seen on the device (RECHO's log): hwcfg2 0820cc42 (OTG, slave mode, UTMI+ PHY),
 * hwcfg3 01340480 (FIFO of 308 words, 11-bit transfer size: at most 2047 bytes per transfer),
 * hwcfg4 09e04061 (one shared TX FIFO for all IN endpoints), RX FIFO 147 words, non-periodic
 * TX FIFO 128 words (one 512-byte packet). Enumerates at high speed. */
#include "usb.h"
#include "board.h"
#include "regs.h"
#include "version.h"

#define OTG        0x60080000u
#define GAHBCFG    (OTG + 0x008)
#define GRSTCTL    (OTG + 0x010)
#define GINTSTS    (OTG + 0x014)
#define GINTMSK    (OTG + 0x018)
#define GRXSTSP    (OTG + 0x020)
#define GRXFSIZ    (OTG + 0x024)
#define GNPTXFSIZ  (OTG + 0x028)
#define DPTXFSIZ1  (OTG + 0x104)
#define GNPTXSTS   (OTG + 0x02c)
#define DCFG       (OTG + 0x800)
#define DCTL       (OTG + 0x804)
#define DSTS       (OTG + 0x808)
#define DIEPMSK    (OTG + 0x810)
#define DOEPMSK    (OTG + 0x814)
#define DAINT      (OTG + 0x818)
#define DAINTMSK   (OTG + 0x81c)
#define DIEPCTL(n) (OTG + 0x900 + 0x20 * (n))
#define DIEPINT(n) (OTG + 0x908 + 0x20 * (n))
#define DIEPTSIZ(n) (OTG + 0x910 + 0x20 * (n))
#define DOEPCTL(n) (OTG + 0xb00 + 0x20 * (n))
#define DOEPINT(n) (OTG + 0xb08 + 0x20 * (n))
#define DOEPTSIZ(n) (OTG + 0xb10 + 0x20 * (n))
#define PCGCCTL    (OTG + 0xe00)
#define FIFO(n)    (OTG + 0x1000 + 0x1000 * (n))

#define GINT_SOF      (1u << 3)
#define GINT_RXFLVL   (1u << 4)
#define GINT_USBSUSP  (1u << 11)
#define GINT_USBRST   (1u << 12)
#define GINT_ENUMDONE (1u << 13)
#define GINT_IEPINT   (1u << 18)
#define GINT_OEPINT   (1u << 19)
#define GINT_INCOMPLP (1u << 21)                /* device mode: incomplete isochronous OUT */
#define GINT_IISOIXFR (1u << 20)                /* incomplete isochronous IN */
#define GINT_WKUP     (1u << 31)

#define EP_ENA     (1u << 31)
#define EP_DIS     (1u << 30)
#define EP_SD0PID  (1u << 28)                   /* isochronous: SetEvenFr */
#define EP_SODDFR  (1u << 29)
#define EP_SNAK    (1u << 27)
#define EP_CNAK    (1u << 26)
#define EP_STALL   (1u << 21)
#define EP_BULK    (2u << 18)
#define EP_ISO     (1u << 18)
#define EP_TXFNUM1 (1u << 22)                   /* IN: periodic TX FIFO 1 */
#define EP_ACTIVE  (1u << 15)

#define INT_XFERCOMPL (1u << 0)
#define INT_SETUP     (1u << 3)

#define DCTL_SFTDISCON (1u << 1)
#define DCTL_CGINAK    (1u << 8)

#define IO_TIMEOUT_US 3000000
#define FIFO_WORDS    308                       /* hwcfg3: the core's FIFO RAM */

struct usb_state usb;
static const struct usb_class *cls;             /* 0: mass storage */

/* ---- descriptors (test IDs of pid.codes: private use only) ---- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x10, 0x00, 1, 2, 3, 1,
};
static const uint8_t qual_desc[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0 };
static uint8_t cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 250,                    /* bus powered, 500 mA */
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,                 /* mass storage, SCSI, bulk only */
    7, 5, 0x81, 2, 0x00, 0x02, 0,                       /* bulk IN 1 */
    7, 5, 0x01, 2, 0x00, 0x02, 0,                       /* bulk OUT 1 */
};
static const char *const strings[] = { 0, "newfw", "ECHO MINI", "000000000001" };

/* ---- endpoint 0 state ---- */
static uint32_t setup[2];
static uint32_t ep0_buf[64];            /* descriptor being sent */
static const uint8_t *ep0_ptr;
static uint32_t ep0_left;
static int ep0_zlp;
static uint8_t ep0_rx[64];              /* the data stage of a request from the host */
static uint32_t ep0_rx_want, ep0_rx_got;
static uint8_t ep0_rx_setup[8];

/* ---- endpoint 1 isochronous OUT and IN (a class): their packet sizes, 0 when closed ---- */
static uint32_t iso_mps, iso_in_mps;

/* ---- endpoint 1 state ---- */
static uint8_t *out_buf;
static uint32_t out_len, out_got;
static volatile int out_done, in_done;
static int aborted;                     /* bus reset or deconfiguration since the transfer began */

static int wait_reg_clear(uint32_t reg, uint32_t mask, uint32_t us)
{
    uint32_t t0 = ticks();
    while (REG(reg) & mask)
        if (ticks() - t0 > us)
            return -1;
    return 0;
}

static void spin_us(uint32_t us)
{
    uint32_t t0 = ticks();
    while (ticks() - t0 < us)
        ;
}

static void flush_fifos(void)
{
    REG(GRSTCTL) = 0x10u << 6 | 1u << 5;        /* all TX FIFOs */
    wait_reg_clear(GRSTCTL, 1u << 5, 10000);
    REG(GRSTCTL) = 1u << 4;                     /* RX FIFO */
    wait_reg_clear(GRSTCTL, 1u << 4, 10000);
}

/* Writes one packet to the shared TX FIFO once it has room for it */
static int fifo_write(int ep, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t words = (len + 3) / 4, t0 = ticks();

    while ((REG(GNPTXSTS) & 0xffff) < words || !((REG(GNPTXSTS) >> 16) & 0xff)) {
        if (ticks() - t0 > IO_TIMEOUT_US)
            return USB_ETIMEOUT;
        wdt_kick();
    }
    for (uint32_t i = 0; i < words; i++, p += 4)
        REG(FIFO(ep)) = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    return 0;
}

static void ep0_out_arm(void)
{
    REG(DOEPTSIZ(0)) = 3u << 29 | 1u << 19 | 24;    /* 3 back-to-back SETUPs, 1 packet */
    REG(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
}

static void ep0_stall(void)
{
    REG(DIEPCTL(0)) |= EP_STALL;
    REG(DOEPCTL(0)) |= EP_STALL;
}

static void ep0_packet(void)
{
    uint32_t n = ep0_left > 64 ? 64 : ep0_left;

    if (!n)
        ep0_zlp = 0;
    REG(DIEPTSIZ(0)) = 1u << 19 | n;
    REG(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
    if (n)
        fifo_write(0, ep0_ptr, n);
    ep0_ptr += n;
    ep0_left -= n;
}

/* Sends at most 'max' bytes of data on endpoint 0 (the host's wLength) */
static void ep0_send(const void *data, uint32_t len, uint32_t max)
{
    const uint8_t *s = data;
    uint8_t *d = (uint8_t *)ep0_buf;

    if (len > max)
        len = max;
    if (len > sizeof ep0_buf)
        len = sizeof ep0_buf;
    for (uint32_t i = 0; i < len; i++)
        d[i] = s[i];
    ep0_ptr = d;
    ep0_left = len;
    ep0_zlp = len < max && len % 64 == 0 && len;    /* a short transfer ending on a full packet */
    ep0_packet();
}

static void ep0_status_in(void)
{
    ep0_left = 0;
    ep0_zlp = 0;
    REG(DIEPTSIZ(0)) = 1u << 19;
    REG(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
}

static void ep1_configure(void)
{
    uint32_t mps = usb.mps;
    REG(DIEPCTL(1)) = EP_SD0PID | EP_BULK | EP_ACTIVE | EP_SNAK | mps;
    REG(DOEPCTL(1)) = EP_SD0PID | EP_BULK | EP_ACTIVE | EP_SNAK | mps;
    REG(DIEPINT(1)) = 0xff;
    REG(DOEPINT(1)) = 0xff;
}

static void ep1_deconfigure(void)
{
    if (REG(DIEPCTL(1)) & EP_ENA)
        REG(DIEPCTL(1)) |= EP_SNAK | EP_DIS;
    if (REG(DOEPCTL(1)) & EP_ENA)
        REG(DOEPCTL(1)) |= EP_SNAK | EP_DIS;
    REG(DIEPCTL(1)) &= ~EP_ACTIVE;
    REG(DOEPCTL(1)) &= ~EP_ACTIVE;
    usb.configured = 0;
    aborted = 1;
}

static uint32_t string_desc(int i, uint8_t *d)
{
    if (i == 0) {
        d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04;
        return 4;
    }
    const char *s = strings[i];
    uint32_t n = 2;
    while (*s && n < 64) {
        d[n++] = (uint8_t)*s++;
        d[n++] = 0;
    }
    d[0] = (uint8_t)n;
    d[1] = 3;
    return n;
}

/* Endpoint 1 OUT armed for one isochronous packet in the next frame (its parity) */
static void iso_arm(void)
{
    uint32_t fn = (REG(DSTS) >> 8) & 0x3fff;
    REG(DOEPTSIZ(1)) = 1u << 19 | iso_mps;
    REG(DOEPCTL(1)) |= EP_ENA | EP_CNAK | ((fn & 1) ? EP_SD0PID : EP_SODDFR);
}

void usb_iso_out_open(uint32_t mps)
{
    iso_mps = mps;
    REG(DOEPCTL(1)) = EP_ISO | EP_ACTIVE | EP_SNAK | mps;
    REG(DOEPINT(1)) = 0xff;
    iso_arm();
}

void usb_iso_out_close(void)
{
    if (REG(DOEPCTL(1)) & EP_ENA)
        REG(DOEPCTL(1)) |= EP_SNAK | EP_DIS;
    REG(DOEPCTL(1)) &= ~EP_ACTIVE;
    iso_mps = 0;
}

/* The parity bit of the frame after the current one */
static uint32_t next_frame_parity(void)
{
    return ((REG(DSTS) >> 8) & 1) ? EP_SD0PID : EP_SODDFR;
}

static void ptx_flush(void)
{
    REG(GRSTCTL) = 1u << 6 | 1u << 5;           /* periodic TX FIFO 1 */
    wait_reg_clear(GRSTCTL, 1u << 5, 10000);
}

void usb_iso_in_open(uint32_t mps)
{
    iso_in_mps = mps;
    REG(DIEPCTL(1)) = EP_ISO | EP_ACTIVE | EP_TXFNUM1 | EP_SNAK | mps;
    REG(DIEPINT(1)) = 0xff;
    ptx_flush();
}

void usb_iso_in_close(void)
{
    if (REG(DIEPCTL(1)) & EP_ENA)
        REG(DIEPCTL(1)) |= EP_SNAK | EP_DIS;
    REG(DIEPCTL(1)) &= ~EP_ACTIVE;
    iso_in_mps = 0;
    ptx_flush();
}

/* One packet for the next frame: the host polls every few frames (bRefresh), so a packet not
 * taken is aimed again at the next frame (GINT_IISOIXFR), as it stays in the FIFO */
void usb_iso_in_send(const void *data, uint32_t n)
{
    const uint8_t *p = data;
    if (!iso_in_mps || n > iso_in_mps)
        return;
    REG(DIEPTSIZ(1)) = 1u << 29 | 1u << 19 | n;        /* one packet per frame */
    REG(DIEPCTL(1)) |= EP_ENA | EP_CNAK | next_frame_parity();
    for (uint32_t i = 0; i < n; i += 4)
        REG(FIFO(1)) = p[i] | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] << 16 : 0) |
                       (uint32_t)(i + 3 < n ? p[i + 3] : 0) << 24;
}

/* A request for the class (or SET_INTERFACE / GET_INTERFACE): its answer, or its data stage */
static void class_setup(const uint8_t *s, uint8_t *tmp)
{
    uint16_t length = s[6] | s[7] << 8;
    if (!(s[0] & 0x80) && length) {                     /* data from the host: receive it first */
        for (int i = 0; i < 8; i++)
            ep0_rx_setup[i] = s[i];
        ep0_rx_want = length > sizeof ep0_rx ? sizeof ep0_rx : length;
        ep0_rx_got = 0;
        REG(DOEPTSIZ(0)) = 1u << 29 | 1u << 19 | ep0_rx_want;
        REG(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
        return;
    }
    int r = cls->setup(s, tmp);
    if (r < 0)
        ep0_stall();
    else if (r == 0)
        ep0_status_in();
    else
        ep0_send(tmp, (uint32_t)r, length);
    ep0_out_arm();
}

static void handle_setup(void)
{
    const uint8_t *s = (const uint8_t *)setup;
    uint8_t type = s[0], req = s[1];
    uint16_t value = s[2] | s[3] << 8, index = s[4] | s[5] << 8, length = s[6] | s[7] << 8;
    uint8_t tmp[64];

    usb.setups++;
    if (cls && ((type & 0x60) == 0x20 || ((type & 0x60) == 0 && (req == 10 || req == 11) &&
                                          (type & 0x1f) == 1))) {
        class_setup(s, tmp);                            /* class, SET/GET_INTERFACE */
        return;
    }
    if ((type & 0x60) == 0x00) {                        /* standard */
        switch (req) {
        case 6:                                         /* GET_DESCRIPTOR */
            switch (value >> 8) {
            case 1:
                ep0_send(cls ? cls->dev : dev_desc, sizeof dev_desc, length);
                break;
            case 2:
                if (cls) {
                    ep0_send(cls->cfg, cls->cfg_len, length);
                    break;
                }
                cfg_desc[22] = cfg_desc[29] = (uint8_t)usb.mps;
                cfg_desc[23] = cfg_desc[30] = (uint8_t)(usb.mps >> 8);
                ep0_send(cfg_desc, sizeof cfg_desc, length);
                break;
            case 3:
                if ((value & 0xff) < sizeof strings / sizeof *strings)
                    ep0_send(tmp, string_desc(value & 0xff, tmp), length);
                else
                    ep0_stall();
                break;
            case 6:                                     /* a full-speed-only device has none */
                if (cls && cls->full_speed)
                    ep0_stall();
                else
                    ep0_send(qual_desc, sizeof qual_desc, length);
                break;
            default:
                ep0_stall();
            }
            break;
        case 5:                                         /* SET_ADDRESS */
            REG(DCFG) = (REG(DCFG) & ~0x7f0u) | (value & 0x7f) << 4;
            ep0_status_in();
            break;
        case 9:                                         /* SET_CONFIGURATION */
            if (cls) {                                  /* the class opens its endpoints */
                if (!value)
                    usb_iso_out_close();
                usb.configured = value == 1;
            } else if (value == 1) {
                ep1_configure();
                usb.configured = 1;
            } else
                ep1_deconfigure();
            ep0_status_in();
            break;
        case 8:                                         /* GET_CONFIGURATION */
            tmp[0] = (uint8_t)usb.configured;
            ep0_send(tmp, 1, length);
            break;
        case 0:                                         /* GET_STATUS */
            tmp[0] = tmp[1] = 0;
            if ((type & 0x1f) == 2 && (index & 0x7f) == 1)
                tmp[0] = (REG((index & 0x80) ? DIEPCTL(1) : DOEPCTL(1)) & EP_STALL) != 0;
            ep0_send(tmp, 2, length);
            break;
        case 1:                                         /* CLEAR_FEATURE (endpoint halt) */
            if ((type & 0x1f) == 2 && (index & 0x7f) == 1) {
                uint32_t r = (index & 0x80) ? DIEPCTL(1) : DOEPCTL(1);
                REG(r) = (REG(r) & ~EP_STALL) | EP_SD0PID;
            }
            ep0_status_in();
            break;
        case 11:                                        /* SET_INTERFACE */
            ep0_status_in();
            break;
        case 10:                                        /* GET_INTERFACE */
            tmp[0] = 0;
            ep0_send(tmp, 1, length);
            break;
        default:
            ep0_stall();
        }
    } else if ((type & 0x60) == 0x20 && req == 0xfe) {  /* GET_MAX_LUN */
        tmp[0] = 0;
        ep0_send(tmp, 1, length);
    } else if ((type & 0x60) == 0x20 && req == 0xff) {  /* bulk-only mass storage reset */
        usb.bot_reset = 1;
        aborted = 1;
        ep0_status_in();
    } else
        ep0_stall();
    ep0_out_arm();
}

static void bus_reset(void)
{
    usb.resets++;
    usb.configured = 0;
    aborted = 1;
    REG(DCFG) &= ~0x7f0u;
    for (int n = 0; n < 2; n++) {
        REG(DIEPINT(n)) = 0xff;
        REG(DOEPINT(n)) = 0xff;
    }
    REG(DIEPCTL(1)) = EP_SNAK;
    REG(DOEPCTL(1)) = EP_SNAK;
    iso_mps = iso_in_mps = 0;
    flush_fifos();
    ep0_left = 0;
    ep0_zlp = 0;
    ep0_rx_want = 0;
    ep0_out_arm();
    if (cls && cls->reset)
        cls->reset();
}

static void rx_pop(void)
{
    uint32_t st = REG(GRXSTSP), ep = st & 0xf, n = (st >> 4) & 0x7ff, kind = (st >> 17) & 0xf;
    uint32_t words = (n + 3) / 4;

    if (kind == 6) {                                    /* SETUP data */
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = REG(FIFO(0));
            if (i < 2)
                setup[i] = w;
        }
    } else if (kind == 2 && ep == 1 && iso_mps && cls) {   /* isochronous OUT: to the class */
        cls->iso_out(n);
    } else if (kind == 2 && ep == 0 && ep0_rx_want) {   /* data stage of a class request */
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = REG(FIFO(0));
            for (int k = 0; k < 4 && 4 * i + k < n; k++)
                if (ep0_rx_got < sizeof ep0_rx)
                    ep0_rx[ep0_rx_got++] = (uint8_t)(w >> (8 * k));
        }
    } else if (kind == 2 && ep == 1 && out_buf && !(((uint32_t)out_buf + out_got) & 3) &&
               out_got + 4 * words <= out_len) {       /* OUT data, aligned: word copy */
        uint32_t *d = (uint32_t *)(out_buf + out_got);
        for (uint32_t i = 0; i < words; i++)
            d[i] = REG(FIFO(0));
        out_got += n;
    } else if (kind == 2) {                             /* OUT data */
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = REG(FIFO(0));
            if (ep == 1 && out_buf)
                for (int k = 0; k < 4 && 4 * i + k < n; k++)
                    if (out_got + 4 * i + k < out_len)
                        out_buf[out_got + 4 * i + k] = (uint8_t)(w >> (8 * k));
        }
        if (ep == 1)
            out_got += n;
    }
}

void usb_service(void)
{
    uint32_t g = REG(GINTSTS);

    if (g & GINT_USBRST) {
        REG(GINTSTS) = GINT_USBRST;
        bus_reset();
    }
    if (g & GINT_ENUMDONE) {
        REG(GINTSTS) = GINT_ENUMDONE;
        usb.high_speed = ((REG(DSTS) >> 1) & 3) == 0;
        usb.mps = usb.high_speed ? 512 : 64;
        REG(DIEPCTL(0)) &= ~3u;                         /* 64-byte control packets */
        REG(DCTL) |= DCTL_CGINAK;
    }
    if (g & GINT_USBSUSP) {
        REG(GINTSTS) = GINT_USBSUSP;
        if (!usb.suspended)
            usb.suspend_since = ticks();
        usb.suspended = 1;
    }
    if (g & (GINT_WKUP | GINT_RXFLVL | GINT_USBRST | GINT_ENUMDONE | GINT_SOF)) {
        REG(GINTSTS) = g & (GINT_WKUP | GINT_SOF);
        usb.suspended = 0;
    }
    for (int n = 0; n < 32 && (REG(GINTSTS) & GINT_RXFLVL); n++)
        rx_pop();
    if (g & GINT_OEPINT) {
        uint32_t d = REG(DAINT);
        if (d & 1u << 16) {
            uint32_t i = REG(DOEPINT(0));
            REG(DOEPINT(0)) = i;
            if (i & INT_SETUP)
                handle_setup();
            else if ((i & INT_XFERCOMPL) && ep0_rx_want) {  /* the data of a class request */
                ep0_rx_want = 0;
                cls->setup_out(ep0_rx_setup, ep0_rx, ep0_rx_got);
                ep0_status_in();
                ep0_out_arm();
            } else if (i & INT_XFERCOMPL)
                ep0_out_arm();                          /* after a status stage */
        }
        if (d & 1u << 17) {
            uint32_t i = REG(DOEPINT(1));
            REG(DOEPINT(1)) = i;
            if ((i & INT_XFERCOMPL) && iso_mps)
                iso_arm();                              /* the next frame's packet */
            else if (i & INT_XFERCOMPL)
                out_done = 1;
        }
    }
    if (g & GINT_IISOIXFR) {                            /* the host did not poll that frame */
        REG(GINTSTS) = GINT_IISOIXFR;
        if (iso_in_mps && (REG(DIEPCTL(1)) & EP_ENA)) {
            usb.iso_in_late++;
            REG(DIEPCTL(1)) |= next_frame_parity();
        }
    }
    if (g & GINT_INCOMPLP) {                            /* a frame went by without our packet */
        REG(GINTSTS) = GINT_INCOMPLP;
        if (iso_mps) {
            usb.iso_incomplete++;
            if (REG(DOEPCTL(1)) & EP_ENA) {             /* armed for a frame gone: the next one */
                uint32_t fn = (REG(DSTS) >> 8) & 0x3fff;
                REG(DOEPCTL(1)) |= (fn & 1) ? EP_SD0PID : EP_SODDFR;
            } else
                iso_arm();
        }
    }
    if (g & GINT_IEPINT) {
        uint32_t d = REG(DAINT);
        if (d & 1u) {
            uint32_t i = REG(DIEPINT(0));
            REG(DIEPINT(0)) = i;
            if ((i & INT_XFERCOMPL) && (ep0_left || ep0_zlp))
                ep0_packet();
        }
        if (d & 2u) {
            uint32_t i = REG(DIEPINT(1));
            REG(DIEPINT(1)) = i;
            if ((i & INT_XFERCOMPL) && iso_in_mps) {
                usb.iso_in_sent++;
                cls->iso_in_done();
            } else if (i & INT_XFERCOMPL)
                in_done = 1;
        }
    }
}

/* At most 3 packets per transfer, as for IN (11-bit transfer size) */
void usb_out_arm(void *buf, uint32_t len)
{
    if (len > 3 * usb.mps)
        len = 3 * usb.mps;
    uint32_t pk = (len + usb.mps - 1) / usb.mps;

    if (!pk)
        pk = 1;
    out_buf = buf;
    out_len = len;
    out_got = 0;
    out_done = 0;
    aborted = 0;
    REG(DOEPTSIZ(1)) = pk << 19 | pk * usb.mps;
    REG(DOEPCTL(1)) |= EP_ENA | EP_CNAK;
}

int usb_out_done(uint32_t *got)
{
    usb_service();
    if (!out_done)
        return 0;
    *got = out_got < out_len ? out_got : out_len;
    return 1;
}

int usb_out(void *buf, uint32_t len, uint32_t *got)
{
    uint32_t t0 = ticks();

    usb_out_arm(buf, len);
    while (!usb_out_done(got)) {
        if (aborted)
            return USB_EABORT;
        if (ticks() - t0 > IO_TIMEOUT_US)
            return USB_ETIMEOUT;
        wdt_kick();
    }
    return 0;
}

/* Transfers of at most 3 packets: the transfer size field is 11 bits wide */
int usb_in(const void *buf, uint32_t len)
{
    const uint8_t *p = buf;

    aborted = 0;
    do {
        uint32_t n = len > 3 * usb.mps ? 3 * usb.mps : len;
        uint32_t pk = n ? (n + usb.mps - 1) / usb.mps : 1, t0 = ticks();

        in_done = 0;
        REG(DIEPTSIZ(1)) = pk << 19 | n;
        REG(DIEPCTL(1)) |= EP_ENA | EP_CNAK;
        for (uint32_t k = 0; k < n; k += usb.mps) {
            uint32_t m = n - k > usb.mps ? usb.mps : n - k;
            int r = fifo_write(1, p + k, m);
            if (r)
                return r;
        }
        while (!in_done) {
            usb_service();
            if (aborted)
                return USB_EABORT;
            if (ticks() - t0 > IO_TIMEOUT_US)
                return USB_ETIMEOUT;
            wdt_kick();
        }
        p += n;
        len -= n;
    } while (len);
    return 0;
}

/* Clocks, PHY and resets as RECHO 0.10.0 sets them before its own USB disk mode (observed in
 * emulation of 0x03054fb8; values only): hclk/PHY gates 7 and 6*16+10 on, soft resets 1, 2 and
 * 0 released, GRF USB PHY registers 0x80-0xb8, and three GPIOs (purpose unknown, kept as
 * RECHO has them in that mode). Then a DWC2 core soft reset in device mode. */
/* ---- interrupt mode: the events usb_service() handles, interrupt 5 of the NVIC ---- */
#define IRQ_USB 5
static int irq_on;

void OTG_IRQHandler(void)
{
    uint32_t t0 = ticks();
    if (irq_on)
        usb_service();
    else
        REG(GINTMSK) = 0;
    usb.irqs++;
    int32_t d = (int32_t)(ticks() - t0);        /* ticks() may step back by a SysTick pending */
    if (d > (int32_t)usb.irq_max_us)
        usb.irq_max_us = (uint32_t)d;
}

void usb_irq(int on)
{
    irq_on = on;
    if (on) {
        REG(GINTMSK) = GINT_RXFLVL | GINT_USBSUSP | GINT_USBRST | GINT_ENUMDONE | GINT_IEPINT |
                       GINT_OEPINT | GINT_IISOIXFR | GINT_INCOMPLP | GINT_WKUP;
        REG(GAHBCFG) |= 1;                              /* the core's interrupt line */
        REG(NVIC_ISER) = 1u << IRQ_USB;
    } else {
        REG(NVIC_ICER) = 1u << IRQ_USB;
        REG(GAHBCFG) &= ~1u;
        REG(GINTMSK) = 0;
    }
}

void usb_irq_block(int block)
{
    if (irq_on)
        REG(block ? NVIC_ICER : NVIC_ISER) = 1u << IRQ_USB;
}

void usb_start(void)
{
    usb_start_class(0);
}

void usb_start_class(const struct usb_class *c)
{
    cls = c;
    iso_mps = iso_in_mps = 0;
    usb.iso_in_sent = usb.iso_in_late = 0;
    ep0_rx_want = 0;
    usb.iso_incomplete = 0;
    usb.configured = 0;
    usb.suspended = 0;
    usb.mps = 512;
    usb.high_speed = 0;
    usb.bot_reset = 0;

    clk_gate(7, 1);
    clk_gate(6 * 16 + 10, 1);
    soft_reset(1, 0);
    soft_reset(2, 0);
    soft_reset(0, 0);
    spin_us(5);
    REG(GRF_BASE + 0xb8) = 0x00010000;
    REG(GRF_BASE + 0xb4) = 0x000e0002;
    REG(GRF_BASE + 0x98) = 0x00040004;
    REG(GRF_BASE + 0xb4) = 0x00010000;
    spin_us(200);
    mdelay(10);
    REG(GRF_BASE + 0x88) = 0x0c000c00;
    REG(GRF_BASE + 0x84) = 0x1fe00003;
    REG(GRF_BASE + 0x8c) = 0x001c0014;
    REG(GRF_BASE + 0x80) = 0x00070004;
    if (!c || !c->keep_audio_pins) {    /* the DACs' pins (audio.c): as RECHO's disk mode */
        iomux(1, 4, 0);
        gpio_set(1, 4, 0);
        gpio_dir(1, 4, 1);
        iomux(2, 0, 0);
        gpio_set(2, 0, 1);
        gpio_dir(2, 0, 1);
        iomux(2, 2, 0);
        gpio_set(2, 2, 1);
        gpio_dir(2, 2, 1);
    }

    REG(DCTL) |= DCTL_SFTDISCON;
    for (uint32_t t0 = ticks(); !(REG(GRSTCTL) & 1u << 31) && ticks() - t0 < 10000;)
        ;                                               /* AHB idle */
    REG(GRSTCTL) |= 1;                                  /* core soft reset */
    wait_reg_clear(GRSTCTL, 1, 10000);
    REG(PCGCCTL) = 0xf;
    mdelay(10);
    REG(PCGCCTL) = 0;
    REG(GAHBCFG) |= 1u << 7;                            /* TX FIFO empty level: completely empty */
    REG(DCFG) &= ~(3u | 0x7f0u);                        /* high speed, address 0 */
    if (c && c->full_speed)
        REG(DCFG) |= 1;                                 /* full speed on the UTMI+ PHY */
    static uint32_t rx_size, nptx_size;                 /* the sizes the core starts with */
    if (!rx_size) {
        rx_size = REG(GRXFSIZ);
        nptx_size = REG(GNPTXFSIZ);
    }
    if (c && c->rx_fifo_words) {        /* the rest: the shared TX FIFO, then the periodic one */
        uint32_t np = FIFO_WORDS - c->rx_fifo_words - c->ptx_fifo_words;
        REG(GRXFSIZ) = c->rx_fifo_words;
        REG(GNPTXFSIZ) = np << 16 | c->rx_fifo_words;
        if (c->ptx_fifo_words)
            REG(DPTXFSIZ1) = (uint32_t)c->ptx_fifo_words << 16 | (c->rx_fifo_words + np);
    } else {
        REG(GRXFSIZ) = rx_size;
        REG(GNPTXFSIZ) = nptx_size;
    }
    flush_fifos();

    REG(DIEPCTL(0)) = EP_SNAK;
    REG(DOEPCTL(0)) = EP_SNAK;
    REG(DIEPTSIZ(0)) = 0;
    REG(DOEPTSIZ(0)) = 0;
    REG(DIEPINT(0)) = 0xff;
    REG(DOEPINT(0)) = 0xff;
    REG(DIEPMSK) = INT_XFERCOMPL;
    REG(DOEPMSK) = INT_XFERCOMPL | INT_SETUP;
    REG(DAINTMSK) = 0x00030003;
    REG(GINTMSK) = 0;                                   /* polled */
    REG(GINTSTS) = 0xffffffff;
    REG(DCTL) &= ~DCTL_SFTDISCON;                       /* connect */
}

void usb_stop(void)
{
    usb_irq(0);
    REG(DCTL) |= DCTL_SFTDISCON;
    usb.configured = 0;
}
