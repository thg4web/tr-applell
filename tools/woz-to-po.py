#!/usr/bin/env python3
"""woz-to-po.py: turn a 3.5-inch (400K or 800K) WOZ 2 disk image into a ProDOS-order
block image (.po), and list the ProDOS files on it.

Usage:
  python3 -I woz-to-po.py IMAGE.woz                   dry run: decode, verify, list files
  python3 -I woz-to-po.py IMAGE.woz --write OUT.po    also write the block image
  python3 -I woz-to-po.py IMAGE.woz --write OUT.po --bad-list OUT.bad
                                                      a copy-protected disk: write the image with each
                                                      unreadable block zero-filled, and the list of those
                                                      blocks, so a block drive can answer them with an I/O
                                                      error as the real drive does (TR ][: block 8)
Options:
  --bad-list FILE   with --write: allow unreadable sectors; write their block numbers to FILE
                    (text: '#' comment lines, then one decimal block number per line). Without
                    --write it shows what the list would hold.
  --log FILE   append this run's report to FILE (dry run or write)
  --no-list    skip the ProDOS file list

Dry run by default: nothing is written unless --write is given. --write refuses to
overwrite an existing file, and refuses if any sector is missing or fails its checksum,
unless --bad-list is given (then the list records them). The WOZ is only ever read.
Copy protection is emulated, never patched: the image keeps every readable byte as is.

How it works (Apple 3.5-inch GCR, the same format as Macintosh 400K/800K disks):
  - Each track's bitstream is read twice round, so a sector that crosses the index
    point is still found. Disk bytes are framed by the high bit, as the drive does.
  - Address field: D5 AA 96, then track, sector, side, format, checksum (6-and-2 GCR).
  - Data field: D5 AA AD, sector, 699 disk bytes holding 524 bytes (12 tag bytes and
    512 data bytes) under a running three-byte checksum, then the 4 checksum bytes.
  - Speed zones: tracks 0-15 have 12 sectors, 16-31 have 11, ... 64-79 have 8.
  - Blocks are numbered track by track, side 0 then side 1, sector 0 up.
Only disk format and file-system metadata are read. File contents are never decoded
beyond the directory blocks needed for the list.
"""
import argparse
import os
import struct
import sys
from datetime import datetime

GCR62 = bytes.fromhex(
    "96979a9b9d9e9fa6a7abacadaeafb2b3"
    "b4b5b6b7b9babbbcbdbebfcbcdcecfd3"
    "d6d7d9dadbdcdddedfe5e6e7e9eaebec"
    "edeeeff2f3f4f5f6f7f9fafbfcfdfeff")
assert len(GCR62) == 64
DEC = {b: i for i, b in enumerate(GCR62)}

ADDR_MARK = b"\xd5\xaa\x96"
DATA_MARK = b"\xd5\xaa\xad"
DATA_NIBS = 699
FILE_TYPES = {0x00: "UNK", 0x01: "BAD", 0x04: "TXT", 0x06: "BIN", 0x0F: "DIR", 0x19: "ADB",
              0x1A: "AWP", 0x1B: "ASP", 0xB3: "S16", 0xEF: "PAS", 0xF0: "CMD", 0xFA: "INT",
              0xFB: "IVR", 0xFC: "BAS", 0xFD: "VAR", 0xFE: "REL", 0xFF: "SYS"}


def sectors_per_track(track):
    return 12 - track // 16


class Report:
    def __init__(self):
        self.lines = []

    def __call__(self, line=""):
        print(line)
        self.lines.append(line)


def read_woz(path):
    data = open(path, "rb").read()
    if data[:4] != b"WOZ2" or data[4:8] != b"\xff\x0a\x0d\x0a":
        sys.exit("not a WOZ 2 image (this tool reads WOZ 2 only)")
    chunks, pos = {}, 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4].decode("latin-1")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        chunks[cid] = (pos + 8, size)
        pos += 8 + size
    for need in ("INFO", "TMAP", "TRKS"):
        if need not in chunks:
            sys.exit(f"image has no {need} chunk")
    info_at = chunks["INFO"][0]
    disk_type, sides = data[info_at + 1], data[info_at + 37]
    if disk_type != 2:
        sys.exit("this is a 5.25-inch image; this tool decodes 3.5-inch disks only")
    tmap_at = chunks["TMAP"][0]
    tmap = data[tmap_at:tmap_at + 160]
    trks_at = chunks["TRKS"][0]
    tracks = []
    for i in range(160):
        start_block, block_count, bit_count = struct.unpack_from("<HHI", data, trks_at + i * 8)
        tracks.append((start_block * 512, block_count * 512, bit_count))
    return data, sides, tmap, tracks


def track_nibbles(data, entry):
    start, length, bit_count = entry
    raw = data[start:start + length]
    bits = "".join(f"{b:08b}" for b in raw)[:bit_count]
    out, reg = bytearray(), 0
    for c in bits + bits:          # twice round, for sectors that cross the index
        reg = (reg << 1) | (c == "1")
        if reg & 0x80:
            out.append(reg)
            reg = 0
    return bytes(out)


def decode_data(vals):
    """vals: 703 six-bit values (699 data + 4 checksum). Returns (524 bytes, checksum ok)."""
    b1, b2, b3, p = [], [], [], 0
    for i in range(175):
        w4, w1, w2 = vals[p], vals[p + 1], vals[p + 2]
        if i < 174:
            w3, p = vals[p + 3], p + 4
        else:
            w3, p = 0, p + 3
        b1.append((w1 & 0x3F) | ((w4 << 2) & 0xC0))
        b2.append((w2 & 0x3F) | ((w4 << 4) & 0xC0))
        b3.append((w3 & 0x3F) | ((w4 << 6) & 0xC0))
    c1 = c2 = c3 = 0
    out, n = bytearray(), 0
    while True:
        c1 = (c1 & 0xFF) << 1
        if c1 & 0x100:
            c1 += 1
        val = (b1[n] ^ c1) & 0xFF
        c3 += val
        if c1 & 0x100:
            c3 += 1
            c1 &= 0xFF
        out.append(val)
        val = (b2[n] ^ c3) & 0xFF
        c2 += val
        if c3 > 0xFF:
            c2 += 1
            c3 &= 0xFF
        out.append(val)
        if len(out) == 524:
            break
        val = (b3[n] ^ c2) & 0xFF
        c1 += val
        if c2 > 0xFF:
            c1 += 1
            c2 &= 0xFF
        out.append(val)
        n += 1
    c4 = ((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2)
    g4, g3, g2, g1 = vals[p:p + 4]
    ok = (g4, g3, g2, g1) == (c4 & 0x3F, c3 & 0x3F, c2 & 0x3F, c1 & 0x3F)
    return bytes(out), ok


def decode_track(nib, want_track, want_side):
    """Returns {sector: 512 bytes} for good sectors, and a list of problems."""
    good, problems = {}, []
    i = nib.find(ADDR_MARK)
    while i != -1:
        fields = [DEC.get(b) for b in nib[i + 3:i + 8]]
        if None not in fields and len(fields) == 5:
            trk_lo, sector, side_b, fmt, csum = fields
            if csum == (trk_lo ^ sector ^ side_b ^ fmt):
                track = trk_lo | ((side_b & 0x1F) << 6)
                side = (side_b >> 5) & 1
                j = nib.find(DATA_MARK, i + 8, i + 8 + 100)
                if (track, side) != (want_track, want_side):
                    problems.append(f"address field says track {track} side {side}")
                elif j != -1 and sector not in good:
                    vals = [DEC.get(b) for b in nib[j + 4:j + 4 + DATA_NIBS + 4]]
                    if DEC.get(nib[j + 3]) != sector:
                        problems.append(f"sector {sector}: data field has the wrong sector number")
                    elif None in vals or len(vals) != DATA_NIBS + 4:
                        problems.append(f"sector {sector}: bad disk byte in data field")
                    else:
                        payload, ok = decode_data(vals)
                        if ok:
                            good[sector] = payload[12:]
                        else:
                            problems.append(f"sector {sector}: data checksum mismatch")
        i = nib.find(ADDR_MARK, i + 1)
    return good, problems


def prodos_date(b, o):
    d, t = b[o] | b[o + 1] << 8, b[o + 2] | b[o + 3] << 8
    if d == 0:
        return "<no date>"
    year, month, day = d >> 9, (d >> 5) & 0x0F, d & 0x1F
    year += 2000 if year < 40 else 1900
    try:
        return datetime(year, month, day, (t >> 8) & 0x1F, t & 0x3F).strftime("%d-%b-%y %H:%M")
    except ValueError:
        return f"<bad date {d:04x}>"


def prodos_list(blocks, nblocks, say):
    def blk(n):
        return blocks[n * 512:(n + 1) * 512]

    def word(b, o):
        return b[o] | b[o + 1] << 8

    head = blk(2)
    if head[4] >> 4 != 0xF:
        say("Block 2 is not a ProDOS volume directory: this disk is not ProDOS, or the")
        say("block order is wrong. No file list.")
        return []
    vol = head[5:5 + (head[4] & 0x0F)].decode("ascii", "replace")
    say(f"ProDOS volume /{vol}  ({word(head, 4 + 0x25)} blocks, "
        f"{word(head, 4 + 0x21)} entries in the volume directory)")
    say(f"  {'name':<34}{'type':<6}{'aux':<7}{'blocks':>7}{'bytes':>9}  modified")
    files = []

    def walk(key, prefix, seen):
        n, first = key, True
        while n and n < nblocks and n not in seen:
            seen.add(n)
            b = blk(n)
            for e in range(13):
                off = 4 + e * 39
                if first and e == 0:
                    continue                       # directory header
                st, nl = b[off] >> 4, b[off] & 0x0F
                if st == 0:
                    continue                       # empty or deleted
                name = prefix + b[off + 1:off + 1 + nl].decode("ascii", "replace")
                ftype = b[off + 0x10]
                tname = FILE_TYPES.get(ftype, f"${ftype:02X}")
                used = word(b, off + 0x13)
                eof = b[off + 0x15] | b[off + 0x16] << 8 | b[off + 0x17] << 16
                aux = word(b, off + 0x1F)
                say(f"  {name:<34}{tname:<6}${aux:04X} {used:>7}{eof:>9}  {prodos_date(b, off + 0x21)}")
                files.append((name, tname))
                if st == 0xD:
                    walk(word(b, off + 0x11), name + "/", seen)
            first = False
            n = word(b, 2)

    walk(2, "", set())
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--write", metavar="OUT.po")
    ap.add_argument("--log", metavar="FILE")
    ap.add_argument("--no-list", action="store_true")
    ap.add_argument("--bad-list", metavar="FILE")
    args = ap.parse_args()
    if args.write and os.path.exists(args.write):
        sys.exit(f"{args.write} already exists; not overwriting it")
    if args.bad_list and args.write and os.path.exists(args.bad_list):
        sys.exit(f"{args.bad_list} already exists; not overwriting it")

    say = Report()
    say(f"woz-to-po {datetime.now():%Y-%m-%d %H:%M}  {'WRITE ' + args.write if args.write else 'dry run'}")
    say(f"image {args.image}")
    data, sides, tmap, tracks = read_woz(args.image)
    nblocks = 0
    for t in range(80):
        nblocks += sectors_per_track(t) * sides
    blocks = bytearray(nblocks * 512)
    missing, problems, block, bad_blocks = [], [], 0, []
    for t in range(80):
        spt = sectors_per_track(t)
        for s in range(sides):
            idx = tmap[t * 2 + s]
            good, probs = ({}, ["track not in image"]) if idx == 0xFF else \
                decode_track(track_nibbles(data, tracks[idx]), t, s)
            for sec in range(spt):
                if sec in good:
                    blocks[(block + sec) * 512:(block + sec + 1) * 512] = good[sec]
                else:
                    missing.append((t, s, sec))
                    bad_blocks.append(block + sec)
            problems += [f"track {t} side {s}: {p}" for p in sorted(set(probs))
                         if not any(f"sector {sec}:" in p for sec in good)]
            block += spt
    found = nblocks - len(missing)
    say(f"sectors   {found}/{nblocks} decoded with good checksums ({sides} side(s), {nblocks // 2}K)")
    if missing:
        say(f"missing   {len(missing)}: " + ", ".join(f"T{t}S{s}#{c}" for t, s, c in missing[:20])
            + (" ..." if len(missing) > 20 else ""))
    for p in problems[:20]:
        say(f"  note: {p}")
    if len(problems) > 20:
        say(f"  ... and {len(problems) - 20} more notes")

    if not args.no_list:
        say()
        files = prodos_list(blocks, nblocks, say)
        if files:
            names = {n.upper() for n, _ in files}
            bas = [n for n, t in files if t == "BAS"]
            say()
            say(f"BASIC.SYSTEM on the disk: {'yes' if any(n.endswith('BASIC.SYSTEM') for n in names) else 'no'}")
            say(f"Applesoft (BAS) files:    {len(bas)}" + (f" ({', '.join(bas[:8])}{' ...' if len(bas) > 8 else ''})" if bas else ""))
            say(f"System (SYS) files:       {', '.join(n for n, t in files if t == 'SYS') or 'none'}")

    bad_text = ("# unreadable blocks of " + os.path.basename(args.image) + ", found by woz-to-po.py\n"
                "# a block drive answers each with an I/O error, as the real drive does\n"
                + "".join(f"{b}\n" for b in bad_blocks))
    if args.bad_list:
        say(f"unreadable blocks: {', '.join(map(str, bad_blocks)) or 'none'}")
    if args.write:
        if missing and not args.bad_list:
            say(f"NOT written: {len(missing)} sector(s) missing or bad (--bad-list writes them as a list)")
        else:
            with open(args.write, "xb") as f:
                f.write(blocks)
            say(f"wrote {args.write} ({len(blocks)} bytes)")
            if args.bad_list:
                with open(args.bad_list, "x") as f:
                    f.write(bad_text)
                say(f"wrote {args.bad_list} ({len(bad_blocks)} block(s))")
    elif args.bad_list:
        say("dry run: the list would read:")
        for line in bad_text.splitlines():
            say("  " + line)
    if args.log:
        with open(args.log, "a") as f:
            f.write("\n".join(say.lines) + "\n\n")
    sys.exit(1 if missing and not args.bad_list else 0)


if __name__ == "__main__":
    main()
