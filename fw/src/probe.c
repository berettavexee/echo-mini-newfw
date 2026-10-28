/* USB probe (test build only, make EXTRA=-DECHO_PROBE, ETUDE §72). The PC writes a request of
 * PROBE_SECTORS sectors at the start of PROBE.BIN (O_DIRECT, fw/tools/probe.py); msc.c hands
 * those sectors here instead of the eMMC; the request runs at once and its answer is what the
 * next read of the same sectors returns. Nothing reaches the eMMC. Commands: information,
 * peek and poke of 32-bit words within an allow-list (the PD_HIGH side: core B RAM, HIFIACC,
 * DMAC2; the MP3 accelerators), PD_HIGH on with every HiFi block reset (core B held), core B
 * run and stop, and the install of a firmware image of the root, done when USB mode ends
 * (the menu's checks and writing), then a reboot. */
#ifdef ECHO_PROBE
#include <stdint.h>
#include "board.h"
#include "fat.h"
#include "hifi.h"
#include "log.h"
#include "probe.h"
#include "regs.h"
#include "update.h"
#include "version.h"

static struct probe_msg req, ans;
static uint32_t box_lba, box_on, served;
static char pending[13];

int probe_attach(uint32_t base)
{
    struct fat_file f;
    uint32_t first, count;
    box_on = 0;
    served = 0;
    pending[0] = 0;
    if (fat_find_root("PROBE.BIN", &f) || f.size < PROBE_SECTORS * 512 ||
        fat_extent(&f, &first, &count) || count < PROBE_SECTORS || first < base)
        return 0;
    box_lba = first - base;
    box_on = 1;
    ans.magic = 0;
    log_printf("probe: mailbox PROBE.BIN at msc sector %x", box_lba);
    return 1;
}

int probe_hit(uint32_t lba, uint32_t n)
{
    return box_on && lba < box_lba + PROBE_SECTORS && lba + n > box_lba;
}

/* the PD_HIGH side as core A sees it, and the MP3 accelerators (core A addresses, TRM) */
static int allowed(uint32_t a, uint32_t words)
{
    uint32_t end = a + 4 * words;
    if (a & 3 || end < a)
        return 0;
    return (a >= 0x01000000u && end <= 0x01080000u) || (a >= 0x62040000u && end <= 0x62050000u);
}

static void run(void)
{
    uint32_t n = req.n, max = sizeof ans.data / 4;
    ans.magic = PROBE_ANS;
    ans.seq = req.seq;
    ans.cmd = req.cmd;
    ans.addr = req.addr;
    ans.n = 0;
    ans.count = ++served;
    ans.status = PROBE_OK;
    if (req.magic != PROBE_REQ) {
        ans.status = PROBE_EMAGIC;
        return;
    }
    switch (req.cmd) {
    case PROBE_INFO: {
        const char *v = "ECHO MINI newfw " FW_VERSION " probe 1";
        char *d = (char *)ans.data;
        int i = 0;
        while (v[i] && i < 63) {
            d[i] = v[i];
            i++;
        }
        d[i] = 0;
        ans.n = 16;
        break;
    }
    case PROBE_PEEK:
        if (n > max) {
            ans.status = PROBE_ELEN;
            break;
        }
        if (!allowed(req.addr, n)) {
            ans.status = PROBE_EADDR;
            break;
        }
        for (uint32_t i = 0; i < n; i++)
            ans.data[i] = REG(req.addr + 4 * i);
        ans.n = n;
        break;
    case PROBE_POKE:
        if (n > max) {
            ans.status = PROBE_ELEN;
            break;
        }
        if (!allowed(req.addr, n)) {
            ans.status = PROBE_EADDR;
            break;
        }
        for (uint32_t i = 0; i < n; i++)
            REG(req.addr + 4 * i) = req.data[i];
        break;
    case PROBE_POWER_B:
        hifi_prepare();                         /* PD_HIGH on, HiFi blocks reset, core B held */
        break;
    case PROBE_RUN_B:
        REG(CRU_SOFTRST(3)) = 0x00040000;       /* core B out of reset: runs from HIGHRAM0 */
        break;
    case PROBE_STOP_B:
        hifi_stop();
        break;
    case PROBE_INSTALL: {
        const char *s = (const char *)req.data;
        int i = 0;
        while (s[i] && i < 12) {
            pending[i] = s[i];
            i++;
        }
        pending[i] = 0;                         /* done when USB mode ends */
        break;
    }
    default:
        ans.status = PROBE_ECMD;
    }
}

void probe_write(uint32_t lba, uint32_t n, const void *data)
{
    const uint8_t *s = data;
    for (uint32_t k = 0; k < n; k++, lba++, s += 512) {
        if (lba < box_lba || lba >= box_lba + PROBE_SECTORS)
            continue;
        uint8_t *d = (uint8_t *)&req + (lba - box_lba) * 512;
        for (int i = 0; i < 512; i++)
            d[i] = s[i];
        if (lba == box_lba + PROBE_SECTORS - 1)  /* the request is complete: its last sector */
            run();
    }
}

void probe_read(uint32_t lba, uint32_t n, void *data)
{
    uint8_t *d = data;
    for (uint32_t k = 0; k < n; k++, lba++, d += 512) {
        if (lba < box_lba || lba >= box_lba + PROBE_SECTORS)
            continue;
        const uint8_t *s = (const uint8_t *)&ans + (lba - box_lba) * 512;
        for (int i = 0; i < 512; i++)
            d[i] = s[i];
    }
}

static void noprogress(int stage, uint32_t done, uint32_t total)
{
    (void)stage;
    (void)done;
    (void)total;
    wdt_kick();
}

void probe_after_usb(void)
{
    struct fat_file f;
    struct fw_info info;
    if (!pending[0])
        return;
    log_printf("probe: install %s requested over USB", pending);
    if (fat_find_root(pending, &f)) {
        log_printf("probe: %s not found", pending);
        pending[0] = 0;
        return;
    }
    int r = fw_check(&f, &info, noprogress);
    log_printf("probe: install %s: check %d", pending, r);
    if (!r) {
        r = fw_install(&f, &info, noprogress);
        log_printf("probe: install %s: result %d", pending, r);
        if (!r)
            reboot();
    }
    pending[0] = 0;
}
#endif
