"""Core B for tools/emu_fw.py: a second Unicorn running the HiFi module that our firmware loaded,
sharing HIGHRAM0, HIGHRAM1 and the mailbox with core A.

Built on the HiFi model of re/newfw/tools/emu_host.py (its exception delivery, WFI handling and
HIFIACC/DMAC2 model); here the module comes from the memory core A wrote, and the mailbox is a
register file both cores see:
  A2B  +0x00 int enable, +0x04 status (write 1 to clear), +0x08 + 8*ch cmd, +0x0c + 8*ch data
  B2A  +0x28 int enable, +0x2c status (write 1 to clear), +0x30 + 8*ch cmd, +0x34 + 8*ch data
A data write sends the message: it sets the receiver's status bit (and for A2B, raises the
core B interrupt 0x1a for channel 1, 0x1b for channel 2).
"""
import ctypes
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK, UC_PROT_ALL
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools")))
import emu_host                                                   # noqa: E402

STOP = 0x0001FFF0


class Shared:
    """HIGHRAM0/1 buffers and the mailbox registers, seen by both cores"""

    def __init__(self):
        self.hr0 = ctypes.create_string_buffer(0x20000)
        self.hr1 = ctypes.create_string_buffer(0x40000)
        self.sram = ctypes.create_string_buffer(0xA0000)          # core A SRAM (descriptor)
        self.mb = bytearray(0x100)
        self.b = None                                             # CoreB once started

    def get(self, off):
        return struct.unpack_from("<I", self.mb, off)[0]

    def put(self, off, v):
        struct.pack_into("<I", self.mb, off, v & 0xffffffff)

    def write(self, off, val, from_a):
        if off in (0x04, 0x2c):                                   # status: write 1 to clear
            self.put(off, self.get(off) & ~val)
            return None
        self.put(off, val)
        if from_a and 0x0c <= off <= 0x24 and (off - 0x0c) % 8 == 0:
            ch = (off - 0x0c) // 8
            self.put(0x04, self.get(0x04) | 1 << ch)
            return ch
        if not from_a and 0x34 <= off <= 0x4c and (off - 0x34) % 8 == 0:
            ch = (off - 0x34) // 8
            self.put(0x2c, self.get(0x2c) | 1 << ch)
        return None


class CoreB(emu_host.Hifi):
    def __init__(self, sh, trace=False):                          # super().__init__ not called
        self.sh, self.trace = sh, trace
        self.mb = sh.mb
        self.msgs = []
        u = self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        u.mem_map_ptr(0, 0x20000, UC_PROT_ALL, sh.hr0)
        u.mem_map_ptr(0x01000000, 0x20000, UC_PROT_ALL, sh.hr0)
        u.mem_map_ptr(0x01020000, 0x40000, UC_PROT_ALL, sh.hr1)
        self.acc = emu_host.Accel(u, self)
        def mmio(base, size, rd, wr):                               # counted with EMU_COUNT=1
            r, w = emu_host.counted("B", base, rd, wr)
            u.mmio_map(base, size, r, None, w, None)
        mmio(0x01060000, 0x20000, self.acc.rd, self.acc.wr)
        self.irqs = []
        u.mem_map_ptr(0x03000000, 0xA0000, UC_PROT_ALL, sh.sram)  # core A SRAM, shared
        u.mem_map(0x40000000, 0x110000)
        mmio(0x40110000, 0x1000, self._rd, self._wr)
        u.mem_map(0x40111000, 0x400000 - 0x111000)
        u.mem_map(0x60000000, 0x50000)                              # DMAC1, IMDCT36, SDMMC, ...: RAM
        self.syn = emu_host.Synth()                                 # SYNTH model
        mmio(0x60050000, 0x2000, self.syn.rd, self.syn.wr)
        u.mem_map(0x60052000, 0x100000 - 0x52000)
        u.mem_map(0xE0000000, 0x100000)
        code = bytes(sh.hr0)
        self.wfi = code.find(bytes.fromhex("30bf7047"))
        if self.wfi < 0:
            raise RuntimeError("core B: no wfi helper in the loaded module")
        u.hook_add(UC_HOOK_BLOCK, lambda uc, a, s, _: uc.emu_stop(), begin=self.wfi, end=self.wfi)
        u.mem_write(STOP, b"\xfe\xe7")
        self.sp0, self.pc = struct.unpack_from("<2I", code, 0)
        u.reg_write(UC_ARM_REG_SP, self.sp0)
        self.pc |= 1
        self.sleeping = False
        self.retired = 0
        # the delay helper waits on SysTick with the frequency of the parameter block; there is
        # no SysTick here, so it gets 0 (no delay), as in emu_host.py
        struct.pack_into("<2I", sh.hr1, 0x20, 0, 0)

    def _rd(self, uc, off, size, _):
        return self.sh.get(off & 0xfc)

    def _wr(self, uc, off, size, val, _):
        off &= 0xfc
        self.sh.write(off, val, False)
        if 0x34 <= off <= 0x4c and (off - 0x34) % 8 == 0:
            ch = (off - 0x34) // 8
            self.msgs.append((ch, self.sh.get(off - 4), val))
            if self.trace:
                print(f"  B2A ch{ch} cmd {self.sh.get(off - 4):#x} data {val:#x}")
            uc.emu_stop()

    def deliver(self, ch):
        """A2B message on channel ch: the interrupt, if enabled, then let the core run"""
        if self.sh.get(0x00) & 1 << ch:
            self._irq(0x1a if ch == 1 else 0x1b)
        self.step()

    def watch(self, lo, hi):
        """EMU_WATCH: the last writes into [lo, hi), shown when core B faults"""
        import collections
        from unicorn import UC_HOOK_MEM_WRITE
        self.wlog = collections.deque(maxlen=40)
        def cb(uc, access, addr, size, value, _):
            self.wlog.append((uc.reg_read(UC_ARM_REG_PC), addr, size, value))
        self.u.hook_add(UC_HOOK_MEM_WRITE, cb, begin=lo, end=hi - 1)

    def step(self, budget=2_000_000):
        try:
            self.run(budget=budget)
        except Exception as ex:                                   # noqa: BLE001
            for pc, a, n, v in getattr(self, "wlog", []):
                print(f"  write pc {pc:#x} addr {a:#x} size {n} value {v:#x}")
            from unicorn import arm_const as ac
            rr = {n: self.u.reg_read(getattr(ac, "UC_ARM_REG_" + n.upper())) for n in
                  ("r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sp", "lr")}
            print(f"core B: {ex} at pc {self.u.reg_read(UC_ARM_REG_PC):#x} " +
                  " ".join(f"{k} {v:#x}" for k, v in rr.items()))
            raise
        self.msgs.clear()
