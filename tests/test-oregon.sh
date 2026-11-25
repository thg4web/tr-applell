#!/usr/bin/env bash
# test-oregon.sh: play The Oregon Trail (your own copy, never part of the repo) from its title to a hunt past the Big Blue.
# Usage: tests/test-oregon.sh        (from the apple2 folder, after `make`; skipped when the WOZ is absent)
#
# Keys as a player types them: travel the trail, banker, five names, May, Matt's store (3 yoke, 800 pounds of
# food, 10 sets of clothes, 10 boxes of bullets, 2 of each spare part), leave, travel to the Kansas River,
# take the ferry, on to the Big Blue River, ford it, a broken wheel (answered once the prompt waits: the game
# throws away keys typed while it draws), the trail menu, and a short hunt (walk, aim, fire).
# The game's words go through COUT before its own hi-res printer draws them, so --cout-log shows what was on
# the screen. --stop-unpinned stops the run if the game calls BASIC anywhere but an Applesoft entry point.
# Run twice: from the WOZ, and from the SD card's files (build/sd/OREGON.PO and .BAD, the list of unreadable
# blocks) when they are there. Read-only: writes only under build/. Exit 0 only if every case passes.
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
OT=disks/the-oregon-trail-800-k.woz
[ -f "$OT" ] || { echo "skip Oregon Trail (no $OT)"; exit 0; }
python3 -I tools/applesoft-entries.py --write > /dev/null || exit 2   # build/entries.txt

. tests/oregon-keys.sh                       # w and KEYS: the play-through

pass=0; fail=0
play() {    # play NAME DISK
    local name=$1 disk=$2 log=build/oregon-$1.txt
    local out; out=$("$EMU" --headless --disk "$disk" --keys "$KEYS" --seconds 1 --stop-unpinned --cout-log "$log")
    local got=$?
    local text; text=$(tr '\r' '\n' < "$log")
    local ok=1
    [ "$got" = 0 ] || { ok=0; echo "    exit $got"; }
    for t in 'missing ROM routines: 0' 'unreadable blocks asked for: 8'; do
        grep -qF -- "$t" <<<"$out" || { ok=0; echo "    report lacks: $t"; }
    done
    grep -q 'STOPPED' <<<"$out" && { ok=0; grep 'STOPPED' <<<"$out" | sed 's/^/    /'; }
    for t in '1. Travel the trail' '1. Be a banker from Boston' 'Are these names correct?' \
             "Matt's General Store" 'Total bill:  $460.00' "Well then, you're ready to start." \
             'From Independence it is 102 miles to the Kansas River crossing.' \
             'You are now at the Kansas River crossing.' 'The ferry got your party and wagon safely across.' \
             'You are now at the Big Blue River crossing.' 'It was a muddy crossing, but you did not get stuck.' \
             'You were unable to repair the broken wagon wheel.' '8. Hunt for food' 'Hunting Instructions'; do
        grep -qF -- "$t" <<<"$text" || { ok=0; echo "    never on screen: $t"; }
    done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   Oregon Trail from $name: title, store, two rivers, a broken wheel, a hunt"
    else fail=$((fail+1)); echo "FAIL Oregon Trail from $name: title, store, two rivers, a broken wheel, a hunt"; grep -E '^(disk|cycles)' <<<"$out" | sed 's/^/    /'; fi
}

play woz "$OT"
if [ -f build/sd/OREGON.PO ] && [ -f build/sd/OREGON.BAD ]; then play sd-files build/sd/OREGON.PO
else echo "skip Oregon Trail from the SD files (make build/sd/OREGON.PO)"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
