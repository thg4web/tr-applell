#!/usr/bin/env python3
"""dos33-files.py: list the files on an Apple DOS 3.3 disk image (.dsk, DOS sector order)
and copy files out of it.

Usage:
  python3 -I dos33-files.py IMAGE.dsk                          dry run: check the VTOC, list files
  python3 -I dos33-files.py IMAGE.dsk --extract NAME --write OUT   copy one file out
  python3 -I dos33-files.py IMAGE.dsk --type A --write-dir DIR     copy every file of a type out
Options:
  --type T     only files of type T (T I A B S R); with --write-dir, the type to copy
  --keep-header  keep DOS's own header (A: 2-byte length; B: address and length)

Dry run by default: nothing is written unless --write or --write-dir is given. Both refuse to
overwrite an existing file. The image is opened read-only.

What comes out: an Applesoft (A) file is its program bytes as Applesoft keeps them in memory
from $0801 (line links, line numbers, tokens), without DOS's 2-byte length in front. A binary
(B) file is its bytes without the address and length. Other types come out whole, sector by
sector. --write-dir names files after the disk name in kebab case with .bas / .bin / .txt.

DOS 3.3 layout used: VTOC at track 17 sector 0 (catalog track and sector at bytes 1-2, release
3 at byte 3, 35 tracks and 16 sectors at $34-$35); catalog sectors hold 7 entries of 35 bytes
from $0B (track/sector list, type, 30-character name in high-bit ASCII, length in sectors);
a track/sector list holds the data sectors as pairs from $0C, chained by bytes 1-2.
"""
import argparse
import os
import re
import sys

TYPES = {0x00: "T", 0x01: "I", 0x02: "A", 0x04: "B", 0x08: "S", 0x10: "R", 0x20: "A2", 0x40: "B2"}
EXT = {"A": ".bas", "B": ".bin", "T": ".txt", "I": ".int"}


class DiskError(Exception):
    pass


class Disk:
    def __init__(self, data):
        if len(data) != 35 * 16 * 256:
            raise DiskError(f"image is {len(data)} bytes, not a 140K 35-track disk")
        self.data = data
        v = self.sector(17, 0)
        if v[3] != 3 or v[0x34] != 35 or v[0x35] != 16:
            raise DiskError("track 17 sector 0 is not a DOS 3.3 VTOC (release, tracks or sectors wrong);"
                            " the image may be in ProDOS order")
        self.volume = v[6]
        self.cat = (v[1], v[2])

    def sector(self, t, s):
        if not (0 <= t < 35 and 0 <= s < 16):
            raise DiskError(f"track {t} sector {s} is off the disk")
        o = (t * 16 + s) * 256
        return self.data[o:o + 256]

    def files(self):
        out, (t, s), seen = [], self.cat, set()
        while t:
            if (t, s) in seen:
                raise DiskError("the catalog chain loops")
            seen.add((t, s))
            c = self.sector(t, s)
            for e in range(7):
                ent = c[0x0B + e * 35:0x0B + (e + 1) * 35]
                if ent[0] == 0 or ent[0] == 0xFF:          # never used, or deleted
                    continue
                name = bytes(b & 0x7F for b in ent[3:33]).decode("ascii").rstrip()
                out.append({"name": name, "type": TYPES.get(ent[2] & 0x7F, "?"), "locked": bool(ent[2] & 0x80),
                            "ts": (ent[0], ent[1]), "sectors": ent[33] | ent[34] << 8})
            t, s = c[1], c[2]
        return out

    def read(self, f):
        out, (t, s), seen = bytearray(), f["ts"], set()
        while t:
            if (t, s) in seen:
                raise DiskError(f"{f['name']}: track/sector list loops")
            seen.add((t, s))
            tsl = self.sector(t, s)
            for k in range(0x0C, 256, 2):
                if tsl[k] == 0 and tsl[k + 1] == 0:
                    return bytes(out)
                out += self.sector(tsl[k], tsl[k + 1])
            t, s = tsl[1], tsl[2]
        return bytes(out)

    def contents(self, f, keep_header):
        raw = self.read(f)
        if keep_header:
            return raw
        if f["type"] == "A":
            n = raw[0] | raw[1] << 8
            return raw[2:2 + n]
        if f["type"] == "B":
            n = raw[2] | raw[3] << 8
            return raw[4:4 + n]
        return raw


def kebab(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "file"


def write_new(path, data):
    if os.path.exists(path):
        raise DiskError(f"{path} exists; not overwriting")
    with open(path, "xb") as out:
        out.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--extract", metavar="NAME")
    ap.add_argument("--write", metavar="OUT")
    ap.add_argument("--write-dir", metavar="DIR")
    ap.add_argument("--type")
    ap.add_argument("--keep-header", action="store_true")
    a = ap.parse_args()
    try:
        with open(a.image, "rb") as fh:
            disk = Disk(fh.read())
        files = [f for f in disk.files() if not a.type or f["type"] == a.type.upper()]
        if a.extract:
            match = [f for f in files if f["name"] == a.extract.upper()]
            if not match:
                raise DiskError(f"no file named {a.extract!r}")
            data = disk.contents(match[0], a.keep_header)
            print(f"{match[0]['type']} {match[0]['name']}: {len(data)} bytes")
            if a.write:
                write_new(a.write, data)
                print(f"wrote {a.write}")
            else:
                print("dry run: nothing written (add --write OUT)")
            return 0
        print(f"volume {disk.volume}, {len(files)} file(s)")
        for f in files:
            size = len(disk.contents(f, False))
            print(f"{'*' if f['locked'] else ' '}{f['type']:<2} {f['sectors']:03d} {size:6d} bytes  {f['name']}")
        if a.write_dir:
            if not a.type:
                raise DiskError("--write-dir needs --type")
            os.makedirs(a.write_dir, exist_ok=True)
            for f in files:
                path = os.path.join(a.write_dir, kebab(f["name"]) + EXT.get(f["type"], ".dat"))
                write_new(path, disk.contents(f, a.keep_header))
                print(f"wrote {path}")
        return 0
    except DiskError as e:
        print(f"dos33-files: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
