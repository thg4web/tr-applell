#!/usr/bin/env bash
# test-basic.sh: our BASIC (Microsoft's BASIC-M6502 with Applesoft's layout, rom/basic.s), typed into the
# headless emulator. Each case enters BASIC from the Monitor with Ctrl-B (XBASIC, $FEB0 -> $E000).
# Usage: tests/test-basic.sh        (from the apple2 folder, after `make`)
# Read-only: nothing outside build/ is touched. Exit 0 only if every case passes.
# Exit status 0 from the emulator also means no missing ROM routine ($02 byte) was called.
# Expected output follows the Applesoft II BASIC Programming Reference Manual: no banner and no "OK",
# numbers printed with no spaces around them, long error messages, TAB counting from 1.
cd "$(dirname "$0")/.." || exit 2
EMU=./build/a2term
pass=0; fail=0

# check NAME KEYS EXPECTED_EXIT TEXT...   every TEXT must appear on the final screen/report
check() {
    local name=$1 keys=$2 want_exit=$3; shift 3
    local out; out=$("$EMU" --headless --keys "$keys" --seconds ${SECONDS_RUN:-1}); local got_exit=$?
    local ok=1
    [ "$got_exit" = "$want_exit" ] || ok=0
    for t in "$@"; do grep -qF -- "$t" <<<"$out" || { ok=0; echo "    missing: $t"; }; done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"
    else fail=$((fail+1)); echo "FAIL $name (exit $got_exit, wanted $want_exit)"; echo "$out" | sed 's/^/    /'; fi
}

GO='\x02\r'                          # Ctrl-B, Return: BASIC's cold start
PROG='10 A$="ABC"\r20 FOR I=1 TO 3\r30 GOSUB 100\r40 NEXT I\r50 PRINT LEN(A$+"][");MID$("ABCDEF",2,3)\r60 END\r100 PRINT A$;I*I\r110 RETURN\r'

check "Ctrl-B starts BASIC: the ] prompt, no banner" "$GO" 0 '|*' '|] '
check "D000G starts it too"          'D000G\r'                   0 '|] '
check "cold start finds RAM up to \$C000 and leaves it alone" "${GO}PRINT PEEK(115)+256*PEEK(116);PEEK(103)+256*PEEK(104)\r" 0 '|491522049 '
check "PRINT 2+2 gives 4, no spaces, no OK" "${GO}PRINT 2+2\r"   0 '|]PRINT 2+2' '|4 ' '|] '
check "program: FOR, GOSUB, strings" "${GO}${PROG}RUN\r"         0 '|ABC1 ' '|ABC4 ' '|ABC9 ' '|5BCD '
check "math: SQR, division, INT"     "${GO}PRINT SQR(2);1/3;INT(-2.5)\r" 0 '|1.41421356.333333333-3 '
check "arrays, real and integer"     "${GO}DIM A(10),B%(3)\rFOR I=0 TO 10:A(I)=I*I:NEXT\rS=0:FOR I=0 TO 10:S=S+A(I):NEXT\rB%(2)=-7.9:PRINT S;B%(2)\r" 0 '|385-8 '   # this BASIC converts to integer by rounding down (as INT does)
check "long error messages, IN line" "${GO}PRINT 1/0\rGOTO 999\r10 X=1/0\rRUN\r" 0 '|?DIVISION BY ZERO ERROR' "|?UNDEF'D STATEMENT ERROR" '|?DIVISION BY ZERO ERROR IN 10'
check "LIST spaces keywords as Applesoft does" "${GO}${PROG}LIST 20-40\r"  0 '|20  FOR I = 1 TO 3' '|30  GOSUB 100' '|40  NEXT I'
check "typed spaces are dropped, REM keeps its text" "${GO}10 X = 1 : REM  HI\rLIST\r" 0 '|10 X = 1: REM   HI'
check "spaces inside a keyword: G O T O"  "${GO}10 G O T O 20\r20 PRINT \"T\"\rLIST 10\rRUN\r" 0 '|10  GOTO 20' '|T '
check "FOR I=BETA TO 3 (A before TO is not AT)" "${GO}BETA=1:FOR I=BETA TO 3:PRINT I;:NEXT\r" 0 '|123'
check "ATN is not AT N"              "${GO}PRINT INT(ATN(1)*400)\r"  0 '|314 '
check "INPUT prints ? alone"         "${GO}10 INPUT N\r20 PRINT N*2\rRUN\r21\r" 0 '|?21' '|42 '
check "INPUT with a prompt prints no ?" "${GO}10 INPUT \"N=\";N\r20 PRINT N*2\rRUN\r21\r" 0 '|N=21' '|42 '
check "INPUT: an empty answer gets ??" "${GO}10 INPUT N\r20 PRINT N*2\rRUN\r\r4\r" 0 '|??4' '|8 '
check "INPUT: a bad answer gets ?REENTER" "${GO}10 INPUT N\r20 PRINT N\rRUN\rX\r5\r" 0 '|?REENTER' '|5 '
check "PRINT comma: tab fields at 1, 17, 33" "${GO}PRINT 1,2,3\rPRINT \"ABCDEFGHIJKLMNOPQRST\",4\r" 0 '|1               2               3' '|ABCDEFGHIJKLMNOPQRST            4'
check "TAB(n) is position n; SPC" "${GO}PRINT TAB(5);\"X\";SPC(2);\"Y\"\r" 0 '|    X  Y '
check "negative addresses: PEEK(-16384), CALL -151" "${GO}PRINT PEEK(-16384)>127;PEEK(64600)=PEEK(-936)\rCALL -151\r" 0 '|11' '|*'
check "true is 1; AND, OR, NOT are logical" "${GO}PRINT 3>2;3<2;NOT 0;NOT 5;2 AND 4;0 AND 1;0 OR 3;0 OR 0\rIF 2 AND 4 THEN PRINT \"YES\"\r" 0 '|10101010 ' '|YES '
check "Ctrl-C in the Monitor is a warm start: the program is kept" "${GO}10 PRINT 7\rCALL -151\r\x03\rLIST\rRUN\r" 0 '|10  PRINT 7' '|7 '
check "a line longer than the screen" "${GO}PRINT \"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJ\"\r" 0 \
    '|ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD|' '|EFGHIJ '
check "a stored line over 128 bytes (Microsoft's signed copy loop)" "${GO}10 REM $(printf 'X%.0s' $(seq 1 180))\rPRINT PEEK(2049)+256*PEEK(2050)-2049\r" 0 '|187 '
check "strings: CHR\$, ASC, STR\$, VAL, LEFT\$, RIGHT\$" \
    "${GO}PRINT CHR\$(65);ASC(\"B\");STR\$(12);VAL(\"3.5\")*2;LEFT\$(\"APPLE\",2);RIGHT\$(\"APPLE\",2)\r" 0 '|A66127APLE '
check "Ctrl-C (RUN/STOP on the C64) breaks a running program" "${GO}10 GOTO 10\rRUN\r\\w\\x03" 0 '|BREAK IN 10 '
check "HOME, VTAB, HTAB, INVERSE, NORMAL" "${GO}HOME:VTAB 5:HTAB 10:PRINT \"X\";:INVERSE:PRINT \"I\";:NORMAL:PRINT \"N\"\rVTAB 25\r" 0 '|         XIN ' '|?ILLEGAL QUANTITY ERROR'
check "& jumps to \$3F5"             "${GO}POKE 1013,96\r&\rPRINT \"BACK\"\r" 0 '|BACK '
check "an Applesoft statement not written yet stops on its trap" "${GO}GR\r" 1 'STOPPED: missing ROM routine at $'
# Hi-res (Applesoft II manual 1981, HGR2, HCOLOR, HPLOT): the bytes the dots land in, read back after TEXT.
# HCOLOR=7 dot at 0,0 then HPLOT TO 6,0 after HCOLOR=0: the line keeps the last dot's color (255); green at x=7
# (odd, in an odd byte) lights bit 0, at x=8 (even) nothing; a line to 279,191 ends on byte $5FF7 bit 6 (64).
check "HGR2, HCOLOR=, HPLOT and HPLOT TO" "${GO}HGR2:HCOLOR=7:HPLOT 0,0:HCOLOR=0:HPLOT TO 6,0:HCOLOR=1:HPLOT 7,1:HPLOT 8,1:HCOLOR=3:HPLOT 0,2 TO 0,191:HPLOT 0,3 TO 279,191\rTEXT:PRINT PEEK(16384);\" \";PEEK(17409);\" \";PEEK(24528);\" \";PEEK(24567)\r" 0 '|255 1 1 64 '
check "HCOLOR= and HPLOT check their ranges" "${GO}HCOLOR=8\rHPLOT 280,0\rHPLOT 0,192\rHCOLOR=7:PRINT \"OK\"\r" 0 '|?ILLEGAL QUANTITY ERROR' '|]HPLOT 0,192' '|OK '
check "HGR: page 1, mixed, cleared; HPLOT draws on it" "${GO}POKE 8192,85:HGR:HCOLOR=3:HPLOT 1,0\rPRINT PEEK(8192);PEEK(230)\r" 0 '|232 ' 'display: HI-RES + 4 text lines, page 1'

# CAVE DIVER (programs/cave-diver.txt, the program F5 types in on the C64), typed in here as keys.
CAVE=$(sed 's/$/\\r/' programs/cave-diver.txt | tr -d '\n')
# RUN clears the screen first (USR is pointed at the Monitor's HOME): the title is on the top row.
top=$("$EMU" --headless --keys "${GO}${CAVE}RUN\r" --seconds 1 | sed -n '2p')
if [ "$top" = "|CAVE DIVER                              |" ]; then pass=$((pass+1)); echo "ok   CAVE DIVER: RUN clears the screen, title on the top row"
else fail=$((fail+1)); echo "FAIL CAVE DIVER: RUN clears the screen (top row was: $top)"; fi
# Line 52 (added 2026-11-24) clears the screen again when a dive starts: the cave begins on the top row.
top=$("$EMU" --headless --keys "${GO}${CAVE}RUN\r\\w3" --seconds 1 | sed -n '2p')
if grep -q '^|.*!.*\*.*!' <<<"$top"; then pass=$((pass+1)); echo "ok   CAVE DIVER: a new dive clears the screen (line 52)"
else fail=$((fail+1)); echo "FAIL CAVE DIVER: a new dive clears the screen (top row was: $top)"; fi
SECONDS_RUN=30 check "CAVE DIVER: types in, dives (level 3), crashes, shows the depth" "${GO}${CAVE}RUN\r\\w3" 0 \
    'CRASH! DEPTH:' '|PRESS 1, 2 OR 3 TO DIVE AGAIN'    # the title has scrolled off by then
# A steers left: the diver's column on the first cave line, then on the last whole line.
cols=$("$EMU" --headless --keys "${GO}${CAVE}RUN\r\\w3\\wA\\wA\\wA" --seconds 0.5 | sed -n '2,25p' | grep '!.*\*.*!' | awk '{print index($0, "*")}' | sed -n '1p;$p' | tr '\n' ' ')
set -- $cols
if [ -n "$2" ] && [ "$2" -lt "$1" ]; then pass=$((pass+1)); echo "ok   CAVE DIVER: A steers left (column $1 to $2)"
else fail=$((fail+1)); echo "FAIL CAVE DIVER: A steers left (columns: $cols)"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
