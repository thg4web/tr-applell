#!/usr/bin/env bash
# test-disk.sh: the disk image layer (emu/diskimg.c, drop-and-play step D1).
# Usage: tests/test-disk.sh        (from the dev folder, after `make`)
#  1. build/test-disk: synthetic disks in every format, then two DOS 3.3 system disks of your own (read-only,
#     from $DOS33_DIR, default disks/dos33; skipped if absent; never copied anywhere): VTOC and catalog through the layer.
#  2. Its catalog listing must equal tools/dos33-files.py's, line for line (two separate readers).
#  3. a2term boots the block card's test disk as .po, as .dsk (DOS order) and as .2mg: the same screen.
# Writes only under build/ (the test disks made from build/bootdisk.po). Exit 0 only if every case passes.
cd "$(dirname "$0")/.." || exit 2
pass=0; fail=0
check() { local name=$1 out=$2 ok=1; shift 2
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    lacks: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"; else fail=$((fail+1)); echo "FAIL $name"; fi; }

DOS33=${DOS33_DIR:-disks/dos33}
out=$(./build/test-disk "$DOS33/1980_dos33c.dsk" "$DOS33/1983_dos33c.dsk"); st=$?
grep -E '^(ok|FAIL|skip)' <<<"$out"
[ $st = 0 ] && { pass=$((pass+1)); echo "ok   test-disk: every check"; } || { fail=$((fail+1)); echo "FAIL test-disk"; }
for d in 1980_dos33c.dsk 1983_dos33c.dsk; do
    [ -f "$DOS33/$d" ] || { echo "skip $d (absent)"; continue; }
    ours=$(sed -n "/^--- catalog $d\$/,/^--- end\$/p" <<<"$out" | sed '1d;$d')
    theirs=$(python3 -I tools/dos33-files.py "$DOS33/$d")
    if [ -n "$ours" ] && [ "$ours" = "$theirs" ]; then pass=$((pass+1)); echo "ok   $d: the layer's catalog equals dos33-files.py's ($(grep -c bytes <<<"$ours") files, names, types, sizes)"
    else fail=$((fail+1)); echo "FAIL $d: catalogs differ"; diff <(echo "$ours") <(echo "$theirs") | head -10; fi
done

# The block card's test disk (ours, build/bootdisk.po: 3 blocks) as a 140K disk in each format.
python3 -I - build/bootdisk.po build <<'PY'
import struct, sys
src, out = sys.argv[1], sys.argv[2]
po = bytearray(open(src, 'rb').read()); po += bytes(143360 - len(po))           # ProDOS order, padded to 140K
skew = [0, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 15]                     # DOS sector -> physical
pphys = lambda i: 15 if i == 15 else (2 * i) % 15                               # ProDOS-order sector -> physical
dos = bytearray(143360)
for t in range(35):
    for s in range(16):
        i = next(i for i in range(16) if pphys(i) == skew[s])
        dos[(t * 16 + s) * 256:(t * 16 + s + 1) * 256] = po[(t * 16 + i) * 256:(t * 16 + i + 1) * 256]
hdr = bytearray(64); hdr[0:8] = b'2IMGTEST'; hdr[8] = 64; hdr[10] = 1
struct.pack_into('<IIIII', hdr, 12, 0, 0, 0, 64, 143360)                       # format 0 (DOS order), data at 64
for name, data in (('bootdisk-140k.po', po), ('bootdisk-140k.dsk', dos), ('bootdisk-140k.2mg', hdr + dos)):
    open(f'{out}/{name}', 'wb').write(data)
PY
for f in build/bootdisk-140k.po build/bootdisk-140k.dsk build/bootdisk-140k.2mg; do
    out=$(./build/a2term --headless --disk "$f" --seconds 1)
    check "a2term boots the test disk from ${f#build/}" "$out" 'BOOT OK FROM BLOCK 0' 'BLOCK 1 VIA PRODOS DRIVER' 'BLOCK 2 VIA SMARTPORT' 'disk: 5.25-inch'
done
out=$(./build/a2term --headless --disk build/bootdisk-140k.dsk --seconds 1 | sed -n 's/^disk: //p')
check "a2term names the format" "$out" '5.25-inch, DOS order, 280 blocks'

echo "$pass passed, $fail failed"
[ $fail = 0 ]
