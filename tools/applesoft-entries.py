#!/usr/bin/env python3
"""applesoft-entries.py: which of Applesoft's entry points our BASIC answers at Applesoft's own address.

Usage (from project/work/apple2, after `make`):
  python3 -I tools/applesoft-entries.py              report: aligned, pinned, or not there
  python3 -I tools/applesoft-entries.py --write      also write build/entries.txt for
                                                                  a2term --stop-unpinned

An entry point is "aligned" when our routine of that name starts at Applesoft's address, and "pinned" when
the address holds a JMP (PINJ/PINF in rom/basic.s). build/entries.txt lists both, so a2term can stop at a
program's call into BASIC's space at any other address. Writes only build/entries.txt.

SOURCE OF THE TABLE: from memory of the published Applesoft entry-point lists (not yet checked against a
document line by line). Evidence that it is right: our routines, derived from the same Microsoft source,
sit at a constant offset from these addresses over long runs (+18 across 30 of them before alignment), and
the offset only changes where the code differs. Programs that call an address not in this table show up in
a2term --rom-calls anyway.
"""
import re
import sys

AS = {
    'ERROR': 0xD412, 'RESTART': 0xD43C, 'INLIN': 0xD52C, 'CRUNCH': 0xD559, 'FNDLIN': 0xD61A, 'SCRTCH': 0xD649,
    'CLEAR': 0xD66A, 'STKINI': 0xD683, 'STXTPT': 0xD697, 'NEWSTT': 0xD7D2, 'RESTOR': 0xD849, 'ISCNTC': 0xD858,
    'STOP': 0xD86E, 'END': 0xD870, 'CONT': 0xD896, 'RUN': 0xD912, 'GOSUB': 0xD921, 'GOTO': 0xD93E,
    'RETURN': 0xD984, 'DATA': 0xD995, 'DATAN': 0xD9A3, 'IF': 0xD9C9, 'REM': 0xD9DC, 'ONGOTO': 0xD9EC,
    'LINGET': 0xDA0C, 'LET': 0xDA46, 'PRINT': 0xDAD5, 'CRDO': 0xDAFB, 'STROUT': 0xDB3A, 'STRPRT': 0xDB3D,
    'OUTSPC': 0xDB57, 'OUTQST': 0xDB5A, 'OUTDO': 0xDB5C, 'INPUT': 0xDBB2, 'READ': 0xDBE2, 'NEXT': 0xDCF9,
    'FRMNUM': 0xDD67, 'CHKNUM': 0xDD6A, 'CHKSTR': 0xDD6C, 'FRMEVL': 0xDD7B, 'EVAL': 0xDE60, 'PARCHK': 0xDEB2,
    'CHKCLS': 0xDEB8, 'CHKOPN': 0xDEBB, 'CHKCOM': 0xDEBE, 'SYNCHR': 0xDEC0, 'SNERR': 0xDEC9, 'PTRGET': 0xDFE3,
    'AYINT': 0xE10C, 'IQERR': 0xE199, 'GIVAYF': 0xE2F2, 'SNGFLT': 0xE301, 'ERRDIR': 0xE306, 'STRINI': 0xE3D5, 'STRSPA': 0xE3DD, 'STRLIT': 0xE3E7, 'STRLT2': 0xE3ED,
    'PUTNEW': 0xE42A, 'GETSPA': 0xE452, 'GARBAG': 0xE484, 'CAT': 0xE597, 'MOVINS': 0xE5D4, 'MOVSTR': 0xE5E2, 'FRESTR': 0xE5FD,
    'FREFAC': 0xE600, 'FRETMP': 0xE604, 'GTBYTC': 0xE6F5, 'GETBYT': 0xE6F8, 'CONINT': 0xE6FB, 'GETNUM': 0xE746,
    'COMBYT': 0xE74C, 'GETADR': 0xE752, 'PEEK': 0xE764, 'POKE': 0xE77B, 'FADDH': 0xE7A0, 'FSUB': 0xE7A7,
    'FADD': 0xE7BE, 'FMULT': 0xE97F, 'CONUPK': 0xE9E3, 'FDIV': 0xEA66, 'MOVFM': 0xEAF9, 'MOVMF': 0xEB2B,
    'MOVFA': 0xEB53, 'MOVAF': 0xEB63, 'SIGN': 0xEB82, 'SGN': 0xEB90, 'FLOAT': 0xEB93, 'ABS': 0xEBAF,
    'FCOMP': 0xEBB2, 'QINT': 0xEBF2, 'INT': 0xEC23, 'FIN': 0xEC4A, 'LINPRT': 0xED24, 'FOUT': 0xED34,
    'SQR': 0xEE8D, 'FPWRT': 0xEE97, 'EXP': 0xEF09, 'RND': 0xEFAE, 'COS': 0xEFEA, 'SIN': 0xEFF1, 'TAN': 0xF03A,
    'ATN': 0xF09E,
    # Added from evidence, not from the list: Oregon Trail's & INP does JSR STRLT2 then JMP $DA7B, the string
    # half of LET (LET at $DA46 plus Microsoft's 53 bytes of LET code before INPCOM).
    'INPCOM': 0xDA7B,
    # Also from evidence: Oregon Trail's library does PTRGET, GIVAYF, LDA INTFLG, JSR $DA63 to store a number
    # in a variable, as INPUT does (QINTGR, LET at $DA46 plus Microsoft's 29 bytes).
    'QINTGR': 0xDA63,
    # DOS 3.3's RUN (source ASRUN1) and Oregon Trail's loader: JSR $D665 (RUNC), JMP $D7D2.
    # More from Oregon Trail's & library (its calls show what each does): JSR $D66C after setting VARTAB (CLEAR
    # past its BNE: CLEARC); TSX, STX $F8, JMP $D7D8 (NEWSTT past its stack save and Ctrl-C check); LINNUM set,
    # JSR $D941 (GOTO past its LINGET); JMP $DD76 when a variable has the wrong type (CHKERR, ?TYPE MISMATCH).
    'RUNC': 0xD665, 'CLEARC': 0xD66C, 'NEWSTC': 0xD7D8, 'GOTOLN': 0xD941, 'CHKERR': 0xDD76,
}


def main():
    ours = {}
    for line in open("build/basic.lst", encoding="latin-1"):
        m = re.match(r"^00([0-9A-F]{4})  1  .{0,12} *([A-Z0-9_]+):", line)
        if m and m.group(2) not in ours:
            ours[m.group(2)] = int(m.group(1), 16)
    rom = open("build/tr-rom.bin", "rb").read()
    good, report = [], {"aligned": [], "pinned": [], "not there": []}
    for name, a in sorted(AS.items(), key=lambda kv: kv[1]):
        if ours.get(name) == a:
            report["aligned"].append(name); good.append(a)
        elif rom[a - 0xD000] == 0x4C:
            report["pinned"].append(name); good.append(a)
        else:
            off = ours.get(name)
            report["not there"].append(f"{name}{off - a:+d}" if off is not None else f"{name}(none)")
    for k, v in report.items():
        print(f"{k} ({len(v)}): {' '.join(v)}")
    pins = [int(x, 16) for x in re.findall(r"^\s+PIN[JF] \$([0-9A-F]{4})", open("rom/basic.s").read(), re.M)]
    pins += [0xE003]                                   # the warm start, right after $E000's PINJ
    pins += [0xDEB2, 0xDEB8, 0xDEBB, 0xDEBE, 0xDEC0, 0xDEC9]   # the block of entry points at $DEB2 (EVSTB)
    pins += [0xDD67, 0xDD6A, 0xDD6C, 0xDD7B]                   # the block at $DD67 (NXSTB)
    pins += [0xF6B9]                                           # HFNS, written at its address
    good += [a for a in pins if rom[a - 0xD000] in (0x4C, 0xA9, 0x18, 0x38, 0x20)]   # a JMP, or a block's LDA/CLC/SEC
    print(f"pins in rom/basic.s: {' '.join(f'${a:04X}' for a in sorted(pins))}")
    if "--write" in sys.argv:
        with open("build/entries.txt", "w") as f:
            for a in sorted(set(good)):
                f.write(f"{a:04X}\n")
        print(f"wrote build/entries.txt ({len(set(good))} addresses)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
