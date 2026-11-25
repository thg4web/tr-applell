#!/usr/bin/env python3
"""applesoft-roundtrip.py: B2's proof that our BASIC reads and writes Applesoft's tokens and line format.

Usage:
  python3 -I tests/applesoft-roundtrip.py [PROGRAM.bas ...]      (from the apple2 folder, after `make`)
  With no arguments it uses every disks/applesoft/*/*.bas (Apple's programs from the DOS 3.3 disks).

For each program: load its saved bytes the way DOS's LOAD does (a2term --bas), LIST it, NEW, type the
listing back in, and compare the program in memory line by line with the original. Read-only: writes only
under build/roundtrip/. Exit 0 only if every program round-trips.

Allowed difference, documented: Applesoft lists a REM with an extra space after it "no matter how many spaces
were typed" (Applesoft II manual, REM), so a listed-and-retyped REM gains one space; DATA, also stored as typed
(SLOT# on Apple's 1983 disk keeps "DATA 32,..."), gains one the same way. Lines longer than 239 characters once
listed cannot be typed back (the input line holds 239), nor can line numbers above 63999 (the manual: "a line
number is an integer from 0 to 63999"; PHONE LIST has 65533-65535, made in memory); both are reported, not hidden.
"""
import glob
import os
import subprocess
import sys

EMU = "./build/a2term"
OUT = "build/roundtrip"


def lines_of(mem, start=0x0801):
    """[(number, body bytes)] from program bytes laid out from `start`, following the bytes, not the links."""
    out, p = [], 0
    while p + 1 < len(mem) and (mem[p] | mem[p + 1] << 8):
        q = p + 4
        while mem[q]:
            q += 1
        out.append((mem[p + 2] | mem[p + 3] << 8, bytes(mem[p + 4:q])))
        p = q + 1
    return out


def run(args):
    return subprocess.run([EMU, "--headless"] + args, capture_output=True, text=True, errors="replace").stdout


def key_escape(text):
    out = []
    for ch in text:
        o = ord(ch)
        out.append(ch if 32 <= o < 127 and ch != "\\" else f"\\x{o:02X}")
    return "".join(out)


def one(path):
    name = os.path.basename(os.path.dirname(path)) + "/" + os.path.basename(path)
    orig = open(path, "rb").read()
    want = lines_of(orig)
    log = os.path.join(OUT, name.replace("/", "-") + ".list.txt")
    run(["--bas", path, "--cout-log", log, "--keys", "D000G\\r\\w\\bLIST\\r", "--seconds", str(2 + len(want) // 6)])   # LIST takes time
    text = open(log, "rb").read().decode("latin-1").split("\r")
    try:
        first = next(i for i, t in enumerate(text) if t.endswith("]LIST"))
    except StopIteration:
        return name, "FAIL", "no LIST output"
    listed = []
    for t in text[first + 1:]:
        if t.startswith("]"):
            break
        listed.append(t)
    listed = [t for t in listed if t.strip()]
    if len(listed) != len(want):
        return name, "FAIL", f"LIST gave {len(listed)} lines, the program has {len(want)}"
    long = [t.split(" ")[0] for t in listed if len(t) > 239]
    keys = "D000G\\r\\wNEW\\r" + "".join(key_escape(t) + "\\r" for t in listed if len(t) <= 239)
    end = 0x0801 + len(orig) + 64 * len(want)
    out = run(["--keys", keys, "--seconds", "1", "--dump", f"0801-{min(end, 0xBFFF):04X}"])
    mem = bytearray()
    for row in out.splitlines():
        if len(row) > 5 and row[4] == "-" and all(c in "0123456789ABCDEF" for c in row[:4]):
            mem += bytes(int(x, 16) for x in row[5:].split())
    got = dict(lines_of(mem))
    exact = rem = 0
    bad = []
    high = [num for num, _ in want if num > 63999]
    for num, body in want:
        if any(t.split(" ")[0] == str(num) for t in listed if len(t) > 239) or num > 63999:
            continue
        g = got.get(num)
        if g == body:
            exact += 1
            continue
        k = next((i for i, b in enumerate(body) if b in (0xB2, 0x83)), None)          # REM or DATA token
        if g is not None and k is not None and g[:k + 1] == body[:k + 1] and g[k + 1:k + 2] == b" " and g[k + 2:] == body[k + 1:]:
            rem += 1
            continue
        bad.append((num, body, g))
    note = f"{exact} exact" + (f", {rem} REM/DATA +1 space (documented)" if rem else "") + (f", {len(long)} too long to retype: {' '.join(long)}" if long else "") + (f", {len(high)} above 63999 cannot be typed: {' '.join(map(str, high))}" if high else "")
    if bad:
        num, body, g = bad[0]
        return name, "FAIL", f"{note}; {len(bad)} differ, first line {num}: want {body.hex(' ')} got {g.hex(' ') if g else 'nothing'}"
    return name, "ok", note


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    os.makedirs(OUT, exist_ok=True)
    files = sys.argv[1:] or sorted(glob.glob("disks/applesoft/*/*.bas"))
    if not files:
        print("skip the Applesoft round trip (no disks/applesoft/*/*.bas: take the programs off your own DOS 3.3 disk"
              " with tools/dos33-files.py --type A --write-dir)")
        return 0
    fails = 0
    for f in files:
        name, status, note = one(f)
        fails += status != "ok"
        print(f"{'ok  ' if status == 'ok' else 'FAIL'} {name}: {note}")
    print(f"{len(files) - fails} round-tripped, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
