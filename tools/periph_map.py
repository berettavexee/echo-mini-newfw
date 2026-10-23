#!/usr/bin/env python3
"""Which system-core functions touch which peripheral, in a stock RKnano image.

  periph_map.py FW.IMG [--ovl N,...] [--detail PERIPH]

Scans the resident code and every system overlay (exec address >= 0x03000000) for
peripheral addresses loaded by pc-relative ldr or built by movw/movt, attributes each site
to the enclosing function (nearest preceding push {.., lr}) and prints, per peripheral block
(RKNanoD TRM fig. 2-1), the functions and register offsets used.
"""
import struct, sys, collections
import capstone

PERIPH = {
    0x01060000: "HIFIACC", 0x01070000: "DMAC2",
    0x40020000: "I2S0", 0x40030000: "I2S1", 0x40040000: "I2C0", 0x40050000: "I2C1",
    0x40060000: "I2C2", 0x40070000: "SPI0", 0x40080000: "SPI1", 0x40090000: "ACODEC",
    0x400A0000: "UART0", 0x400B0000: "UART1", 0x400C0000: "UART2", 0x400D0000: "SARADC",
    0x400E0000: "TIMER", 0x400F0000: "PWM0", 0x40100000: "PWM1", 0x40110000: "MAILBOX",
    0x40120000: "WDT", 0x40130000: "UART3", 0x40140000: "UART4", 0x40150000: "UART5",
    0x40160000: "GPIO0", 0x40170000: "GPIO1", 0x40180000: "CRU",
    0x50010000: "GRF", 0x50020000: "PMU", 0x50030000: "GPIO2",
    0x60000000: "DMAC1", 0x60010000: "IMDCT", 0x60020000: "SDMMC", 0x60030000: "EMMC",
    0x60040000: "EBC", 0x60050000: "SYNTH", 0x60060000: "SFC", 0x60070000: "LCDC",
    0x60080000: "USB", 0xE000E000: "SCS",
}


def periph(v):
    base = v & 0xFFFF0000 if v < 0x60080000 or v >= 0x600C0000 else 0x60080000
    if v >= 0xE000E000 and v < 0xE000F000:
        base = 0xE000E000
    return (PERIPH[base], v - base) if base in PERIPH else None


def main():
    img = sys.argv[1]
    detail = sys.argv[sys.argv.index("--detail") + 1] if "--detail" in sys.argv else None
    only = None
    if "--ovl" in sys.argv:
        only = {int(x) for x in sys.argv[sys.argv.index("--ovl") + 1].split(",")}
    d = open(img, "rb").read()
    clen = struct.unpack_from("<I", d, 0x70)[0]
    c = d[0x200:0x200 + clen]
    B, n = struct.unpack_from("<2I", c, 0)
    ents = [struct.unpack_from("<8I", c, 8 + 32 * i) for i in range(n)]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    md.skipdata = True
    use = collections.defaultdict(lambda: collections.defaultdict(set))   # p -> (ovl,fn) -> offs
    for i, (cl, ce, cs, *_ ) in enumerate(ents):
        if not cs or ce < 0x03000000 or (only and i not in only):
            continue
        code = c[cl - B:cl - B + cs]
        fn, movw = ce, {}
        for x in md.disasm(code, ce):
            if x.mnemonic == "push" and "lr" in x.op_str:
                fn = x.address
            v = None
            if x.mnemonic.startswith("ldr") and "[pc, #" in x.op_str:
                a = ((x.address + 4) & ~3) + int(x.op_str.split("#")[-1].rstrip("]"), 0)
                if ce <= a < ce + cs - 3:
                    v = struct.unpack_from("<I", code, a - ce)[0]
            elif x.mnemonic in ("movw", "mov.w") and "#" in x.op_str:
                r = x.op_str.split(",")[0]
                movw[r] = int(x.op_str.split("#")[-1], 0) & 0xFFFF
            elif x.mnemonic == "movt" and "#" in x.op_str:
                r = x.op_str.split(",")[0]
                v = (int(x.op_str.split("#")[-1], 0) << 16) | movw.get(r, 0)
            if v is not None:
                p = periph(v)
                if p:
                    use[p[0]][(i, fn)].add(p[1])
    for p in sorted(use, key=lambda k: [b for b, nm in PERIPH.items() if nm == k][0]):
        fns = use[p]
        ovls = sorted({o for o, _ in fns})
        print(f"{p:8s} {len(fns):3d} functions, overlays {ovls}")
        if detail in (p, "all"):
            for (o, f), offs in sorted(fns.items()):
                print(f"    ovl{o:02d} {f:#010x}  " + " ".join(f"+{x:#x}" for x in sorted(offs)))


if __name__ == "__main__":
    main()
