#!/usr/bin/env python3
"""Run a stock panel-driver function (overlay 4) under Unicorn and log what it sends.

  emu_lcd.py FW.IMG FUNC [FUNC...]        (hex exec addresses in overlay 4)

Logged: VOP_MCU_CMD (+0x28) and VOP_MCU_DATA (+0x2c) writes of the VOP at 0x60070000, the
other VOP registers, the GPIO helpers' arguments (iomux 0x03052d70, direction 0x03056a94,
level 0x03056ace) and the delays (0x03065cac ms, 0x03065cd0, 0x030575c8 us), which return at
once. Reads of the VOP status register return 0 (idle). Only block hooks plus an MMIO
mapping are used (Unicorn 2.1 mis-executes Thumb IT blocks with code or memory-write hooks).
"""
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
                               UC_ARM_REG_LR)

sys.path.insert(0, __file__.rsplit("/", 3)[0])
from rknanofw import overlays                                     # noqa: E402

VOP, STOP = 0x60070000, 0x030A0000 - 0x10
NAMES = {0x00: "CON", 0x08: "TIMING", 0x0c: "SIZE", 0x10: "+10", 0x14: "SRT", 0x18: "+18",
         0x1c: "INT_CLR", 0x20: "INT_ST", 0x24: "STATUS", 0x28: "CMD", 0x2c: "DATA",
         0x30: "START"}
HOOKS = {0x03052d70: "iomux", 0x03056a94: "gpio_dir", 0x03056ace: "gpio_set",
         0x03065cac: "delay_ms", 0x03065cd0: "delay2", 0x030575c8: "delay_us",
         0x03061da8: "wdt_kick"}
RETURNS = {0x03065cac, 0x03065cd0, 0x030575c8, 0x03061da8}


def main():
    d = open(sys.argv[1], "rb").read()
    c, B, E = overlays(d)
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x03000000, 0xA0000)
    for i in (0, 4):
        cl, ce, cs, dl, de, ds, be, bs = E[i]
        u.mem_write(ce, c[cl - B:cl - B + cs])
        if ds:
            u.mem_write(de, c[dl - B:dl - B + ds])
    u.mem_map(0x40000000, 0x200000)
    u.mem_map(0x50000000, 0x100000)
    u.mem_map(0x60000000, VOP - 0x60000000)
    log = []

    def rd(uc, off, size, _):
        return 0

    def wr(uc, off, size, val, _):
        nm = NAMES.get(off, f"+{off:x}")
        if log and log[-1][0] == nm and nm == "DATA":
            log[-1][1].append(val)
        else:
            log.append((nm, [val]))

    u.mmio_map(VOP, 0x1000, rd, None, wr, None)
    u.mem_map(VOP + 0x1000, 0x10000000 - (VOP + 0x1000 - 0x60000000))
    u.mem_map(0xE0000000, 0x100000)
    for a in RETURNS:
        u.mem_write(a, b"\x70\x47")                               # bx lr

    def hk(uc, addr, size, _):
        nm = HOOKS[addr]
        r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        log.append((nm, r))

    for a in HOOKS:
        u.hook_add(UC_HOOK_BLOCK, hk, begin=a, end=a)
    u.mem_write(STOP, b"\xfe\xe7")
    for f in sys.argv[2:]:
        fn = int(f, 16)
        log.clear()
        u.reg_write(UC_ARM_REG_SP, 0x03004000 - 0x100)
        u.reg_write(UC_ARM_REG_LR, STOP | 1)
        u.reg_write(UC_ARM_REG_R0, 0)
        u.emu_start(fn | 1, STOP, count=2_000_000)
        print(f"== {fn:#x}: {len(log)} events")
        for nm, v in log:
            if nm == "CMD":
                print(f"  cmd  {v[0]:02x}")
            elif nm == "DATA":
                print("  data " + " ".join(f"{x:02x}" for x in v))
            elif nm in ("delay_ms", "delay2", "delay_us"):
                print(f"  {nm}({v[0]})")
            elif nm in ("iomux", "gpio_dir", "gpio_set"):
                print(f"  {nm}({v[0]}, {v[1]}, {v[2]})")
            elif nm == "wdt_kick":
                continue
            else:
                print(f"  VOP {nm} = {v[0]:#010x}")


if __name__ == "__main__":
    main()
