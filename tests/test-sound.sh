#!/usr/bin/env bash
# test-sound.sh: the speaker path on the Mac (sound plan step S1): a2term's sound report, its WAV and the
# game I/O counts (joystick, paddles, buttons: $C060-$C07F).
# Usage: tests/test-sound.sh        (from the apple2 folder, after `make`)
# The Oregon Trail case plays the same keys as test-oregon.sh (skipped when the WOZ is absent) and leaves
# build/oregon-sound.txt (the report) and build/oregon-sound.wav (to listen to). Writes only under build/.
# Exit 0 only if every case passes.
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
pass=0; fail=0
check() {    # check NAME OUTPUT TEXT...: pass when OUTPUT holds every TEXT
    local name=$1 out=$2 ok=1; shift 2
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    lacks: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"; else fail=$((fail+1)); echo "FAIL $name"; fi
}

out=$("$EMU" --headless --seconds 1 --sound-report)
check "power-on beep: one 0.1 s tone at 934.5 Hz (BELL1 flips every 546 cycles)" "$out" \
    'speaker clicks 192' 'sound: 192 clicks in 1 events' 'steady tones: 192 clicks (100.0%)' '934.5-934.5 Hz'

out=$("$EMU" --headless --keys '\x07' --seconds 1 --sound-report --wav build/test-bell.wav)
check "Ctrl-G at the Monitor prompt: a second beep, the same note" "$out" \
    'speaker clicks 384' 'steady tones: 384 clicks (100.0%)' '1 note 934.5-934.5 Hz' 'speaker saved: build/test-bell.wav'
cycles=$(sed -n 's/^cycles \([0-9]*\) .*/\1/p' <<<"$out")
wav=$(python3 -I - build/test-bell.wav "$cycles" <<'PY'
import array, sys, wave
w = wave.open(sys.argv[1]); a = array.array('h', w.readframes(w.getnframes()))
want = int(int(sys.argv[2]) / (1020484.4 / 22050))
print("wav ok" if (w.getnchannels(), w.getframerate(), w.getsampwidth()) == (1, 22050, 2)
      and abs(w.getnframes() - want) <= 1 and max(a) > 8000 and min(a) < -8000 else
      f"wav bad: {w.getnchannels()} ch, {w.getframerate()} Hz, {w.getsampwidth()} bytes, {w.getnframes()} frames (want {want}), {min(a)}..{max(a)}")
PY
)
check "the WAV: mono, 22050 Hz, 16-bit, as long as the run, not silent" "$wav" 'wav ok'

out=$("$EMU" --headless --seconds 1)
check "no game I/O read at the Monitor prompt" "$out" 'game I/O ($C060-$C07F, joystick and paddles): none'
out=$("$EMU" --headless --keys 'C061\rC064\rC070\r' --seconds 1)
check "game I/O counted: button 0, paddle 0, the trigger" "$out" '$C061 button 0 x1' '$C064 paddle 0 x1' '$C070 trigger x1'

OT=disks/the-oregon-trail-800-k.woz
if [ -f "$OT" ]; then
    . tests/oregon-keys.sh
    "$EMU" --headless --disk "$OT" --keys "$KEYS" --seconds 1 --sound-report --wav build/oregon-sound.wav > build/oregon-sound.txt
    out=$(cat build/oregon-sound.txt)
    check "Oregon Trail play-through: 5 tunes, all steady notes, no joystick read" "$out" \
        'speaker clicks 37228' 'sound: 37228 clicks in 5 events' 'irregular: 0 clicks' \
        'game I/O ($C060-$C07F, joystick and paddles): none' 'speaker saved: build/oregon-sound.wav'
    sed -n '/^sound: /,/^  sparse/p' build/oregon-sound.txt | sed 's/^/    /'
else echo "skip Oregon Trail sound (no $OT)"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
