#!/usr/bin/env python3
"""Run one function of a firmware image (stock or RECHO) and log its peripheral accesses.

  emu_mmio.py FW.IMG FUNC [--ovl N,...] [--args r0,r1,r2,r3] [--stub ADDR[=RET],...]
              [--read ADDR=VAL,...] [--reads] [--max N]

The resident code (overlay 0) and the overlays named by --ovl are loaded at their exec
addresses (code and data). Every access to the peripheral windows (0x40000000-0x40200000,
0x50000000-0x50040000, 0x60000000-0x600c0000, 0x01060000-0x01080000) goes through an MMIO
mapping: writes are logged with the block name from the TRM address map and remembered;
reads return the last value written, or the value given by --read, and are logged with
--reads. A read polled more than 2000 times in a row (no write in between) stops the run and is reported, which shows what
the code waits for. --stub replaces a function by "return RET" (delays, log printers); a
stubbed log printer whose r0 points to a string has it printed.
Only block hooks plus MMIO mappings are used (Unicorn 2.1 mis-executes Thumb IT blocks with
code or memory-write hooks).
"""
import re
import struct
import sys
from collections import Counter
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

sys.path.insert(0, __file__.rsplit("/", 3)[0])
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from rknanofw import overlays                                     # noqa: E402
from periph_map import PERIPH                                     # noqa: E402

WINDOWS = [(0x40000000, 0x200000), (0x50000000, 0x40000), (0x60000000, 0xC0000),
           (0x01060000, 0x20000)]
STOP = 0x030A0000 - 0x10


def name(a):
    for base in sorted(PERIPH, reverse=True):
        size = 0x40000 if PERIPH[base] == "USB" else 0x10000
        if base <= a < base + size:
            return f"{PERIPH[base]}+{a - base:#x}"
    return f"{a:#010x}"


def opt(flag, default=None):
    a = sys.argv
    return a[a.index(flag) + 1] if flag in a else default


def main():
    d = open(sys.argv[1], "rb").read()
    fn = int(sys.argv[2], 16)
    c, B, E = overlays(d)
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x01000000, 0x60000)
    u.mem_map(0x03000000, 0xA0000)
    u.mem_map(0xE0000000, 0x100000)
    ovls = [0] + [int(x) for x in opt("--ovl", "").split(",") if x]
    for i in ovls:
        cl, ce, cs, dl, de, ds, be, bs = E[i]
        u.mem_write(ce, c[cl - B:cl - B + cs])
        if ds:
            u.mem_write(de, c[dl - B:dl - B + ds])
    mem, polls, log = {}, Counter(), []
    fixed = {int(k, 16): int(v, 16) for k, v in
             (x.split("=") for x in opt("--read", "").split(",") if x)}
    show_reads = "--reads" in sys.argv

    def mk(base):
        def rd(uc, off, size, _):
            a = base + off
            polls[a] += 1
            if polls[a] > 2000:
                log.append(f"  POLL {name(a)} stuck (last value {mem.get(a, 0):#x})")
                uc.emu_stop()
            v = fixed.get(a, mem.get(a, 0))
            if show_reads and polls[a] <= 3:
                log.append(f"  R {name(a)} -> {v:#x}")
            return v

        def wr(uc, off, size, val, _):
            polls.clear()                       # a write means progress: polls count again
            a = base + off
            mem[a] = val
            log.append(f"  W {name(a)} = {val:#x}")
        return rd, wr

    for base, size in WINDOWS:
        rd, wr = mk(base)
        u.mmio_map(base, size, rd, None, wr, None)
    strs = {}
    for cl, ce, cs, *_ in (E[i] for i in ovls):
        blob = c[cl - B:cl - B + cs]
        for m in re.finditer(rb"[\x20-\x7e]{4,}", blob):
            strs[ce + m.start()] = m.group().decode()
    for item in [x for x in opt("--stub", "").split(",") if x]:
        a, _, ret = item.partition("=")
        a, ret = int(a, 16), int(ret or "0", 0)
        u.mem_write(a, struct.pack("<HH", 0x2000 | (ret & 0xff), 0x4770))   # movs r0,#ret; bx lr

        def hk(uc, addr, size, _):
            r0, r1 = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
            if r0 in strs:
                log.append(f"  [{strs[r0]}] r1={r1:#x}")
            else:
                log.append(f"  call {addr:#x}({r0:#x}, {r1:#x})")
        u.hook_add(UC_HOOK_BLOCK, hk, begin=a, end=a)
    u.mem_write(STOP, b"\xfe\xe7")
    args = [int(x, 0) for x in opt("--args", "").split(",") if x]
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        u.reg_write(r, v)
    u.reg_write(UC_ARM_REG_SP, 0x03004000 - 0x200)
    u.reg_write(UC_ARM_REG_LR, STOP | 1)
    try:
        u.emu_start(fn | 1, STOP, count=int(opt("--max", "5000000")))
    except Exception as e:                                        # noqa: BLE001
        log.append(f"  EXCEPTION {e} at pc {u.reg_read(UC_ARM_REG_PC):#x}")
    print("\n".join(log))
    print(f"-- end at pc {u.reg_read(UC_ARM_REG_PC):#x}, {len(log)} events")


if __name__ == "__main__":
    main()
