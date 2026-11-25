#!/usr/bin/env python3
"""pin-entry.py: put a pinned entry point (a 3-byte JMP at a fixed Applesoft address) into rom/basic.s.

Usage (from project/work/apple2, after `make`):
  python3 -I tools/pin-entry.py ADDR TARGET NOTE            dry run: show where it would go
  python3 -I tools/pin-entry.py ADDR TARGET NOTE --write    insert it, rebuild, check

ADDR is hex (D412), TARGET our routine's label (ERROR), NOTE the [RM] source text for the comment.

How: the listing (build/basic.lst) gives every instruction's address. The pin goes before the instruction
with the highest address X where X + 3 <= ADDR (room for PINF's jump over the entry), skipping spots right
after Microsoft's BIT-skip bytes (.byte $2C / $24 / $A9, which must stay next to the instruction they skip)
and data lines. --write archives rom/basic.s to rom/.archived/ first (a new name each time, nothing is
overwritten), inserts "PINF $ADDR, TARGET", runs make, and if the build fails (a branch out of range, a later
pin overrun) puts the file back and tries the next earlier spot, up to 8 spots. Never deletes anything.
"""
import os
import re
import shutil
import subprocess
import sys
import time

LST, SRC = "build/basic.lst", "rom/basic.s"
INSN = re.compile(r"^00([0-9A-F]{4})  1  ((?:[0-9A-F]{2} )+)\s*(\s{8}[a-z]{3}\b.*)$")


def candidates(addr):
    out, prev_src = [], ""
    for line in open(LST, encoding="latin-1"):
        m = INSN.match(line.rstrip("\n"))
        if m:
            a, src = int(m.group(1), 16), m.group(3)
            skip = re.search(r"\.byte \$(2C|24|A9)\b", prev_src)
            if a + 3 <= addr and not skip:
                out.append((a, src))
        text = line[24:].rstrip("\n") if len(line) > 24 else ""
        if text.strip():
            prev_src = text
    out.sort(key=lambda t: -t[0])
    return [c for c in out if c[0] >= addr - 48]


def build():
    r = subprocess.run(["make", "-B", "build/tr-rom.bin"], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)[-600:]


def main():
    args = [a for a in sys.argv[1:] if a != "--write"]
    write = "--write" in sys.argv
    if len(args) != 3:
        print(__doc__.split("\n\n")[1]); return 2
    addr, target, note = int(args[0], 16), args[1], args[2]
    rom = open("build/tr-rom.bin", "rb").read()
    if rom[addr - 0xD000] == 0x4C:
        print(f"${addr:04X} already holds a JMP: nothing to do"); return 0
    cands = candidates(addr)
    if not cands:
        print(f"no instruction boundary in the 48 bytes below ${addr:04X}"); return 1
    pin = f"        PINF ${addr:04X}, {target:<14}; [OURS] [RM] ${addr:04X}: {note}"
    if not write:
        for a, src in cands[:3]:
            print(f"would go before ${a:04X}: {src.strip()[:90]}")
        return 0
    stamp = time.strftime("%Y-%m-%d-%H%M%S")
    shutil.copy2(SRC, f"rom/.archived/basic-{stamp}-before-pin-{addr:04X}.s")
    original = open(SRC).read()
    lines = original.split("\n")
    for a, src in cands[:8]:
        hits = [i for i, l in enumerate(lines) if l.rstrip() == src.rstrip()]
        if len(hits) != 1:
            continue
        new = lines[:hits[0]] + [pin] + lines[hits[0]:]
        open(SRC, "w").write("\n".join(new))
        ok, log = build()
        if ok:
            got = open("build/tr-rom.bin", "rb").read()[addr - 0xD000:addr - 0xD000 + 3]
            print(f"pinned ${addr:04X} -> {target} before ${a:04X}: {src.strip()[:70]}  (bytes {got.hex(' ')})")
            return 0
        open(SRC, "w").write(original)
        print(f"  not before ${a:04X}: {log.strip().splitlines()[-1][:120] if log.strip() else 'build failed'}")
    open(SRC, "w").write(original)
    build()
    print(f"could not pin ${addr:04X}"); return 1


if __name__ == "__main__":
    sys.exit(main())
