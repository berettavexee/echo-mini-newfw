#!/usr/bin/env python3
"""Run a stock HiFi-core codec module from its reset vector, driven by a fake main core.

  emu_host.py FW.IMG MODULE FILE OUT.raw [--codec N] [--seconds S] [--trace]

Unlike tools/emu74.py, which calls the decoder entry directly, this runs the whole module
(runtime included) and talks to it only through the mailbox, as the main core does. It
checks the host protocol a new main-core firmware would implement:

  mailbox 0x40110000, instance 0 (stride 0x50):
    A2B  +0x00 int enable, +0x04 status (write 1 to clear), +0x08 + 8*ch cmd, +0x0c + 8*ch data
    B2A  +0x28 int enable, +0x2c status,                    +0x30 + 8*ch cmd, +0x34 + 8*ch data
  A2B ch1 (HiFi IRQ 0x1a): 1 OPEN, 4 DECODE, 0xb SEEK(ms), 0xd CLOSE
  A2B ch2 (HiFi IRQ 0x1b): 0x65 file descriptor, replies 0x6a seek, 0x6c read (data = result)
  B2A ch0: 1 ready, 2 open failed, 3 opened (&info), 5 decoded (&info), 6 end, 0xc seek, 0xe closed
  B2A ch2: 0x66 descriptor taken, 0x69 seek {off, whence, h}, 0x6b read {buf, size, h}
           (data = the request block; the main core writes the resulting file position back
           into it, at +0 for a seek and at +4 for a read, and the runtime copies it into its
           position table 0x01022228[h] of module 74)

Model: the HiFi core runs until it writes a B2A data register (message) or reaches the
runtime's WFI. Interrupts are delivered between two runs by calling the handler found in the
module's vector table (VTOR) on the interrupted context, as an exception would. The delay
helper reads the core clock from the parameter block (left zero: no delay). Peripherals
other than the mailbox are plain RAM. Only block hooks are used (Unicorn 2.1 mis-executes
Thumb IT blocks with code or memory-write hooks); the mailbox is an MMIO mapping.
"""
import ctypes
import os
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK, UC_PROT_ALL
from unicorn.arm_const import (UC_ARM_REG_APSR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                               UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_XPSR)

sys.path.insert(0, __file__.rsplit("/", 3)[0])
from rknanofw import overlays                                     # noqa: E402

MBOX, STOP = 0x40110000, 0x0001FFF0
DESC = 0x03010000                                                 # descriptor in main-core RAM
CODEC = {73: 0, 74: 1, 75: 2, 76: 3, 77: 4, 80: 5, 78: 6, 79: 7, 81: 8, 82: 9}
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
        UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
        UC_ARM_REG_PC, UC_ARM_REG_XPSR]

# EMU_COUNT=1: every MMIO access counted by (core, r/w, address), printed at the end (emu_fw.py)
COUNTS = {} if os.environ.get("EMU_COUNT") else None


def counted(core, base, rd, wr):
    """rd, wr as given, or wrapped to count their calls in COUNTS"""
    if COUNTS is None:
        return rd, wr

    def crd(uc, off, size, d):
        k = (core, "r", base + off)
        COUNTS[k] = COUNTS.get(k, 0) + 1
        return rd(uc, off, size, d)

    def cwr(uc, off, size, val, d):
        k = (core, "w", base + off)
        COUNTS[k] = COUNTS.get(k, 0) + 1
        return wr(uc, off, size, val, d)
    return crd, cwr


class Synth:
    """SYNTH, the MPEG polyphase synthesis (ETUDE.md sections 68-71 and 77, docs/MP3ACC.md), seen
    by core B at 0x60050000: +0x804 channel (bit 0) and phase, the 32 subband samples written to
    +0x000..+0x07c (the last one starts the computation), +0x80c nonzero when done (0x11 written
    to acknowledge), the 32 PCM samples read back from +0x000..+0x07c; a write to the history
    (+0x1000..+0x1ffc) clears it. Model: the standard synthesis (matrixing, then the window of
    libmpcdec's synth_filter.c, read from third_party/; numpy), output = round(sum / 2^13),
    saturated to 16 bits; one history per channel, the phase not checked. Against libmpcdec with the
    device's input scale: within 1 LSB (the device: within 1-2 LSB of pvmp3dec and libmpcdec),
    not bit-exact.
    """
    BASE = 0x60050000
    OFF = (0, 96, 128, 224, 256, 352, 384, 480, 512, 608, 640, 736, 768, 864, 896, 992)

    def __init__(self):
        import numpy as np
        import re
        self.np = np
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "libmpcdec",
                           "synth_filter.c")
        tab = open(src).read()
        tab = tab[tab.index("Di_opt [32] [16]"):]
        d = [int(v) / 65536 for v in re.findall(r"_\(\s*(-?\d+)\)", tab[:tab.index("};")])]
        self.win = np.array(d).reshape(32, 16)                     # window, per output k
        self.idx = np.arange(32)[:, None] + np.array(self.OFF)[None, :]   # its taps in the history
        i, k = np.arange(64)[:, None], np.arange(32)[None, :]
        self.n = np.cos((16 + i) * (2 * k + 1) * np.pi / 64)       # matrixing
        self.io = [0] * 32
        self.reg = {}
        self.clear()

    def clear(self):
        self.v = [self.np.zeros(1024), self.np.zeros(1024)]

    def slot(self):
        np = self.np
        v = self.v[self.reg.get(0x804, 0) & 1]
        x = np.array(self.io, dtype=np.uint32).view(np.int32).astype(float)
        v[64:] = v[:960].copy()
        v[:64] = self.n @ x
        y = np.clip(np.rint((v[self.idx] * self.win).sum(axis=1) / 8192), -32768, 32767)
        self.io = [int(t) & 0xffffffff for t in y.astype(np.int64)]
        self.reg[0x80c] = 1

    def rd(self, uc, off, size, _):
        return self.io[off >> 2] if off < 0x80 else self.reg.get(off, 0)

    def wr(self, uc, off, size, val, _):
        if off < 0x80:
            self.io[off >> 2] = val
            if off == 0x7c:
                self.slot()
        elif off == 0x80c:
            self.reg[off] = 0 if val == 0x11 else val
        elif off >= 0x1000:
            self.clear()
        else:
            self.reg[off] = val


class Accel:
    """HIFIACC LPC (FLAC FIR) and DMAC2 model, from the register map of ETUDE.md section 1.5.

    HIFIACC 0x01060000: +0x04 bit 15 mode, +0x08 bit 15 done, +0x0c 0xa0000000 | n << 15
    (| 3 = start), +0x2c order << 16 | shift << 8, +0x40 input FIFO (warm-up samples then
    residuals), +0xa0 output FIFO, coefficients at 0x0106b000 in reverse order.
    DMAC2 0x01070000 (DesignWare, 2 channels, stride 0x58): a channel runs when enabled
    through ChEnReg (+0x3a0); single blocks and LLI chains are followed; RawTfr/RawBlock are
    set and IRQ 0x38 is raised if the channel is unmasked. Memory-to-memory blocks are copied.

    FFT (ETUDE.md section 73, measured on the device): +0x0c function 2 (0x40000000, | 3 =
    start); +0x24 size << 29 (64: 0 .. 1024: 4), bit 27 no scaling (0: result / N), bit 20
    real input (imaginary part ignored); imaginary parts at 0x01061000, real parts at
    0x01062000, inputs in bit-reversed order, forward transform in natural order, in place.
    While +0x0c still selects the FFT, the data memory reads as one value (here FFT_BUSY), as
    on the device: the code must write 0 to +0x0c before reading. The model is the exact
    transform rounded to integers; the device's twiddles have about 14 bits (error up to
    0.008 % of the peak), so results are close, not bit-exact.
    """
    HACC, DMA, FIN, FOUT, COEF = 0x01060000, 0x01070000, 0x01060040, 0x010600a0, 0x0106b000
    FFT_IM, FFT_RE, FFT_BUSY = 0x01061000, 0x01062000, 0x0badf00d

    def __init__(self, u, hifi):
        self.u, self.h, self.reg = u, hifi, {}
        self.fin, self.fout, self.pending = [], [], []

    def rd(self, uc, off, size, _):
        a = 0x01060000 + off
        if self.FFT_IM <= a < self.FFT_RE + 0x1000 and self.reg.get(self.HACC + 0x0c, 0) >> 29 == 2:
            return self.FFT_BUSY                                  # memory owned by the engine
        if a == self.HACC + 8:
            return self.reg.get(a, 0)
        if a in (self.DMA + 0x2e8, self.DMA + 0x2f0):             # StatusTfr / StatusBlock
            raw = self.reg.get(a - 0x28, 0)
            return raw & self.reg.get(a + 0x28, 0)
        if a == self.FOUT:
            return self.fout.pop(0) & 0xffffffff if self.fout else 0
        return self.reg.get(a, 0)

    def wr(self, uc, off, size, val, _):
        a = 0x01060000 + off
        if a == self.FIN:
            self.fin.append(val)
            self._compute()
            return
        if self.DMA + 0x338 <= a <= self.DMA + 0x358 and (a - self.DMA - 0x338) % 8 == 0:
            raw = a - 0x78                                        # Clear* -> Raw*
            self.reg[raw] = self.reg.get(raw, 0) & ~val
            return
        if a in (self.DMA + 0x310, self.DMA + 0x318, self.DMA + 0x320, self.DMA + 0x3a0):
            old = self.reg.get(a, 0)                              # write-enable in bits 8..15
            we = (val >> 8) & 0xff
            val = (old & ~we) | (val & we & 0xff)
            self.reg[a] = val
            if a == self.DMA + 0x3a0:
                if not hasattr(self, "queue"):
                    self.queue = {}
                for ch in range(2):
                    if we & (1 << ch) and val & (1 << ch):
                        # (re)enabled. A channel reads its registers when it first moves data:
                        # at once from memory, or when the output FIFO has data. The stock FLAC
                        # module re-arms channel 0 after an interrupt and reprograms it later.
                        self.queue[ch] = None                     # registers read at start
                        if ch not in self.pending:
                            self.pending.append(ch)
                self._service()
            return
        self.reg[a] = val
        if a == self.HACC + 0x0c and val & 3 == 0:
            # configuration of a new run: the hardware FIFOs (32 words) start empty, whatever
            # the previous run left (the decoder may not read back a result it does not need)
            self.fin.clear()
            self.fout.clear()
        if a == self.HACC + 0x0c and val & 3 == 3 and val >> 29 == 2:
            self._fft()
            return
        if a == self.HACC + 0x0c and val & 3 == 3:
            self.n = (val >> 15) & 0x1fff
            self.reg[self.HACC + 8] = self.reg.get(self.HACC + 8, 0) & ~0x8000
            self._compute()

    def _compute(self):
        n = getattr(self, "n", 0)
        if not n or len(self.fin) < n:
            return
        cfg = self.reg.get(self.HACC + 0x2c, 0)
        order, shift = (cfg >> 16) & 0xff, (cfg >> 8) & 0xff
        coef = [struct.unpack("<i", struct.pack("<I", self.reg.get(self.COEF + 4 * i, 0)))[0]
                for i in range(order)]
        x = [struct.unpack("<i", struct.pack("<I", v & 0xffffffff))[0] for v in self.fin[:n]]
        del self.fin[:n]
        y = x[:order]
        for i in range(order, n):
            acc = sum(coef[k] * y[i - order + k] for k in range(order))
            y.append(x[i] + (acc >> shift))
        self.fout += y
        self.n = 0
        self.reg[self.HACC + 8] = self.reg.get(self.HACC + 8, 0) | 0x8000
        self._service()

    def _fft(self):
        import cmath
        cfg = self.reg.get(self.HACC + 0x24, 0)
        n = 64 << min((cfg >> 29) & 7, 4)
        s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v
        x = [complex(s32(self.reg.get(self.FFT_RE + 4 * i, 0)),
                     0 if cfg & (1 << 20) else s32(self.reg.get(self.FFT_IM + 4 * i, 0)))
             for i in range(n)]
        # the input is already in bit-reversed order: iterative radix-2 butterflies
        m = 2
        while m <= n:
            w = cmath.exp(-2j * cmath.pi / m)
            for k in range(0, n, m):
                t = 1
                for j in range(m // 2):
                    a, b = x[k + j], t * x[k + j + m // 2]
                    x[k + j], x[k + j + m // 2] = a + b, a - b
                    t *= w
            m *= 2
        scale = 1 if cfg & (1 << 27) else 1 / n
        for i, v in enumerate(x):
            self.reg[self.FFT_RE + 4 * i] = int(round(v.real * scale)) & 0xffffffff
            self.reg[self.FFT_IM + 4 * i] = int(round(v.imag * scale)) & 0xffffffff
        self.reg[self.HACC + 8] = self.reg.get(self.HACC + 8, 0) | 0x8000

    def _block(self, sar, dar, ctl, ctlh):
        """Run one DMA block; return False if it must wait for the accelerator."""
        width = 1 << ((ctl >> 4) & 7)
        n = ctlh & 0xfff
        if sar == self.FOUT and len(self.fout) < n:
            return False
        sinc, dinc = (ctl >> 9) & 3, (ctl >> 7) & 3
        for i in range(n):
            s = sar if sar in (self.FOUT,) or sinc else sar + i * width
            dd = dar if dar in (self.FIN,) or dinc else dar + i * width
            if s == self.FOUT:
                v = self.fout.pop(0) & 0xffffffff
            elif 0x01060000 <= s < 0x01080000:
                v = self.reg.get(s, 0)
            else:
                v = int.from_bytes(self.u.mem_read(s, width), "little")
            if dd == self.FIN:
                self.fin.append(v)
            elif 0x01060000 <= dd < 0x01080000:
                self.reg[dd] = v
            else:
                self.u.mem_write(dd, v.to_bytes(width, "little"))
        if dar == self.FIN:
            self._compute()
        return True

    def _service(self):
        if getattr(self, "busy", False):                          # re-entered from _compute
            self.again = True
            return
        self.busy, self.again = True, False
        try:
            self._service_loop()
        finally:
            self.busy = False

    def _blocks(self, ch):
        """The blocks of a channel: the registers, then its LLI chain (read at enable)."""
        b = self.DMA + 0x58 * ch
        r = self.reg.get
        blk = [(r(b, 0), r(b + 8, 0), r(b + 0x18, 0), r(b + 0x1c, 0))]
        ctl, llp, seen = r(b + 0x18, 0), r(b + 0x10, 0), set()
        while ctl & (3 << 27) and llp and llp not in seen:
            seen.add(llp)
            sar, dar, llp, ctl, ctlh = struct.unpack("<5I", self.u.mem_read(llp, 20))
            blk.append((sar, dar, ctl, ctlh))
        return blk

    def _service_loop(self):
        if not hasattr(self, "queue"):
            self.queue = {}
        progress = True
        while progress:
            progress = False
            for ch in list(self.queue):
                q = self.queue[ch]
                if q is None:
                    b = self.DMA + 0x58 * ch
                    if self.reg.get(b, 0) == self.FOUT and not self.fout:
                        continue                                  # waits for the accelerator
                    q = self.queue[ch] = self._blocks(ch)
                while q and self._block(*q[0]):
                    q.pop(0)
                    progress = True
                if q:
                    continue
                del self.queue[ch]
                if ch in self.pending:
                    self.pending.remove(ch)
                en = self.DMA + 0x3a0
                self.reg[en] = self.reg.get(en, 0) & ~(1 << ch)
                for raw in (self.DMA + 0x2c0, self.DMA + 0x2c8):  # RawTfr, RawBlock
                    self.reg[raw] = self.reg.get(raw, 0) | (1 << ch)
                if self.reg.get(self.DMA + 0x310, 0) & (1 << ch) or self.reg.get(self.DMA + 0x318, 0) & (1 << ch):
                    # pending until the core next stops (WFI or budget): stopping inside this
                    # MMIO callback would make Unicorn re-run the store that got us here
                    self.h.irqs.append(0x38)
                progress = True
            if self.again:
                self.again, progress = False, True


class Hifi:
    def __init__(self, img, m, path, trace):
        d = open(img, "rb").read()
        c, base, ents = overlays(d)
        cl, ce, cs, dl, de, ds, be, bs = ents[m]
        code = c[cl - base:cl - base + cs]
        self.f = open(path, "rb").read()
        self.trace = trace
        self.mb = bytearray(0x100)                                # mailbox registers
        self.msgs = []                                            # B2A (ch, cmd, data)
        u = self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        # HIGHRAM0 seen at 0 and at 0x01000000: one buffer, two mappings
        self.hr0 = ctypes.create_string_buffer(0x20000)
        u.mem_map_ptr(0, 0x20000, UC_PROT_ALL, self.hr0)
        u.mem_map_ptr(0x01000000, 0x20000, UC_PROT_ALL, self.hr0)
        u.mem_map(0x01020000, 0x40000)                            # HIGHRAM1
        self.acc = Accel(u, self)                                 # HIFIACC LPC + DMAC2 model
        u.mmio_map(0x01060000, 0x20000, self.acc.rd, None, self.acc.wr, None)
        self.irqs = []
        u.mem_map(0x03000000, 0xA0000)                            # main-core SRAM
        u.mem_map(0x40000000, MBOX - 0x40000000)
        u.mmio_map(MBOX, 0x1000, self._rd, None, self._wr, None)
        u.mem_map(MBOX + 0x1000, 0x400000 - 0x111000)
        u.mem_map(0x60000000, 0x100000)                            # DMAC1, IMDCT36, SDMMC, EMMC, EBC, SYNTH: RAM
        u.mem_map(0xE0000000, 0x100000)
        u.mem_write(0x01020000, b"\xa5" * 0x40000)                 # RAM content is unknown
        u.mem_write(0x01020000, bytes(0x90))                      # parameter block (zero)
        u.mem_write(0, code)
        if ds:
            u.mem_write(de, c[dl - base:dl - base + ds])
        self.wfi = code.find(bytes.fromhex("30bf7047"))           # wfi; bx lr
        if self.wfi < 0:
            raise SystemExit("no wfi helper in module %d" % m)
        u.hook_add(UC_HOOK_BLOCK, lambda uc, a, s, _: uc.emu_stop(), begin=self.wfi,
                   end=self.wfi)
        u.mem_write(STOP, b"\xfe\xe7")
        self.sp0, self.pc = struct.unpack_from("<2I", code, 0)
        u.reg_write(UC_ARM_REG_SP, self.sp0)
        self.pc |= 1
        self.sleeping = False
        self.retired = 0

    # ---- mailbox model ----
    def _rd(self, uc, off, size, _):
        return struct.unpack_from("<I", self.mb, off & 0xfc)[0]

    def _wr(self, uc, off, size, val, _):
        off &= 0xfc
        if off in (0x04, 0x2c):                                   # status: write 1 to clear
            v = struct.unpack_from("<I", self.mb, off)[0] & ~val
            struct.pack_into("<I", self.mb, off, v)
            return
        struct.pack_into("<I", self.mb, off, val)
        if 0x34 <= off <= 0x4c and (off - 0x34) % 8 == 0:         # B2A data: message sent
            ch = (off - 0x34) // 8
            cmd = struct.unpack_from("<I", self.mb, off - 4)[0]
            self.msgs.append((ch, cmd, val))
            uc.emu_stop()

    def a2b(self, ch, cmd, data):
        struct.pack_into("<I", self.mb, 0x08 + 8 * ch, cmd)
        struct.pack_into("<I", self.mb, 0x0c + 8 * ch, data)
        st = struct.unpack_from("<I", self.mb, 0x04)[0] | (1 << ch)
        struct.pack_into("<I", self.mb, 0x04, st)
        inten = struct.unpack_from("<I", self.mb, 0x00)[0]
        if not inten & (1 << ch):
            raise RuntimeError(f"A2B ch{ch} interrupt not enabled (inten {inten:#x})")
        self._irq(0x1a if ch == 1 else 0x1b)

    def _irq(self, irq):
        u = self.u
        vtor = struct.unpack("<I", u.mem_read(0xE000ED08, 4))[0]
        h = struct.unpack("<I", u.mem_read(vtor + 4 * irq, 4))[0]     # exception number
        ctx = [u.reg_read(r) for r in REGS]
        sp = ctx[13]
        if not self.sleeping:
            sp = ctx[13]                                          # running: PC from the stop
        u.reg_write(UC_ARM_REG_SP, (sp - 0x40) & ~7)               # room of an exception frame
        u.reg_write(UC_ARM_REG_LR, STOP | 1)
        u.emu_start(h | 1, STOP, count=2_000_000)
        for r, v in zip(REGS[:15], ctx[:15]):
            u.reg_write(r, v)
        # the flags too, as the hardware's exception return does: without
        # them, a stop between a compare and its conditional branch took the wrong way (a
        # rare, layout-dependent error in a decoder, re/newfw/ETUDE.md §29.1)
        u.reg_write(UC_ARM_REG_APSR, ctx[16] & 0xf80f0000)

    def run(self, budget=50_000_000):
        """Run the HiFi core until it sends a message or sleeps; return the new messages.

        Interrupts raised by the accelerator model (DMAC2 transfer done) are delivered
        between two runs, and the core resumes after its WFI if it was sleeping."""
        u = self.u
        while True:
            if self.sleeping:
                self.pc = self.wfi + 2 + 1                        # resume after the wfi
                self.sleeping = False
            u.emu_start(self.pc, STOP, count=budget)
            self.pc = u.reg_read(UC_ARM_REG_PC) | 1
            if (self.pc & ~1) == self.wfi:
                self.sleeping = True
            delivered = bool(self.irqs)
            while self.irqs:
                self._irq(self.irqs.pop(0))
            if self.msgs or not delivered:
                break
        m, self.msgs = self.msgs, []
        for ch, cmd, data in m:
            if self.trace:
                print(f"  B2A ch{ch} cmd {cmd:#x} data {data:#x}")
        return m

    def rd32(self, a, n=1):
        return struct.unpack(f"<{n}I", self.u.mem_read(a, 4 * n))


class Host:
    """The fake main core: file server and player."""

    def __init__(self, hifi, codec):
        self.h, self.codec, self.pos = hifi, codec, {}
        self.pending, self.deferred = [], []

    def serve(self, msgs):
        """Answer the file requests; return the control messages."""
        ctl = []
        for ch, cmd, data in msgs:
            if ch != 2:
                ctl.append((cmd, data))
                continue
            h = self.h
            if cmd == 0x69:                                       # seek {off, whence, h}
                off, wh = h.rd32(data, 2)
                hd = h.u.mem_read(data + 8, 1)[0]
                size = len(h.f)
                p = {0: off, 1: self.pos.get(hd, 0) + off, 2: size + off}[wh] & 0xffffffff
                ok = p <= size
                if ok:
                    self.pos[hd] = p
                    h.u.mem_write(data, struct.pack("<I", p))     # runtime reads the new position
                if h.trace:
                    print(f"  seek h{hd} {off:#x}/{wh} -> {p:#x}")
                self.pending.append((2, 0x6a, 0 if ok else 0xffffffff))
            elif cmd == 0x6b:                                     # read {buf, size, h}
                buf, n = h.rd32(data, 2)
                hd = h.u.mem_read(data + 8, 1)[0]
                p = self.pos.get(hd, 0)
                blob = h.f[p:p + n]
                self.pos[hd] = p + len(blob)

                def done(buf=buf, blob=blob, data=data, end=p + len(blob)):
                    h.u.mem_write(buf, blob)
                    h.u.mem_write(data + 4, struct.pack("<I", end))   # new position
                    self.pending.append((2, 0x6c, len(blob)))
                if h.trace:
                    print(f"  read h{hd} {buf:#x} {n:#x} at {p:#x} -> {len(blob):#x}")
                # Served at once (the stock modules expect it). EMU_LATENCY=1 delays the data
                # until the HiFi core next waits, to test a slow main core; with the stock FLAC
                # module that breaks decoding, so the runtime relies on the data being there.
                if os.environ.get("EMU_LATENCY"):
                    self.deferred.append(done)
                else:
                    done()
            elif cmd == 0x73:                                     # close (data = handle)
                if h.trace:
                    print(f"  close h{data}")
                self.pending.append((2, 0x74, 1))
            elif cmd in (0x66, 0x68, 0x75, 0xd3):                 # acknowledgements
                ctl.append((cmd, data))
            else:
                raise RuntimeError(f"unknown file request {cmd:#x} data {data:#x}")
        return ctl

    def step(self, until):
        """Run the HiFi core, serving files, until a control message in `until` arrives."""
        for _ in range(100000):
            while self.pending:
                self.h.a2b(*self.pending.pop(0))
            msgs = self.h.run(budget=2_000_000)
            ctl = self.serve(msgs)
            for cmd, data in ctl:
                if cmd in until:
                    return cmd, data
                raise RuntimeError(f"unexpected message {cmd:#x} data {data:#x}")
            if not msgs and self.deferred:                        # the HiFi core waits
                while self.deferred:
                    self.deferred.pop(0)()
                continue
            if self.h.sleeping and not self.pending and not self.deferred:
                raise RuntimeError("HiFi core asleep, no message")
        raise RuntimeError("no progress")


def main():
    a = sys.argv
    img, m, path, out = a[1], int(a[2]), a[3], a[4]
    codec = int(a[a.index("--codec") + 1]) if "--codec" in a else CODEC[m]
    secs = float(a[a.index("--seconds") + 1]) if "--seconds" in a else 0
    h = Hifi(img, m, path, "--trace" in a)
    host = Host(h, codec)
    print(f"module {m}: reset {h.pc & ~1:#x}, SP {h.sp0:#x}, wfi helper {h.wfi:#x}")
    host.step({1})
    print("ready (message 1)")
    # descriptor: u8 handle, u8 h2, u8 h3, pad | u32 size | u32 pos | u32 pos2 | u32 pos3 | u32 codec
    h.u.mem_write(DESC, struct.pack("<4B5I", 3, 4, 5, 0, len(h.f), 0, 0, 0, codec))
    host.pending.append((2, 0x65, DESC))
    host.step({0x66})
    print("descriptor taken (0x66)")
    h.a2b(1, 1, 0)
    cmd, info = host.step({2, 3})
    if cmd == 2:
        sys.exit("OPEN failed (message 2)")
    t, length, br, bps, rate, ch = h.rd32(info, 6)
    print(f"opened (message 3), info {info:#x}: {rate} Hz, {ch} ch, {bps} bit, {br} b/s, "
          f"length {length} ms")
    frames, bufs = 0, set()
    with open(out, "wb") as o:
        while True:                                               # message 3 carries a buffer too
            pcm, n = h.rd32(info + 0x1c, 2)
            o.write(bytes(h.u.mem_read(pcm, n * 4)))              # 16-bit stereo interleaved
            frames += n
            bufs.add(pcm)
            if secs and frames >= secs * rate:
                break
            h.a2b(1, 4, 0)
            cmd, info = host.step({5, 6})
            if cmd == 6:
                print("end of stream (message 6)")
                break
    t = h.rd32(info)[0]
    print(f"{frames} frames ({frames / max(rate, 1):.3f} s), time {t} ms, "
          f"PCM buffers {[hex(x) for x in sorted(bufs)]}")
    h.a2b(1, 0xd, 0)
    cmd, _ = host.step({0xe})
    print("closed (message 0xe)")


if __name__ == "__main__":
    main()
