#!/usr/bin/env bash
# test-monitor.sh: type Monitor commands into the headless emulator and check the screen.
# Usage: tests/test-monitor.sh        (from the apple2 folder, after `make`)
# Read-only: nothing outside build/ is touched. Exit 0 only if every case passes.
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
pass=0; fail=0

# check NAME KEYS EXPECTED_EXIT TEXT...   every TEXT must appear on the final screen/report
# (set DISK=file to boot with a disk in slot 5)
check() {
    local name=$1 keys=$2 want_exit=$3; shift 3
    local args=(--headless --keys "$keys" --seconds ${SECONDS_RUN:-1})
    [ -n "$DISK" ] && args+=(--disk "$DISK")
    local out; out=$("$EMU" "${args[@]}"); local got_exit=$?
    local ok=1
    [ "$got_exit" = "$want_exit" ] || ok=0
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    missing: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"
    else fail=$((fail+1)); echo "FAIL $name (exit $got_exit, wanted $want_exit)"; echo "$out" | sed 's/^/    /'; fi
}

PROG='300:A9 C1 20 ED FD 00\r'     # LDA #'A' / JSR COUT / BRK

check "boot: banner and prompt"   ''                       0 'TR ][ MONITOR 0.3' '|*'
check "store, then show a range"  "${PROG}300.305\r"       0 '0300- A9 C1 20 ED FD 00'
check "show one byte"             "${PROG}302\r"           0 '0302- 20'
check "two addresses in a row"    "${PROG}300 302\r"       0 '0300- A9' '0302- 20'
check "Return alone carries on"   "${PROG}300\r\r"         0 '0300- A9' '0301- C1 20 ED FD 00 00 00'
check "colon alone carries on"    '300:11\r:22 33\r300.302\r' 0 '0300- 11 22 33'
check "range across a line"       '306:66 77 88\r306.308\r' 0 '0306- 66 77' '0308- 88'
check "G runs code, BRK shows registers" "${PROG}300G\r" 0 '|A ' '|0305- A=C1 X=00 Y='
check "left arrow edits the line" "${PROG}3XX\x08\x0802\r" 0 '0302- 20'
check "Ctrl-X cancels the line"   '12\x18300\r'            0 '|*12\ ' '|*300 ' '0300- 00'
check "unknown command beeps"     'Z\r'                    0 '|*Z' 'speaker clicks 384'
check "scrolling keeps the bottom line" '300.3BF\r'        0 '03B8- 00 00 00 00 00 00 00 00'
# $FECD is the cassette WRITE fence ($E000 was used here until BASIC filled $D000 up)
check "missing ROM routine is logged" 'FECDG\r'            1 'STOPPED: missing ROM routine at $FECD'
check "hex A-F digits"            'ABCD:EF\rABCD\r'       0 'ABCD- EF'

DISK=build/bootdisk.po check "boots a disk card: ProDOS and SmartPort reads" '' 0 \
    'BOOT OK FROM BLOCK 0' 'BLOCK 1 VIA PRODOS DRIVER' 'BLOCK 2 VIA SMARTPORT' \
    'disk calls: 3 reads (0 failed)' 'SmartPort: STATUS 0, READ 1' 'blocks read, in order: 0-2'

# Oregon Trail (your own copy, never part of the repo): only when the image is present.
OT=disks/the-oregon-trail-800-k.woz
if [ -f "$OT" ]; then
    # B2 put Applesoft's JMP at $E000: the game takes our BASIC, loads its menu and shows it on hi-res
    # page 2 (tests/test-oregon.sh plays on from there)
    SECONDS_RUN=20 DISK=$OT check "Oregon Trail boots to its menu (hi-res page 2)" '' 0 \
        'display: HI-RES, page 2' 'unreadable blocks asked for: 8' 'missing ROM routines: 0'
else
    echo "skip Oregon Trail (no $OT)"
fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
