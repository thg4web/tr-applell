#!/usr/bin/env bash
# test-tone.sh: the tone tracker (sound plan step S2, emu/tone.c) and the SID voice WAV (mac/sid-voice.c).
# Usage: tests/test-tone.sh        (from the dev folder, after `make`)
# The expected SID values are worked out here with bc from the click timing, not copied from a run.
# Three small programs typed into the Monitor make noise, a 50 Hz tone and an uneven square wave.
# The Oregon Trail case (skipped when the WOZ is absent) checks the tracker against S1's report and
# leaves build/oregon-sound.wav (the Apple's speaker), build/oregon-sid.wav (what SID voice 1 would play,
# sample for sample beside it) and build/oregon-tone.txt (the changes: S3's packets). Writes only under build/.
# Exit 0 only if every case passes.
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
pass=0; fail=0
check() {    # check NAME OUTPUT TEXT...: pass when OUTPUT holds every TEXT
    local name=$1 out=$2 ok=1; shift 2
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    lacks: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"; else fail=$((fail+1)); echo "FAIL $name"; fi
}
fn() { echo "(2^25*910/(912*$1)+1)/2" | bc; }          # SID Fn for a full period of $1 Apple cycles (NTSC), rounded
pw() { echo "($1*4096*2/($1+$2)+1)/2" | bc; }           # pulse width for halves $1 (the shorter) and $2

# --- the bell: BELL1 flips every 546 cycles, so the full period is 1092 ---
BELL=$(fn 1092)
out=$("$EMU" --headless --seconds 1 --sound-report --tone-log build/tone-bell.txt --wav build/test-bell.wav --sid-wav build/tone-bell-sid.wav)
log=$(grep -v '^#' build/tone-bell.txt)
check "power-on beep: Fn $BELL (bc, 934.5 Hz), 7 steady frames, 6 frames on, then the gate goes off" "$out$log" \
    "kinds: steady 7, split 0 (0 carry the old note on), slow 0, irregular 0, sparse 0, silent 52" \
    "voice 1 on: 6 frames (pulse 6, noise 0); 1 note played, Fn $BELL-$BELL (934.5-934.5 Hz)" \
    "packets (state changes): 2" \
    "on  pulse   $BELL  2048     934.5  steady" "off  pulse   $BELL  2048     934.5  steady"
wav=$(python3 -I - build/test-bell.wav build/tone-bell-sid.wav <<'PY'
import array, sys, wave
w = [wave.open(p) for p in sys.argv[1:]]
a = array.array('h', w[1].readframes(w[1].getnframes()))
same = all((x.getnchannels(), x.getframerate(), x.getsampwidth(), x.getnframes()) ==
           (w[0].getnchannels(), w[0].getframerate(), w[0].getsampwidth(), w[0].getnframes()) for x in w)
print("sid wav ok" if same and max(a) > 8000 and min(a) < -8000 else f"sid wav bad: same={same} {min(a)}..{max(a)}")
PY
)
check "the SID WAV: the same format and length as the speaker's, not silent" "$wav" 'sid wav ok'

out=$("$EMU" --headless --keys '\x07' --seconds 1 --sound-report --tone-log build/tone-ctrl-g.txt)
log=$(grep -v '^#' build/tone-ctrl-g.txt)
check "Ctrl-G right after the power-on beep: the same note runs on, 12 frames on, one gate on and off" "$out$log" \
    "voice 1 on: 12 frames (pulse 12, noise 0); 1 note played, Fn $BELL-$BELL (934.5-934.5 Hz)" "packets (state changes): 2"

# --- noise: an 8-bit LFSR ($B8 taps) picks each delay, 1024 clicks ---
NOISE='300:A9 01 85 06 A9 04 85 07 A0 00 A5 06 4A 90 02 49 B8 85 06 AA CA D0 FD 8D 30 C0 88 D0 ED C6 07 D0 E9 60\r300G\r'
out=$("$EMU" --headless --keys "$NOISE" --seconds 1 --sound-report --tone-log build/tone-noise.txt)
log=$(grep -v '^#' build/tone-noise.txt)
check "noise: 40 irregular frames play as SID noise, Fn from the click rate" "$out$log" \
    "sound: 1216 clicks in 2 events" "irregular 40, sparse 1" "voice 1 on: 45 frames (pulse 6, noise 39)" \
    "on  noise    1911  2048     116.5  irregular      30"

# --- a 50 Hz tone: 10,301 cycles a flip (STA 4, LDX 2, 8 x 1,286 - 1, DEC 5, BNE 3), under 4 clicks a frame ---
SLOW='300:A9 64 85 07 8D 30 C0 A2 08 A0 00 88 D0 FD CA D0 F8 C6 07 D0 EF 60\r300G\r'
F50=$(fn 20602)
out=$("$EMU" --headless --keys "$SLOW" --seconds 2 --sound-report --tone-log build/tone-slow.txt)
log=$(grep -v '^#' build/tone-slow.txt)
check "a 50 Hz tone is followed across frames: Fn $F50 (bc), 59 slow frames" "$out$log" \
    "slow 59," "voice 1 on: 65 frames (pulse 65, noise 0)" "on  pulse     $F50  2048      49.6  slow" \
    "steady 7, split 0, slow 59, irregular 0, sparse 2 frames"

# --- an uneven square: 200 cycles one way (LDX 2, 39 x 5 - 1, STA 4), 798 the other (LDX 2, 157 x 5 - 1, DEC 5, BNE 3, STA 4) ---
DUTY='300:A9 00 85 07 8D 30 C0 A2 27 CA D0 FD 8D 30 C0 A2 9D CA D0 FD C6 07 D0 EC 60\r300G\r'
FD=$(fn 998); PD=$(pw 200 798)
out=$("$EMU" --headless --keys "$DUTY" --seconds 1 --tone-log build/tone-duty.txt)
log=$(grep -v '^#' build/tone-duty.txt)
check "an uneven square: Fn $FD and pulse width $PD (bc, 200 of 998 cycles)" "$log" \
    "on  pulse   $FD   $PD    1022.5  steady" "off  pulse   $FD   $PD    1022.5  steady"

OT=disks/the-oregon-trail-800-k.woz
if [ -f "$OT" ]; then
    . tests/oregon-keys.sh
    "$EMU" --headless --disk "$OT" --keys "$KEYS" --seconds 1 --sound-report --wav build/oregon-sound.wav \
        --sid-wav build/oregon-sid.wav --tone-log build/oregon-tone.txt > build/oregon-tone-report.txt
    out=$(cat build/oregon-tone-report.txt)
    # S1's frame counts and the tracker's at S1's frame: two separate code paths over the same clicks
    s1=$(sed -n 's/^  steady tones: .*, \([0-9]*\) frames, \([0-9]*\) notes.*/steady \1 notes \2/p; s/^  split .*, \([0-9]*\) frames$/split \1/p;
                s/^  irregular: .*, \([0-9]*\) frames$/irregular \1/p; s/^  sparse: .*, \([0-9]*\) frames$/sparse \1/p' <<<"$out" | tr '\n' ' ')
    tr=$(sed -n "s/^  check at S1's frame (1\/60 Apple second): steady \([0-9]*\), split \([0-9]*\), slow 0, irregular \([0-9]*\), sparse \([0-9]*\) frames; \([0-9]*\) notes.*/steady \1 notes \5 split \2 irregular \3 sparse \4 /p" <<<"$out")
    [ -n "$s1" ] || s1="S1's lines not found"
    check "Oregon Trail: the tracker at S1's frame agrees with S1's report frame for frame ($s1)" "tracker: $tr|" "tracker: $s1|"
    LO=$(fn 4008); HI=$(fn 1890)
    check "Oregon Trail: Fn $LO-$HI (bc, 254.6-539.9 Hz), no noise, 115 packets, both WAVs" "$out" \
        "voice 1 on: 2832 frames (pulse 2832, noise 0); 109 notes played, Fn $LO-$HI (254.6-539.9 Hz)" \
        "out of the SID's range: 0 frames an octave or two down, 0 too high" \
        "packets (state changes): 115, at most 5 in any 60 frames" \
        'speaker saved: build/oregon-sound.wav' 'SID voice saved: build/oregon-sid.wav' 'tone log saved: build/oregon-tone.txt'
    cmp=$(python3 -I - build/oregon-sound.wav build/oregon-sid.wav build/oregon-tone.txt <<'PY'
import array, math, sys, wave
a, b = (array.array('h', w.readframes(w.getnframes())) for w in map(wave.open, sys.argv[1:3]))
B = 2205                                   # 0.1 s blocks
rms = lambda x: math.sqrt(sum(v * v for v in x) / len(x))
la = lb = both = 0
for k in range(min(len(a), len(b)) // B):
    x, y = rms(a[k * B:(k + 1) * B]) > 1000, rms(b[k * B:(k + 1) * B]) > 1000
    la += x; lb += y; both += x and y
rows = [l.split() for l in open(sys.argv[3]) if not l.startswith('#')]
runs = [(int(r[0]), r[2] == 'on', int(r[4])) for r in rows]
blips = sum(1 for p, q, r in zip(runs, runs[1:], runs[2:]) if p[1] and q[1] and r[1] and r[0] - q[0] == 1
            and abs(q[2] - p[2]) > 0.03 * p[2] and abs(q[2] - r[2]) > 0.03 * r[2])
ok = len(a) == len(b) and both >= 0.98 * max(la, lb) and blips == 0
print(f"{'match' if ok else 'differ'}: loud 0.1 s blocks speaker {la}, SID {lb}, both {both}; one-frame pitch blips {blips}")
PY
)
    check "Oregon Trail: the SID WAV sounds where the speaker does, with no one-frame pitch blips" "$cmp" 'match:'
    echo "    $cmp"
    sed -n '/^tone tracker/,/^  check/p' build/oregon-tone-report.txt | sed 's/^/    /'
else echo "skip Oregon Trail tone tracker (no $OT)"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
