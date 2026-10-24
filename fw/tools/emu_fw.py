#!/usr/bin/env python3
"""Run a newfw image under Unicorn, loaded the way the IDB loader does it.

  emu_fw.py IMG [--steps N] [--png OUT.png] [--adc1 V] [--power 0|1] [--sel 0|1] [--fast 0|1]
            [--trace-lcd] [--elf build/fw.elf] [--disk DISK.IMG] [--keys MS:KEY,...]
            [--shots MS:OUT.png,...] [--expect-fw FILE]

The loader is reproduced from re/newfw/ETUDE.md 2.2: header check against its copy at fw_end,
overlay table at [0x6c], code and data of entries 0-2 copied, BSS cleared, then SP, PC from the
vector table at the start of the data of entry 0, r0 = table, r1 = SP, r2 = reset.
Models: CRU (PLL locked), SARADC (value per channel), GPIO inputs (Power key, panel variant
pin), VOP (8-bit bus decoded as ST7789 commands into a 320x170 framebuffer), eMMC (dw_mmc
PIO model over a disk image; writes kept in memory, never written back, and checked against
the firmware area), time (ticks() replaced by a counter advancing 50 us per call).
--keys holds a key from a simulated time on (VOL-, VOL+, PLAY, MENU, BACK, NONE), --shots
saves the screen at given times, --expect-fw checks FW1 (and FW2 when it fits) against a file,
--pcm saves core B's output, --dac what the DMA reads from the DAC ring (what is heard).
"""
import os
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
                               UC_ARM_REG_PC)


def opt(flag, default):
    a = sys.argv
    return a[a.index(flag) + 1] if flag in a else default


def symbols(path):
    e = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", e, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", e, 0x2e)
    sh = [struct.unpack_from("<10I", e, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in sh:
        if s[1] == 2:                                           # SHT_SYMTAB
            strtab = sh[s[6]]
            for i in range(s[5] // 16):
                nm, val = struct.unpack_from("<II", e, s[4] + 16 * i)
                name = e[strtab[4] + nm:].split(b"\0")[0].decode()
                out[name] = val
    return out


def load(u, d):
    fw_end = struct.unpack_from("<I", d, 0x1f4)[0]
    assert d[0x1f8:0x200] == b"RKnanoFW" and d[:0x200] == d[fw_end:fw_end + 0x200], "header"
    code_off = struct.unpack_from("<I", d, 0x6c)[0]
    tab = d[code_off:code_off + 0x200]
    base = struct.unpack_from("<I", tab, 0)[0]
    for i in range(3):
        cl, ce, cs, dl, de, ds, be, bs = struct.unpack_from("<8I", tab, 8 + 32 * i)
        if cs:
            u.mem_write(ce, d[code_off + cl - base:code_off + cl - base + cs])
        if ds:
            u.mem_write(de, d[code_off + dl - base:code_off + dl - base + ds])
        if bs:
            u.mem_write(be, bytes(bs))
    vt = struct.unpack_from("<I", tab, 8 + 16)[0]               # entry 0 data exec
    sp, pc = struct.unpack("<II", u.mem_read(vt, 8))
    return vt, sp, pc


class Panel:
    def __init__(self):
        self.fb = [[0] * 320 for _ in range(170)]
        self.cmd, self.args, self.cmds = None, [], []
        self.x0 = self.x1 = self.y0 = self.y1 = self.x = self.y = 0
        self.hi = None
        self.pixels = 0

    def command(self, c):
        self.flush()
        self.cmd, self.args = c, []
        if c == 0x2c:
            self.x, self.y, self.hi = self.x0, self.y0, None

    def flush(self):
        if self.cmd is not None and self.cmd != 0x2c:
            self.cmds.append((self.cmd, self.args))
        elif self.cmd == 0x2c:
            self.cmds.append((0x2c, [f"{self.pixels} px"]))
            self.pixels = 0

    def data(self, v):
        v &= 0xff
        if self.cmd == 0x2c:
            if self.hi is None:
                self.hi = v
                return
            px, self.hi = self.hi << 8 | v, None
            if 0 <= self.y - 35 < 170 and 0 <= self.x < 320:
                self.fb[self.y - 35][self.x] = px
            self.pixels += 1
            self.x += 1
            if self.x > self.x1:
                self.x, self.y = self.x0, self.y + 1
            return
        self.args.append(v)
        if self.cmd == 0x2a and len(self.args) == 4:
            self.x0, self.x1 = self.args[0] << 8 | self.args[1], self.args[2] << 8 | self.args[3]
        if self.cmd == 0x2b and len(self.args) == 4:
            self.y0, self.y1 = self.args[0] << 8 | self.args[1], self.args[2] << 8 | self.args[3]

    def png(self, path, scale=2):
        import zlib
        raw = b""
        for row in self.fb:
            line = b"".join(bytes(((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                                   (p & 31) * 255 // 31)) * scale for p in row)
            raw += (b"\0" + line) * scale

        def chunk(t, b):
            return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
        ihdr = struct.pack(">IIBBBBB", 320 * scale, 170 * scale, 8, 2, 0, 0, 0)
        open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                               + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


CLOCK = 0x0309f000
FW1 = 0x2000


class MMC:
    """dw_mmc in PIO mode: CMD17/18 reads, CMD24/25 writes, auto-stop; disk image read-only,
    written sectors kept in a dictionary."""

    def __init__(self, path, log):
        self.f = open(path, "rb") if path else None
        self.log, self.regs, self.written = log, {}, {}
        self.rq, self.wq, self.wexp, self.wlba = [], [], 0, 0
        self.rint, self.auto, self.cmds = 0, 0, 0
        self.order = []                                           # write runs, in order
        self.p = 0x1b000                                          # from the IDB of BOOTDUMP

    def sector(self, n):
        if n in self.written:
            return self.written[n]
        if not self.f:
            return bytes(512)
        self.f.seek(n * 512)
        return self.f.read(512).ljust(512, b"\0")

    def read(self, off):
        if off == 0x2c:
            return self.regs.get(0x2c, 0) & 0x7fffffff
        if off == 0x00:
            return self.regs.get(0, 0) & ~7
        if off == 0x44:
            return self.rint
        if off == 0x48:
            n = min(16, len(self.rq))
            return n << 17 | (4 if not self.rq else 0)
        if off == 0x200:
            v = self.rq.pop(0) if self.rq else 0
            if not self.rq:
                self.rint |= 8 | (0x4000 if self.auto else 0)
            return v
        return self.regs.get(off, 0)

    def write(self, off, val):
        self.regs[off] = val
        if off == 0x44:
            self.rint &= ~val
        elif off == 0x2c and val & 0x80000000:
            self.command(val)
        elif off == 0x200:
            self.wq.append(val)
            if len(self.wq) == self.wexp:
                data = struct.pack("<%dI" % len(self.wq), *self.wq)
                for i in range(len(data) // 512):
                    lba = self.wlba + i
                    if not FW1 <= lba < FW1 + self.p and not 0x81000 <= lba:
                        self.log.append(f"!! WRITE OUTSIDE THE FIRMWARE AREA: sector {lba:#x}")
                    self.written[lba] = data[512 * i:512 * (i + 1)]
                if self.order and self.order[-1][1] == self.wlba:
                    self.order[-1][1] += len(data) // 512
                else:
                    self.order.append([self.wlba, self.wlba + len(data) // 512])
                self.wq = []
                self.rint |= 8 | (0x4000 if self.auto else 0)

    def command(self, val):
        idx, arg = val & 0x3f, self.regs.get(0x28, 0)
        data, wr, self.auto = val & 0x200, val & 0x400, val & 0x1000
        self.cmds += 1
        self.rint |= 4
        if not data:
            return
        n = self.regs.get(0x20, 0) // 512
        if wr:
            if idx not in (24, 25):
                self.log.append(f"!! write with CMD{idx}")
            self.wq, self.wexp, self.wlba = [], n * 128, arg
        elif idx == 8:                                            # EXT_CSD: SEC_COUNT
            size = 0xE90000
            if self.f:
                self.f.seek(0, 2)
                size = max(size, self.f.tell() // 512)
            raw = bytearray(512)
            raw[212:216] = struct.pack("<I", size)
            self.rq = list(struct.unpack("<128I", bytes(raw)))
        else:
            raw = b"".join(self.sector(arg + i) for i in range(n))
            self.rq = list(struct.unpack("<%dI" % (len(raw) // 4), raw))

    def report(self, expect):
        if not self.written:
            print(f"eMMC: {self.cmds} commands, nothing written")
            return
        lbas = sorted(self.written)
        runs, start = [], lbas[0]
        for a, b in zip(lbas, lbas[1:] + [None]):
            if b != a + 1:
                runs.append((start, a))
                start = b
        print(f"eMMC: {self.cmds} commands, {len(lbas)} sectors written: " +
              ", ".join(f"{a:#x}-{b:#x}" for a, b in runs))
        print("  order: " + ", ".join(f"{a:#x}-{b - 1:#x}" for a, b in self.order))
        if expect:
            img = open(expect, "rb").read()
            fw_end = struct.unpack_from("<I", img, 0x1f4)[0]
            size = (fw_end + 0x104000) & ~0xfffff
            for name, base in (("FW1", FW1), ("FW2", FW1 + self.p // 2)):
                got = b"".join(self.sector(base + i) for i in range(size // 512))
                print(f"  {name}: {'== image' if got == img[:size] else 'differs from the image'}")


class SDCard(MMC):
    """microSD card behind the second dw_mmc (0x60020000): SD 2.0 identification (CMD0, CMD8,
    ACMD41, CMD2, CMD3, CMD9, CMD7, ACMD6), SDHC block addressing, reads only."""

    def __init__(self, path, log):
        super().__init__(path, log)
        self.app = False
        self.f.seek(0, 2)
        self.size = self.f.tell() // 512

    def command(self, val):
        if val & (1 << 21):                                       # update clock: no command
            return
        idx, arg = val & 0x3f, self.regs.get(0x28, 0)
        app, self.app = self.app, False
        r = {0x30: 0x900, 0x34: 0, 0x38: 0, 0x3c: 0}
        if idx == 8 and not app:
            r[0x30] = arg & 0xfff
        elif idx == 55:
            self.app, r[0x30] = True, 0x920
        elif idx == 41 and app:
            r[0x30] = 0xC0FF8000
        elif idx == 3:
            r[0x30] = 0x12340500
        elif idx == 9:                                            # CSD 2.0
            c = self.size // 1024 - 1
            r = {0x30: 0, 0x34: (c & 0xffff) << 16, 0x38: c >> 16, 0x3c: 0x40000000}
        elif idx == 2:
            r = {0x30: 0x01020304, 0x34: 0, 0x38: 0, 0x3c: 0x03534453}
        self.regs.update(r)
        if val & 0x200 and val & 0x400:
            self.log.append(f"!! SD write with CMD{idx}")
            return
        super().command(val)


def main():
    img = sys.argv[1]
    d = open(img, "rb").read()
    sym = symbols(opt("--elf", img.rsplit("/", 1)[0] + "/fw.elf"))
    steps = int(opt("--steps", "60000000"))
    adc = {0: 820, 1: int(opt("--adc1", "1023")), 2: 300, 3: 300}
    gpio_in = {(2, 5): int(opt("--power", "0")), (2, 9): int(opt("--sel", "0")),
               (0, 13): 0, (2, 8): 1, (0, 11): 0 if opt("--sd", None) else 1}

    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    from emu_coreb import Shared, CoreB                           # core B on demand (step 5c)
    from unicorn import UC_PROT_ALL
    sh = Shared()
    u.mem_map_ptr(0x03000000, 0xA0000, UC_PROT_ALL, sh.sram)      # SRAM, PMU-SRAM (B sees it)
    u.mem_map_ptr(0x01000000, 0x20000, UC_PROT_ALL, sh.hr0)       # HIGHRAM0, shared with core B
    u.mem_map_ptr(0x01020000, 0x40000, UC_PROT_ALL, sh.hr1)       # HIGHRAM1
    if opt("--fast", "0") == "1":
        u.mem_write(0x0309ffe0, b"FAST")
    vt, sp, pc = load(u, d)
    # Unicorn handles the system space itself and has no running SysTick: ticks() is replaced
    # by a counter that advances by 50 us per call (stored in unused PMU-SRAM).
    t = sym["ticks"] & ~1
    stub = bytes.fromhex("0249" "0868" "3230" "0860" "7047" "00bf") + struct.pack("<I", CLOCK)
    if t & 2:
        stub = bytes.fromhex("00bf") + stub
    u.mem_write(t, stub)
    if "millis" in sym:                       # no SysTick: from the counter, by a stub next to it
        ms = (bytes.fromhex("0349" "0868" "40f2e831" "b0fbf1f0" "7047" "00bf")   # ldr r1, =CLOCK;
              + struct.pack("<I", CLOCK))         # ldr r0, [r1]; movw r1, #1000; udiv r0, r0, r1
        u.mem_write(CLOCK + 0x10, ms)
        m = sym["millis"] & ~1                    # millis: ldr.w pc, [pc, #0] (aligned) to it
        jump = bytes.fromhex("dff800f0") + struct.pack("<I", (CLOCK + 0x10) | 1)
        u.mem_write(m, (bytes.fromhex("00bf") if m & 2 else b"") + jump)
    if "cpu_wait" in sym:                                         # WFI: no SysTick here
        u.mem_write(sym["cpu_wait"] & ~1, bytes.fromhex("7047"))

    # core B runs alongside: 5000 instructions (50 us at 100 MHz) per ticks() call, while it is
    # awake (our runtime has no mailbox polling from core A to drive it)
    pcm_out = open(opt("--pcm", None), "wb", buffering=0) if opt("--pcm", None) else None
    # --dac: what the DMA reads from the ring, captured at each read of its position (the
    # producer never writes past the last position read, so those bytes are still there)
    dac_out = open(opt("--dac", None), "wb", buffering=0) if opt("--dac", None) else None
    cap = {"start": None, "done": 0}

    OUT_OFF, OUT_SIZE = 0x38000, 0x8000                          # core B output ring in HIGHRAM1

    def capture():
        # our runtime: everything core B wrote to its output ring (out_wr: offset in the ring)
        st, out_wr, frames = (struct.unpack_from("<I", sh.hr1, o)[0] for o in (0x0c, 0x30, 0x48))
        if struct.unpack_from("<I", sh.hr1, 0)[0] != 0x5452424e or st not in (3, 4, 5):
            cap["start"] = None
            return
        if st == 3:
            cap["start"] = None
            return
        if cap["start"] is None:                                 # first look after PLAY
            fb = struct.unpack_from("<I", sh.hr1, 0x44)[0]
            cap["start"] = (out_wr - frames * fb) % OUT_SIZE
        last = cap["start"]
        while last != out_wr:
            n = (out_wr - last) % OUT_SIZE
            n = min(n, OUT_SIZE - last)
            pcm_out.write(sh.hr1[OUT_OFF + last:OUT_OFF + last + n])
            last = (last + n) % OUT_SIZE
        cap["start"] = last

    def coreb_tick(uc, addr, size, _):
        if sh.b and not sh.b.sleeping:
            sh.b.step(5000)
        if pcm_out and sh.b:
            capture()
    u.hook_add(UC_HOOK_BLOCK, coreb_tick, begin=t, end=t + 4)
    panel, log, regs = Panel(), [], {}
    state = {"adc_ch": 0}
    keyval = {"VOL-": 1, "VOL+": 126, "PLAY": 270, "MENU": 384, "BACK": 515, "NONE": 1023}
    keys = sorted((int(t), keyval[k] if k in keyval else int(k)) for t, k in (x.split(":") for x in opt("--keys", "").split(",") if x))
    shots = sorted((int(t), f) for t, f in (x.split(":") for x in opt("--shots", "").split(",") if x))
    mmc = MMC(opt("--disk", None), log)
    sdc = SDCard(opt("--sd", None), log) if opt("--sd", None) else None
    sd_out = int(opt("--sd-out", "0")) or None                    # ms: the card is removed
    adc0_at = sorted(tuple(int(v) for v in x.split(":"))            # MS:VALUE, ... (mV * 1024 / 5000)
                     for x in opt("--adc0-at", "").split(",") if x)
    gpio_at = sorted(tuple(int(v) for v in x.split(":"))            # MS:BANK:PIN:LEVEL, ...
                     for x in opt("--gpio-at", "").split(",") if x)

    def now_ms(uc):
        return struct.unpack("<I", uc.mem_read(CLOCK, 4))[0] // 1000

    def events(uc):
        t = now_ms(uc)
        while keys and keys[0][0] <= t:
            adc[1] = keys.pop(0)[1]
        while adc0_at and adc0_at[0][0] <= t:                   # --adc0-at: the battery
            adc[0] = adc0_at.pop(0)[1]
        while shots and shots[0][0] <= t:
            panel.png(shots.pop(0)[1])

    # PCF8563-type real-time clock at 0xA2 on I2C1 (ETUDE §28): starts as the device's
    # (2025-05-07 04:37:22, VL set) and runs with the simulated time; writes set it
    import datetime as _dt
    rtc = {"base": _dt.datetime(2025, 5, 7, 4, 37, 22), "t0": 0, "vl": 1, "ptr": 0}

    def rtc_regs(uc):
        now = struct.unpack("<I", uc.mem_read(CLOCK, 4))[0]
        t = rtc["base"] + _dt.timedelta(microseconds=now - rtc["t0"])
        bcd = lambda v: (v // 10) << 4 | v % 10
        return bytes([0, 0, bcd(t.second) | (0x80 if rtc["vl"] else 0), bcd(t.minute), bcd(t.hour),
                      bcd(t.day), (t.weekday() + 1) % 7, bcd(t.month), bcd(t.year % 100)]) + bytes(7)

    def periph(base):
        def rd(uc, off, size, _):
            a = base + off
            if a == 0x4005001c:                                  # I2C1 IPD: all done, no NAK
                return 0x3f
            if 0x40050200 <= a < 0x40050220:                     # I2C1 RXDATA: RTC registers
                r = rtc_regs(uc) + bytes(32)
                k = rtc["ptr"] + 4 * ((a - 0x40050200) // 4)
                return int.from_bytes(r[k:k + 4], "little")
            if a == 0x40180004:
                return regs.get(a, 0) | 0x0400                   # PLL locked (bit 10)
            if a == 0x400d0000:
                events(uc)
                return adc[state["adc_ch"]]
            if 0x60030000 <= a < 0x60034000:
                return mmc.read(a - 0x60030000)
            if 0x60020000 <= a < 0x60024000:
                return sdc.read(a - 0x60020000) if sdc else regs.get(a, 0) & 0x7fffffff
            if a == 0x400d0004:
                return 0
            if a == 0x4004001c:                                  # I2C0 IPD: all done, no NAK
                return 0x3f
            if a == 0x40030020:                                  # I2S1 CLR: done
                return 0
            if a == 0x500100e8:
                return 2 if opt("--vbus", "0") == "1" else 0
            if a == 0x600001b8 and "dma" in state:               # DMAC1 ch5 SAR: moves with time
                dbase, total, t0, bps = state["dma"]
                now = struct.unpack("<I", uc.mem_read(CLOCK, 4))[0]
                pos = (now - t0) * bps // 1000000 // 32 * 32
                if dac_out:
                    prev = max(state.get("dac_pos", 0), pos - total)
                    while prev < pos:
                        o = prev % total
                        k = min(pos - prev, total - o)
                        dac_out.write(uc.mem_read(dbase + o, k))
                        prev += k
                    state["dac_pos"] = pos
                return dbase + pos % total
            if 0x40110000 <= a < 0x40110100:                     # mailbox
                if a == 0x4011002c and sh.b and not sh.get(0x2c) and not sh.b.sleeping:
                    sh.b.step(200_000)                            # let core B work while A polls
                return sh.get(a - 0x40110000)
            if a in (0x40160050, 0x40170050, 0x50030050):
                bank = {0x40160050: 0, 0x40170050: 1, 0x50030050: 2}[a]
                if sd_out is not None and now_ms(uc) >= sd_out:          # card pulled out
                    gpio_in[(0, 11)] = 1
                while gpio_at and gpio_at[0][0] <= now_ms(uc):         # --gpio-at changes
                    _, gb, gp, gv = gpio_at.pop(0)
                    gpio_in[(gb, gp)] = gv
                v = 0
                for (b, p), lvl in gpio_in.items():
                    if b == bank and lvl:
                        v |= 1 << p
                return v
            return regs.get(a, 0)

        def wr(uc, off, size, val, _):
            a = base + off
            regs[a] = val
            if 0x40110000 <= a < 0x40110100:                     # mailbox
                ch = sh.write(a - 0x40110000, val, True)
                if ch is not None and sh.b:
                    sh.b.deliver(ch)
                return
            if a == 0x600003a0 and val & 0x2000:                 # DMAC1 ChEnReg, channel 5
                if val & 0x20:
                    dbase, total, llp, seen = regs.get(0x600001b8, 0), 0, regs.get(0x600001c8, 0), set()
                    while llp and llp not in seen:
                        seen.add(llp)
                        _, _, llp, _, ctlh = struct.unpack("<5I", uc.mem_read(llp, 20))
                        total += (ctlh & 0xfff) * 4
                    fb = 4 if regs.get(0x40030000, 0) & 0x1f == 0x0f else 8
                    rate = int(opt("--rate", "44100"))
                    now = struct.unpack("<I", uc.mem_read(CLOCK, 4))[0]
                    if total:
                        state["dma"] = (dbase, total, now, rate * fb)
                        state["dac_pos"] = 0
                else:
                    state.pop("dma", None)
            if a == 0x401800cc and val & 0x40000:                # core B reset (SOFTRST3 bit 2)
                if val & 4:
                    sh.b = None
                else:
                    sh.b = CoreB(sh, "--trace-b" in sys.argv)
                    if os.environ.get("EMU_WATCH"):
                        lo, hi = (int(v, 0) for v in os.environ["EMU_WATCH"].split(":"))
                        sh.b.watch(lo, hi)
                    log.append(f"core B started: SP {sh.b.sp0:#x} PC {sh.b.pc:#x}")
            if 0x60030000 <= a < 0x60034000:
                mmc.write(a - 0x60030000, val)
            elif 0x60020000 <= a < 0x60024000 and sdc:
                sdc.write(a - 0x60020000, val)
            elif a == 0x400d0008:
                state["adc_ch"] = val & 7
            elif a == 0x60070028:
                panel.command(val & 0xff)
            elif a == 0x6007002c:
                panel.data(val)
            elif a == 0x40040010:                                # I2C0 MTXCNT: one message
                b = b"".join(regs.get(0x40040100 + 4 * i, 0).to_bytes(4, "little")
                             for i in range(8))[:val]
                log.append("I2C TX " + b.hex(" "))
                return
            elif 0x40040000 <= a < 0x40040300:
                return
            elif a == 0x40050010:                                # I2C1 MTXCNT: [0xa2, reg, data]
                b = b"".join(regs.get(0x40050100 + 4 * i, 0).to_bytes(4, "little")
                             for i in range(8))[:val]
                if len(b) >= 2 and b[0] == 0xa2:
                    rtc["ptr"] = b[1]
                    if len(b) >= 9 and b[1] == 2:                # time set: registers 2 to 8
                        d = lambda v: (v >> 4) * 10 + (v & 15)
                        rtc["base"] = _dt.datetime(2000 + d(b[8]), d(b[7] & 0x1f), d(b[5] & 0x3f),
                                                   d(b[4] & 0x3f), d(b[3] & 0x7f), d(b[2] & 0x7f))
                        rtc["t0"] = struct.unpack("<I", uc.mem_read(CLOCK, 4))[0]
                        rtc["vl"] = b[2] >> 7
                        log.append("RTC set " + rtc["base"].isoformat())
                return
            elif 0x40050000 <= a < 0x40050300:
                return
            elif a == 0x5002004c:
                log.append(f"PMU_SYS_REG3 = {val:#x}")
            elif not (0x40160000 <= a < 0x40180000 or 0x50030000 <= a < 0x50040000
                      or a == 0x40120000 + 0xc):
                log.append(f"W {a:#010x} = {val:#x}")
        return rd, wr

    for base, size in ((0x40000000, 0x200000), (0x50000000, 0x40000), (0x60000000, 0xC0000)):
        rd, wr = periph(base)
        u.mmio_map(base, size, rd, None, wr, None)

    def st_rd(uc, off, size, _):
        return regs.get(0xE000E000 + off, 0)

    def st_wr(uc, off, size, val, _):
        regs[0xE000E000 + off] = val
    u.mmio_map(0xE0000000, 0x100000, st_rd, None, st_wr, None)

    u.reg_write(UC_ARM_REG_SP, sp)
    u.reg_write(UC_ARM_REG_R0, vt)
    u.reg_write(UC_ARM_REG_R1, sp)
    u.reg_write(UC_ARM_REG_R2, pc)
    try:
        u.emu_start(pc, 0xffffffff, count=steps)
    except Exception as ex:                                       # noqa: BLE001
        log.append(f"EXCEPTION {ex} at pc {u.reg_read(UC_ARM_REG_PC):#x}")
    panel.flush()
    c = struct.unpack_from("<24I", sh.hr1, 0)
    if c[0] == 0x5452424e:                                       # our runtime's control block
        names = ("magic version codec state error info cmd seq ack in_wr in_rd in_end out_wr "
                 "out_rd rate ch bits fb frames wakeups loops fpc flr cfsr").split()
        log.append("core B ctl: " + " ".join(f"{n}={v:#x}" for n, v in zip(names, c)))
        if sh.b:
            log.append(f"core B pc {sh.b.pc:#x} sleeping {sh.b.sleeping}")
    ms = struct.unpack("<I", u.mem_read(CLOCK, 4))[0] // 1000
    for l in log:
        print(l)
    print(f"-- stopped at pc {u.reg_read(UC_ARM_REG_PC):#x}, simulated time {ms} ms, "
          f"{len(panel.cmds)} panel commands")
    if "--trace-lcd" in sys.argv:
        for c, args in panel.cmds:
            print(f"  {c:02x} " + " ".join(f"{x:02x}" if isinstance(x, int) else x for x in args))
    if "--png" in sys.argv:
        panel.png(opt("--png", "screen.png"))
    mmc.report(opt("--expect-fw", None))
    if opt("--dump-disk", None):                                  # disk with the writes applied
        import shutil
        out = opt("--dump-disk", None)
        shutil.copyfile(opt("--disk", None), out)
        with open(out, "r+b") as f:
            for lba, data in mmc.written.items():
                f.seek(lba * 512)
                f.write(data)


if __name__ == "__main__":
    main()
