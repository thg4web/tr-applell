#!/usr/bin/env python3
"""woz-info.py: print the INFO and META chunks of a WOZ 1/2 disk image (read-only).

Usage: python3 -I woz-info.py IMAGE.woz

Reads only the header metadata. Never decodes or prints track data.
"""
import struct
import sys
import zlib

HW = ["Apple ][", "Apple ][ Plus", "Apple //e (unenhanced)", "Apple //c",
      "Apple //e Enhanced", "Apple IIgs", "Apple //c Plus", "Apple ///", "Apple /// Plus"]


def main(path):
    data = open(path, "rb").read()
    magic = data[:4]
    if magic not in (b"WOZ1", b"WOZ2") or data[4:8] != b"\xff\x0a\x0d\x0a":
        sys.exit(f"not a WOZ image: {magic!r}")
    crc = struct.unpack_from("<I", data, 8)[0]
    print(f"format     {magic.decode()}  crc {'ok' if crc in (0, zlib.crc32(data[12:])) else 'MISMATCH'}")
    pos, chunks = 12, []
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4].decode("latin-1"), struct.unpack_from("<I", data, pos + 4)[0]
        chunks.append((cid, size))
        body = data[pos + 8:pos + 8 + size]
        if cid == "INFO":
            ver, dtype, wp, sync, clean = body[0:5]
            print(f"info ver   {ver}")
            print(f"disk type  {'5.25-inch' if dtype == 1 else '3.5-inch' if dtype == 2 else dtype}")
            print(f"write prot {bool(wp)}  synchronized {bool(sync)}  cleaned {bool(clean)}")
            print(f"creator    {body[5:37].decode('utf-8', 'replace').strip()}")
            if ver >= 2:
                sides, bootfmt, timing = body[37:40]
                hw, ram, largest = struct.unpack_from("<HHH", body, 40)
                print(f"sides      {sides}  boot sector format {bootfmt}  bit timing {timing * 125} ns")
                print(f"hardware   {', '.join(n for i, n in enumerate(HW) if hw >> i & 1) or 'unknown'}")
                print(f"min RAM    {ram}K" if ram else "min RAM    unknown")
                print(f"largest    {largest} blocks")
        elif cid == "META":
            print("meta:")
            for line in body.decode("utf-8", "replace").splitlines():
                if line.strip():
                    k, _, v = line.partition("\t")
                    print(f"  {k:<16} {v}")
        pos += 8 + size
    print("chunks     " + ", ".join(f"{c}({s})" for c, s in chunks))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
