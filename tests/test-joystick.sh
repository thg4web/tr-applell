#!/usr/bin/env bash
# test-joystick.sh: the Apple's joystick in the emulator (plan step J1): paddles timed so the Monitor's
# PREAD ($FB1E) counts exactly the value set, buttons on bit 7 of $C061-$C063, and BASIC's PDL(.
# Usage: tests/test-joystick.sh        (from the dev folder, after `make`)
# The stick is set with a2term's --paddle N=V and --button N, and moved mid-run with the --keys escapes
# \pNHH (paddle N to hex HH), \dN and \uN (button N down and up). J2: \sHH is the C64's stick as the
# client sends it, through emu/stick.c as in the module (paddles, button 0, Oregon Trail's hunt keys).
# Read-only: nothing outside build/.
# Exit 0 only if every case passes (exit status 0 from the emulator also means no missing ROM routine ran).
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
pass=0; fail=0
check() {    # check NAME OUTPUT TEXT...: pass when OUTPUT holds every TEXT
    local name=$1 out=$2 ok=1; shift 2
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    lacks: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"; else fail=$((fail+1)); echo "FAIL $name"; echo "$out" | sed 's/^/    /'; fi
}
run() { "$EMU" --headless "$@"; echo "exit $?"; }
answers() {  # the line after each ]PRINT on the screen, trimmed, as "a,b,c,", then the emulator's exit status
    sed -n '/^|\]PRINT/{n;p;}' | tr -d '|' | sed 's/ *$//' | tr '\n' ','
}
got() { echo "got $(answers <<<"$1") $(tail -1 <<<"$1")"; }
W='\w'                               # wait one Apple second: the line before has run when the stick moves
GO='\x02\r'                          # Ctrl-B, Return: BASIC's cold start

got=""; ex=""
for v in 0 127 200 255; do o=$(run --paddle 0=$v --keys "${GO}PRINT PDL(0)\r"); got+=$(answers <<<"$o"); ex+=$(tail -1 <<<"$o")" "; done
check "PDL(0) at 0, 127, 200, 255 gives exactly those" "got $got $ex" "got 0,127,200,255, exit 0 exit 0 exit 0 exit 0"

out=$(run --paddle 0=10 --paddle 1=250 --keys "${GO}PRINT PDL(0);\" \";PDL(1);\" \";PDL(0)\r")
check "PDL(1) is its own paddle: 10 and 250, and PDL(0) again after it" "$(got "$out")" 'got 10 250 10, exit 0'

out=$(run --keys "${GO}PRINT PDL(0)\r$W\\p0C8PRINT PDL(0)\r$W\\p032PRINT PDL(0)\r")
check "the stick moves mid-run: 0, then 200, then 50" "$(got "$out")" 'got 0,200,50, exit 0'

out=$(run --keys "${GO}PRINT PEEK(-16287)\r$W\\d0PRINT PEEK(-16287)\r$W\\u0PRINT PEEK(-16287)\r")
check "button 0 at PEEK(-16287): up 0, down 128, up 0" "$(got "$out")" 'got 0,128,0, exit 0'

out=$(run --button 1 --keys "${GO}PRINT PEEK(-16286)\rPRINT PEEK(-16287)\rPRINT PEEK(-16285)\r")
check "button 1 at PEEK(-16286), held from the start; buttons 0 and 2 up" "$(got "$out")" 'got 128,0,0, exit 0'

# LDX #1, JSR PREAD, BRK: the Monitor shows Y
out=$(run --paddle 1=200 --keys '300:A2 01 20 1E FB 00\r300G\r')
check "PREAD typed into the Monitor: paddle 1 at 200 gives Y=C8" "$out" 'Y=C8' 'exit 0'
out=$(run --paddle 3=255 --keys '300:A2 03 20 1E FB 00\r300G\r')
check "PREAD on paddle 3 at 255 gives Y=FF" "$out" 'Y=FF' 'exit 0'

# the same loop as Apple's own PREAD (LDY #0, NOP, NOP before it: a cycle later than ours) counts exactly too
APL='300:A2 00 20 10 03 00\r310:AD 70 C0 A0 00 EA EA BD 64 C0 10 04 C8 D0 F8 88 60\r300G\r'
got_y=""; for v in 0 1 127 254 255; do got_y+=$(run --paddle 0=$v --keys "$APL" | grep -o 'Y=[0-9A-F][0-9A-F]' | tail -1)" "; done
check "a PREAD laid out as Apple's (first read at 10 cycles, not 9) gives 0, 1, 127, 254, 255" "got $got_y" "got Y=00 Y=01 Y=7F Y=FE Y=FF "

# every value, 0-255, through BASIC (what BASIC prints goes to a COUT log; the screen shows only 24 lines)
K=$GO; for v in $(seq 0 255); do K+=$(printf '\\p0%02XPRINT PDL(0)\\r\\w' "$v"); done
out=$(run --keys "$K" --cout-log build/joystick-sweep.txt)
sweep=$(python3 -I - build/joystick-sweep.txt <<'PY2'
import sys
lines = bytes(b & 0x7F for b in open(sys.argv[1], 'rb').read()).decode().split('\r')
vals = [int(l) for l in lines if l.strip().isdigit()]
print("sweep ok" if vals == list(range(256)) else f"sweep bad: {len(vals)} values, first wrong at {next((i for i, v in enumerate(vals) if v != i), None)}")
PY2
)
check "every paddle value 0-255 comes back from PDL(0) exactly" "$sweep $(tail -1 <<<"$out")" 'sweep ok exit 0'

# a digital stick: each axis is 0, 127 or 255
S='PRINT PDL(0);" ";PDL(1)\r'
out=$(run --keys "${GO}\\p000\\p1FF$S$W\\p07F\\p17F$S$W\\p0FF\\p100$S")
check "a digital stick's three positions on both axes: left-down, centre, right-up" "$(got "$out")" 'got 0 255,127 127,255 0, exit 0'

out=$(run --paddle 0=99 --keys "${GO}PRINT PDL(256)\r")
check "PDL(256) is an illegal quantity, as in Applesoft" "$out" '?ILLEGAL QUANTITY ERROR' 'exit 0'

out=$(run --keys 'C061\rC064\rC070\r')
check "the game I/O counters still count (S1)" "$out" '$C061 button 0 x1' '$C064 paddle 0 x1' '$C070 trigger x1'

# J2: the C64 stick through emu/stick.c, the module's own mapping
check "the C64 stick on the Apple: up-left reads 0,0 by PREAD's count; down-right with fire reaches the button" \
    "$(run --keys '\s28300:A2 00 20 1E FB 84 06 A2 01 20 1E FB 00\r300G\r' | grep -o 'Y=[0-9A-F][0-9A-F]' | tail -1) \
$(run --keys '\sD0300:A2 00 20 1E FB 84 06 A2 01 20 1E FB AD 61 C0 00\r300G\r' | grep -o 'A=[0-9A-F][0-9A-F] X=01 Y=[0-9A-F][0-9A-F]' | tail -1)" \
    'Y=00 A=80 X=01 Y=FF'
out=$(run --keys '\s48\s88\s00PRINT\r')
check "outside a hunt the stick types nothing (the Monitor sees only what was typed)" "$out" '|*PRINT' 'exit 0'

OT=disks/the-oregon-trail-800-k.woz
if [ -f "$OT" ]; then
    . tests/oregon-keys.sh
    TAIL_KEYS="P$(w 1) $(w 2)O$(w 1) $(w 2)K$(w 1) $(w 3)"                 # the play-through's hunt, as keys
    TAIL_STICK="\\s48$(w 1)\\sC8$(w 2)\\s08$(w 1)\\s88$(w 2)\\s20$(w 1)\\sA0$(w 3)"   # the same with the stick
    HEAD=${KEYS%"$TAIL_KEYS"}
    if [ "$HEAD$TAIL_KEYS" = "$KEYS" ]; then
        "$EMU" --headless --disk "$OT" --keys "$KEYS" --seconds 1 > build/hunt-keys.txt
        "$EMU" --headless --disk "$OT" --keys "$HEAD$TAIL_STICK" --seconds 1 --png build/hunt-stick.png > build/hunt-stick.txt
        same=$(cmp -s <(grep -v 'saved:' build/hunt-keys.txt) <(grep -v 'saved:' build/hunt-stick.txt) && echo same || echo differ)
        check "Oregon Trail's hunt played with the stick (aim up-right, fire, up, fire, left, fire) ends exactly as with the keys" \
            "$same $(grep -c . build/hunt-stick.txt)" 'same '
        NAME=${KEYS%%OSCAR*}
        a=$("$EMU" --headless --disk "$OT" --keys "$NAME" --seconds 1 | sed -n '2,25p')
        b=$("$EMU" --headless --disk "$OT" --keys "$NAME\\s08\\s48\\s80\\s00" --seconds 1 | sed -n '2,25p')
        check "at the name prompt the stick types nothing (the screen is the same with and without it)" \
            "$([ "$a" = "$b" ] && echo same || echo differ)" 'same'
    else echo "    oregon-keys.sh no longer ends with the hunt this test expects"; fail=$((fail+1)); fi
else echo "skip Oregon Trail stick hunt (no $OT)"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
