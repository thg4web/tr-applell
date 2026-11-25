#!/usr/bin/env python3
"""m6502-to-ca65.py: convert Microsoft's BASIC-M6502 source (MACRO-10 syntax, 1978) into
ca65 source, line for line, for the TR ][ ROM.

Usage:
  python3 -I m6502-to-ca65.py SRC                    dry run: convert in memory, print a summary
  python3 -I m6502-to-ca65.py SRC --write OUT.s      also write the ca65 source
  python3 -I m6502-to-ca65.py SRC --stdout           print the ca65 source (for diffs and checks)
Options:
  --set NAME=VALUE   use VALUE wherever the source sets NAME (repeatable; the output marks the
                     line [OURS]), for example --set ROMLOC=0xD000 --set RAMLOC=0x0800
  --switch NAME=VALUE  same, for a build switch such as REALIO (the source's own is REALIO=4,
                     the Apple build); a different switch gives a different BASIC

Dry run by default. --write refuses to overwrite an existing file: the written file is ours to
edit by hand afterwards, so a regenerated copy goes out under a new name and gets compared.

SRC is third-party/basic-m6502/m6502.asm (Microsoft, MIT licence). The tool reads it and
nothing else. Two stages:
  1. A MACRO-10 text preprocessor. It resolves IFE / IFN / IF1 / IF2 / IFNDEF / REPEAT with the
     switch values the source sets, stores DEFINE macros and expands them where they are used,
     and turns COMMENT blocks into ; comments. As in MACRO-10, every < and > counts as a
     bracket, in strings and comments too (Microsoft wrote 188 and 190 for "<" and ">").
  2. A line translator to ca65: labels, assignments, the 6502 instructions with Microsoft's
     mode suffixes (LDAI = lda #, LDADY = lda (zp),y, JMPD = jmp (abs)), data (ADR, EXP, XWD,
     DC, DT and bare expressions, one byte per MACRO-10 word), BLOCK, ORG and RADIX.
Pseudo-ops from Microsoft's M6502 universal file, which is not published, are done here from
how the source uses them: ADR = a 16-bit word, low byte first; DC = a string whose last
character has bit 7 set (the error printer stops on it); DT = a plain string; XWD 1000,n = the
byte n (an opcode, used for the BIT skip tricks); EXP = one byte per value.

Every output line carries "[MS nnnn]", the m6502.asm line it came from, plus the macro name
when it came out of a macro. Assembly-time variables that the source sets more than once
(Q, LINLEN, ...) become ca65 .set symbols; anything computed from them is written as a number.
Zero page goes in segment BZP and page one in BPG1 (both addresses only, nothing is loaded:
Microsoft's INIT copies CHRGET and the random seed down from ROM); the code goes in BASIC.
"""
import argparse
import os
import re
import sys
from collections import Counter

BASE_OPS = set("""ADC AND ASL BCC BCS BEQ BIT BMI BNE BPL BRK BVC BVS CLC CLD CLI CLV CMP CPX
    CPY DEC DEX DEY EOR INC INX INY JMP JSR LDA LDX LDY LSR NOP ORA PHA PHP PLA PLP ROL ROR RTI
    RTS SBC SEC SED SEI STA STX STY TAX TAY TSX TXA TXS TYA""".split())
ACC_OPS = {"ASL", "LSR", "ROL", "ROR"}
NATIVE = {"DT", "DC", "ADR", "EXP", "XWD"}
LISTING = {"PAGE", "SUBTTL", "TITLE", "SEARCH", "SALL", "XLIST", "LIST", ".XCREF", ".CREF",
           "PRINTX", "PURGE"}
COND = {"IFE", "IFN", "IF1", "IF2", "IFNDEF", "IFDEF", "REPEAT", "IFDIF", "IFIDN"}
# ORG targets below ROM, and the segment each one belongs in.
RAM_ORGS = {0: "BZP", 80: "BZP", 255: "BPG1"}
# Labels that only the PDP-10 simulator build uses.
SKIP_LABELS = {"$Z"}

WORD = re.compile(r"[A-Za-z.$%][A-Za-z0-9.$%]*")
TOKEN = re.compile(r"""\s+|\^[OoDdBb][0-9]+|[0-9]+\.?|"[^"]*"|[A-Za-z.$%][A-Za-z0-9.$%]*|[-+*/&!<>(),]""")


class ConvertError(Exception):
    pass


def canon(name):
    """MACRO-10 symbols are significant to six characters: RESTORE is the label RESTOR."""
    return name.upper()[:6]


ODD_DIGITS = []   # octal-radix numbers holding an 8 or 9, reported in the summary
LOW_BYTES = []    # immediate values outside 0-255, cut to their low byte as MACRO-10 does


# ---------------------------------------------------------------- expressions

def tokens(expr, radix, where):
    """MACRO-10 expression -> list of (kind, value, decimal?) tokens."""
    out, i = [], 0
    while i < len(expr):
        m = TOKEN.match(expr, i)
        if not m:
            raise ConvertError(f"{where}: cannot read expression {expr!r} at {expr[i:]!r}")
        t = m.group(0)
        i = m.end()
        if t.isspace():
            continue
        if t[0] == "^":
            base = {"O": 8, "D": 10, "B": 2}[t[1].upper()]
            out.append(("num", int(t[2:], base), base == 10))
        elif t[0].isdigit():
            if t.endswith("."):
                out.append(("num", int(t[:-1], 10), True))
            else:
                v = 0
                for d in t:                    # MACRO-10 does the arithmetic even for an 8 or 9
                    v = v * radix + int(d)     # in octal: "8" is 8 (QINT's 8*ADDPRC+230)
                if radix == 8 and re.search(r"[89]", t):
                    ODD_DIGITS.append(f"{where}: {t} in octal = {v}")
                out.append(("num", v, radix == 10))
        elif t[0] == '"':
            text = t[1:-1]
            v = 0
            for ch in text:
                v = (v << 7) | (ord(ch) & 0x7F)
            out.append(("chr", v, text))
        elif t == ".":
            out.append(("here", None, None))
        elif t[0].isalpha() or t[0] in ".$%":
            out.append(("sym", canon(t), None))
        else:
            out.append(("op", t, None))
    return out


def evaluate(expr, radix, syms, where=""):
    """Value of a MACRO-10 expression from known constants, or None if it needs a label."""
    py = []
    for kind, v, _ in tokens(expr, radix, where):
        if kind in ("num", "chr"):
            py.append(str(v))
        elif kind == "sym":
            if v not in syms:
                return None
            py.append(str(syms[v]))
        elif kind == "here":
            return None
        else:
            py.append({"<": "(", ">": ")", "!": "|", "/": "//"}.get(v, v))
    if not py:
        return None
    try:
        return int(eval("".join(py), {"__builtins__": {}}, {}))
    except Exception as e:  # malformed: report where
        raise ConvertError(f"{where}: cannot evaluate {expr!r}: {e}")


def fmt_num(v, decimal):
    if decimal or 0 <= v < 10:
        return str(v)
    if v < 0:
        return "-" + fmt_num(-v, False)
    return f"${v:02X}" if v < 256 else f"${v:04X}"


def fmt_chr(v, text):
    if len(text) == 1 and text.isprintable() and text not in "'\\":
        return f"'{text}'"
    return str(v)


def to_ca65(expr, radix, where, varsyms, syms):
    """MACRO-10 expression -> ca65 expression text. Symbols in varsyms (set more than once) are
    replaced by their current value."""
    toks = tokens(expr, radix, where)
    parts = []
    for kind, v, extra in toks:
        if kind == "num":
            parts.append(fmt_num(v, extra))
        elif kind == "chr":
            parts.append(fmt_chr(v, extra))
        elif kind == "here":
            parts.append("*")
        elif kind == "sym":
            if v in varsyms:
                if v not in syms:
                    raise ConvertError(f"{where}: {v} used before the source sets it")
                parts.append(fmt_num(syms[v], True))
            else:
                parts.append(v)
        else:
            parts.append({"<": "(", ">": ")", "!": "|"}.get(v, v))
    # Drop brackets around a single atom: <WD>+1 -> WD+1.
    changed = True
    while changed:
        changed = False
        for k in range(len(parts) - 2):
            if parts[k] == "(" and parts[k + 2] == ")" and parts[k + 1] not in "()+-*/&|,":
                parts[k:k + 3] = [parts[k + 1]]
                changed = True
                break
    text = "".join(parts)
    # MACRO-10 binds & and ! differently from ca65; Microsoft always brackets them, check it.
    depth_ops = re.sub(r"\([^()]*\)", "", text)
    while re.search(r"\([^()]*\)", depth_ops):
        depth_ops = re.sub(r"\([^()]*\)", "", depth_ops)
    if re.search(r"[&|]", depth_ops) and re.search(r"[-+*/]", depth_ops):
        raise ConvertError(f"{where}: & or ! mixed with other operators, unbracketed: {expr!r}")
    return text


# ---------------------------------------------------------------- stage 1: preprocessor

def find_close(text, i, where):
    """text[i] is '<'; index of the matching '>'. Every bracket counts, as in MACRO-10."""
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return j
    raise ConvertError(f"{where}: no closing '>'")


def find_paren(text, i, where):
    """text[i] is '('; index of the matching ')', skipping strings."""
    depth, j = 0, i
    while j < len(text):
        c = text[j]
        if c == '"':
            j = text.index('"', j + 1)
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    raise ConvertError(f"{where}: no closing ')'")


def split_args(s):
    """Split macro arguments at top-level commas."""
    args, depth, cur, instr = [], 0, "", False
    for c in s:
        if c == '"':
            instr = not instr
        elif not instr and c in "<(":
            depth += 1
        elif not instr and c in ">)":
            depth -= 1
        if c == "," and depth == 0 and not instr:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        args.append(cur.strip())
    return args


class Preprocessor:
    def __init__(self, overrides, labels):
        self.syms = {}
        self.overrides = overrides
        self.labels = labels
        self.macros = {}
        self.radix = 10
        self.chars = []      # output characters
        self.origin = []     # (source line, macro name or None) for each output character
        self.gensym = 0
        self.stats = Counter()

    def put(self, s, org):
        for k, c in enumerate(s):
            self.chars.append(c)
            self.origin.append(org[k] if isinstance(org, list) else org)

    def process(self, text, org):
        """org: one origin per character of text (list), or one origin for all (tuple)."""
        at = lambda k: org[k] if isinstance(org, list) else org
        n, i, stmt = len(text), 0, True
        while i < n:
            c = text[i]
            where = f"line {at(i)[0]}"
            if c == "\n":
                self.put(c, at(i)); i += 1; stmt = True; continue
            if c == ";":
                j = text.find("\n", i)
                j = n if j < 0 else j
                self.put(text[i:j], org[i:j] if isinstance(org, list) else org); i = j; continue
            if c in " \t":
                self.put(c, at(i)); i += 1; continue
            if c == '"':
                j = text.index('"', i + 1) + 1
                self.put(text[i:j], org[i:j] if isinstance(org, list) else org); i = j; stmt = False; continue
            m = WORD.match(text, i) if stmt else None
            if not m:
                self.put(c, at(i)); i += 1
                if not c.isspace():
                    stmt = False
                continue
            word, j = canon(m.group(0)), m.end()
            if text[j:j + 1] == ":":                                   # label
                k = j + 2 if text[j + 1:j + 2] == ":" else j + 1
                self.put(text[i:k], org[i:k] if isinstance(org, list) else org)
                i = k; continue
            if word in COND:
                i = self.conditional(word, text, org, i, j, where); stmt = False; continue
            if word == "DEFINE":
                i = self.define(text, j, where); stmt = False; continue
            if word == canon("COMMENT"):
                i = self.comment(text, org, j, where); stmt = False; continue
            if word == "RADIX":
                k = text.find("\n", j); k = n if k < 0 else k
                code = text[j:k].split(";")[0].strip()
                self.radix = int(code, 10)
            if word in self.macros and word not in NATIVE:
                i = self.expand(word, text, at(i), j, where); stmt = False; continue
            a = re.compile(r"\s*==?").match(text, j)
            if a:                                                      # NAME=expr or NAME==expr
                k = text.find("\n", a.end()); k = n if k < 0 else k
                rhs = text[a.end():k].split(";")[0].strip()
                if word in self.overrides:
                    self.syms[word] = self.overrides[word]
                else:
                    v = evaluate(rhs, self.radix, self.syms, where)
                    if v is not None:
                        self.syms[word] = v
            self.put(text[i:j], org[i:j] if isinstance(org, list) else org)
            i, stmt = j, False

    def conditional(self, word, text, org, i, j, where):
        # the head runs to the first comma outside brackets: IFN <<A>/256>-<<B>/256>,<...>
        depth, k = 0, j
        while k < len(text) and not (text[k] == "," and depth == 0):
            if text[k] == "\n":
                raise ConvertError(f"{where}: {word} without ',<'")
            depth += {"<": 1, ">": -1}.get(text[k], 0)
            k += 1
        head = text[j:k].strip()
        lt = k + 1
        while text[lt] in " \t":
            lt += 1
        if text[lt] != "<":
            raise ConvertError(f"{where}: {word} without ',<'")
        gt = find_close(text, lt, where)
        body = text[lt + 1:gt]
        body_org = org[lt + 1:gt] if isinstance(org, list) else org
        if word in ("IFE", "IFN"):
            v = evaluate(head, self.radix, self.syms, where)
            if v is None:
                raise ConvertError(f"{where}: {word} {head} needs a value the tool does not know")
            take = (v == 0) if word == "IFE" else (v != 0)
            times = 1 if take else 0
        elif word == "IF1":
            times = 0                         # pass-1 only: PRINTX messages
        elif word == "IF2":
            times = 1                         # pass 2: PURGE (listing only) and a C64 block
        elif word in ("IFNDEF", "IFDEF"):
            known = canon(head) in self.labels or canon(head) in self.syms
            times = 1 if known == (word == "IFDEF") else 0
        elif word == "REPEAT":
            times = evaluate(head, self.radix, self.syms, where)
        else:
            raise ConvertError(f"{where}: {word} outside a macro the tool replaces")
        self.stats[f"{word} {'taken' if times else 'skipped'}"] += 1
        for n in range(times):               # REPEAT: each copy is its own line
            if n:
                self.put("\n", body_org[0] if isinstance(body_org, list) else body_org)
            self.process(body, body_org)
        return gt + 1

    def define(self, text, j, where):
        m = re.compile(r"\s*([A-Za-z.$%][A-Za-z0-9.$%]*)\s*(\(([^)]*)\))?\s*,\s*").match(text, j)
        if not m or text[m.end()] != "<":
            raise ConvertError(f"{where}: cannot read DEFINE")
        name = canon(m.group(1))
        params = [p.strip().upper() for p in m.group(3).split(",")] if m.group(3) else []
        gt = find_close(text, m.end(), where)
        self.macros[name] = (params, text[m.end() + 1:gt])
        self.stats["DEFINE"] += 1
        return gt + 1

    def comment(self, text, org, j, where):
        k = j
        while text[k] in " \t":
            k += 1
        delim = text[k]
        end = text.index(delim, k + 1)
        line0 = org[j][0] if isinstance(org, list) else org[0]
        for n, ln in enumerate(text[k + 1:end].split("\n")):
            if n:
                self.put("\n", (line0 + n, None))
            self.put("; " + ln.rstrip(), (line0 + n, None))
        self.stats["COMMENT block"] += 1
        return end + 1

    def expand(self, name, text, org, j, where):
        params, body = self.macros[name]
        args, k = [], j
        if params:
            if text[k:k + 1] == "(":
                close = find_paren(text, k, where)
                args = split_args(text[k + 1:close]); k = close + 1
            elif text[k:k + 1] == '"':
                close = text.index('"', k + 1)
                args = [text[k:close + 1]]; k = close + 1
            else:
                e = k
                while e < len(text) and text[e] not in ";\n":
                    e = text.index('"', e + 1) + 1 if text[e] == '"' else e + 1
                args = split_args(text[k:e]); k = e
        if len(args) != len(params):
            raise ConvertError(f"{where}: {name} wants {len(params)} argument(s), got {args}")
        out = body
        for p, a in zip(params, args):
            out = re.sub(rf"(?<![A-Za-z0-9.$%]){re.escape(p)}(?![A-Za-z0-9.$%])", lambda _: a, out)
        if "%" in out:                         # created labels, one per use
            self.gensym += 1
            out = re.sub(r"%([A-Z0-9]+)", lambda mm: f"{name}{mm.group(1)}{self.gensym}", out)
        self.stats[f"macro {name}"] += 1
        self.process(out, (org[0], name))
        return k


# ---------------------------------------------------------------- stage 2: line translator

def split_comment(line):
    instr = False
    for k, c in enumerate(line):
        if c == '"':
            instr = not instr
        elif c == ";" and not instr:
            return line[:k], line[k + 1:]
    return line, None


def string_arg(s, where):
    m = re.fullmatch(r'\(?\s*"([^"]*)"\s*\)?', s.strip())
    if not m:
        raise ConvertError(f"{where}: expected a string, got {s!r}")
    return m.group(1)


def ca65_string(s):
    if '"' in s or "\\" in s:
        raise ConvertError(f"string {s!r} needs escaping")
    return f'"{s}"'


class Translator:
    def __init__(self, lines, overrides, switches, varsyms, labels):
        self.lines = lines
        self.overrides = overrides
        self.switches = switches
        self.varsyms = varsyms
        self.labels = labels
        self.syms = {}
        self.radix = 10
        self.out = []
        self.segment = None
        self.absolute = False
        self.stats = Counter()

    def emit(self, code, origin, comment, ours=False):
        line, macro = origin
        tag = f"[MS {line}{' ' + macro if macro else ''}]" + (" [OURS]" if ours else "")
        note = f"; {tag}" + (f" {comment.strip()}" if comment and comment.strip() else "")
        code = code.rstrip()
        self.out.append((code.ljust(39) + " " + note) if code else note)

    def x(self, expr, where):
        return to_ca65(expr, self.radix, where, self.varsyms, self.syms)

    def run(self):
        for text, origin in self.lines:
            where = f"line {origin[0]}" + (f" ({origin[1]})" if origin[1] else "")
            code, comment = split_comment(text)
            if not code.strip() and comment is None:
                if not origin[1]:
                    self.out.append("")
                continue
            if code.strip() == "" :
                self.emit("", origin, comment); continue
            if self.line(code, comment, origin, where) == "END":
                break
        return self.out

    def line(self, code, comment, origin, where):
        code = code.strip()
        labels = []
        while True:
            m = re.match(r"([A-Za-z.$%][A-Za-z0-9.$%]*)(::?)\s*", code)
            if not m:
                break
            labels.append(canon(m.group(1)))
            code = code[m.end():]
        for lab in labels:
            if lab in SKIP_LABELS:
                self.emit("", origin, f"{lab}: (PDP-10 simulator entry, not used)")
            else:
                self.emit(f"{lab}:", origin, comment if not code else None)
                self.stats["labels"] += 1
        if not code:
            return
        # assignment
        m = re.fullmatch(r"([A-Za-z.$%][A-Za-z0-9.$%]*)\s*==?\s*(.*)", code)
        if m and not code.upper().startswith(("DC(", "DT(")):
            return self.assign(canon(m.group(1)), m.group(2).strip(), comment, origin, where)
        m = re.match(r"([A-Za-z.$%][A-Za-z0-9.$%]*)", code)
        op = canon(m.group(1)) if m else ""
        rest = code[m.end():].strip() if m else code
        if op in LISTING:
            if op == "SUBTTL":
                self.emit("", origin, f"==== {rest} ====")
            elif comment:
                self.emit("", origin, comment)
            return
        if op == "END":
            self.emit("", origin, f"END {rest} (end of Microsoft's source)")
            return "END"
        if op == "RADIX":
            self.radix = int(rest, 10)
            self.emit("", origin, f"RADIX {rest}: numbers below are {'octal' if self.radix == 8 else 'decimal'} in Microsoft's text" + (f"; {comment.strip()}" if comment else ""))
            return
        if op == "ORG":
            return self.org(rest, comment, origin, where)
        if op == "BLOCK":
            self.emit(f"        .res {self.x(rest, where)}", origin, comment); return
        if op == "ADR":
            inner = code[m.end():].strip()
            if not (inner.startswith("(") and inner.endswith(")")):
                raise ConvertError(f"{where}: ADR without (...)")
            self.emit(f"        .word {self.x(inner[1:-1], where)}", origin, comment); return
        if op == "EXP":
            vals = [self.x(a, where) for a in split_args(rest)]
            self.emit(f"        .byte {', '.join(vals)}", origin, comment); return
        if op == "XWD":
            left, right = split_args(rest)
            if evaluate(left, self.radix, {}, where) != 0o1000:
                raise ConvertError(f"{where}: XWD with left half {left}")
            self.emit(f"        .byte {self.x(right, where)}", origin, comment); return
        if op in ("DC", "DT"):
            s = string_arg(code[m.end():], where)
            if op == "DT" or not s:
                self.emit(f"        .byte {ca65_string(s)}", origin, comment)
            else:
                head = f"{ca65_string(s[:-1])}, " if len(s) > 1 else ""
                last = s[-1]
                lastx = f"'{last}'" if last not in "'\\" else str(ord(last))
                self.emit(f"        .byte {head}{lastx}|$80", origin, comment)
            return
        if op == "JMPD":
            self.emit(f"        jmp ({self.x(rest, where)})", origin, comment); return
        mode, base = None, None
        if op in BASE_OPS:
            base = op
        elif op.endswith("DY") and op[:-2] in BASE_OPS:
            base, mode = op[:-2], "dy"
        elif op.endswith("I") and op[:-1] in BASE_OPS:
            base, mode = op[:-1], "imm"
        if base:
            return self.instruction(base, mode, rest, comment, origin, where)
        # a bare expression: one byte
        self.emit(f"        .byte {self.x(code, where)}", origin, comment)
        self.stats["data bytes (bare)"] += 1

    def instruction(self, base, mode, rest, comment, origin, where):
        mn = base.lower()
        rest = rest.strip()
        while rest.endswith(","):
            rest = rest[:-1].strip()
        self.stats["instructions"] += 1
        if mode == "imm":
            m1 = re.fullmatch(r"<<(.+)>&\^O377>", rest.replace(" ", ""))
            m2 = re.fullmatch(r"<<(.+)>/\^O400>", rest.replace(" ", ""))
            if m1 or m2:
                inner = self.x((m1 or m2).group(1), where)
                if not re.fullmatch(r"[A-Za-z0-9_$']+", inner):
                    inner = f"({inner})"
                self.emit(f"        {mn} #{'<' if m1 else '>'}{inner}", origin, comment); return
            text = self.x(rest, where)
            v = evaluate(rest, self.radix, self.syms, where)
            if v is not None and not 0 <= v <= 255:   # MACRO-10 keeps the low 8 bits
                text = f"<({text})"
                self.stats["immediates cut to 8 bits"] += 1
                LOW_BYTES.append(f"{where}: {rest.strip()} = {v}, assembles as ${v & 255:02X}")
            elif v is None and re.search(r"[-+*/&|]", text):
                # arithmetic on a label (ISVRET-1)/256: ca65 cannot prove it fits a byte until
                # link time; <( ) keeps the low 8 bits, which is what MACRO-10 assembled
                text = f"<({text})"
                self.stats["label arithmetic as <( )"] += 1
            self.emit(f"        {mn} #{text}", origin, comment); return
        if mode == "dy":
            self.emit(f"        {mn} ({self.x(rest, where)}),y", origin, comment); return
        if not rest:
            self.emit(f"        {mn}", origin, comment); return
        if base in ACC_OPS and rest.upper() == "A":
            self.emit(f"        {mn} a", origin, comment); return
        reg = None
        m = re.fullmatch(r"(.*),\s*([XYxy])", rest)
        if m:
            rest, reg = m.group(1).strip(), m.group(2).lower()
        operand = self.x(rest, where)
        if operand.startswith("("):
            operand = "0+" + operand      # ca65 would read a leading ( as indirect
        self.emit(f"        {mn} {operand}" + (f",{reg}" if reg else ""), origin, comment)

    def assign(self, name, rhs, comment, origin, where):
        ours = False
        if name in self.overrides:
            value = self.overrides[name]
            text = fmt_num(value, False)
            note = f"Microsoft: {name}={rhs}" + (f"; {comment.strip()}" if comment else "")
            ours, comment = True, note
        else:
            value = evaluate(rhs, self.radix, self.syms, where)
            text = None
        if value is not None:
            self.syms[name] = value
        if name in self.varsyms:
            if value is None:
                raise ConvertError(f"{where}: {name} is set more than once but {rhs!r} has no value")
            self.emit(f"{name} .set {text or fmt_num(value, True)}", origin, comment if ours else (f"{rhs}" + (f"; {comment.strip()}" if comment else "")), ours)
            return
        uses_var = any(k == "sym" and v in self.varsyms for k, v, _ in tokens(rhs, self.radix, where))
        if text is None:
            text = fmt_num(value, True) if uses_var else self.x(rhs, where)
            if uses_var and comment is not None:
                comment = f"{rhs}; {comment.strip()}"
            elif uses_var:
                comment = rhs
        self.emit(f"{name} = {text}", origin, comment, ours)
        self.stats["symbols"] += 1

    def org(self, rest, comment, origin, where):
        v = evaluate(rest, self.radix, self.syms, where)
        if v is None:
            raise ConvertError(f"{where}: ORG {rest} has no value")
        if rest.strip().upper() == "ROMLOC":
            seg, ab = "BASIC", False
        elif v in RAM_ORGS:
            seg, ab = RAM_ORGS[v], True
        else:
            raise ConvertError(f"{where}: ORG {rest} = {v}: no segment for it")
        if seg != self.segment:
            if self.absolute:
                self.emit("        .reloc", origin, None)
            kind = ": zeropage" if seg == "BZP" else ""
            self.emit(f'        .segment "{seg}"{kind}', origin, f"ORG {rest}" + (f"; {comment.strip()}" if comment else ""))
            self.segment, self.absolute = seg, False
            comment = None
        if ab:
            self.emit(f"        .org {fmt_num(v, False)}", origin, comment)
            self.absolute = True


# ---------------------------------------------------------------- driver

def parse_sets(items, what):
    out = {}
    for it in items or []:
        if "=" not in it:
            raise SystemExit(f"--{what} wants NAME=VALUE, got {it!r}")
        k, v = it.split("=", 1)
        out[k.strip().upper()] = int(v, 0)
    return out


def convert(src_text, overrides):
    raw = src_text.replace("\r", "").replace("\f", "")
    labels = set(canon(m.group(1)) for m in re.finditer(r"(?m)^\s*([A-Za-z.$%][A-Za-z0-9.$%]*)::?", raw))
    org = [None] * len(raw)
    line = 1
    for k, c in enumerate(raw):
        org[k] = (line, None)
        if c == "\n":
            line += 1
    pre = Preprocessor(overrides, labels)
    pre.process(raw, org)
    # regroup into lines
    lines, cur, cur_org = [], [], None
    for c, o in zip(pre.chars, pre.origin):
        if c == "\n":
            lines.append(("".join(cur), cur_org or o)); cur, cur_org = [], None
            continue
        if cur_org is None and not c.isspace():
            cur_org = o
        cur.append(c)
    if cur:
        lines.append(("".join(cur), cur_org or (line, None)))
    # symbols the kept text sets more than once
    counts = Counter()
    for text, _ in lines:
        code, _c = split_comment(text)
        code = re.sub(r"^\s*([A-Za-z.$%][A-Za-z0-9.$%]*::?\s*)+", "", code)
        m = re.match(r"\s*([A-Za-z.$%][A-Za-z0-9.$%]*)\s*==?", code)
        if m and not code.strip().upper().startswith(("DC(", "DT(")):
            counts[canon(m.group(1))] += 1
    varsyms = {k for k, n in counts.items() if n > 1}
    tr = Translator(lines, overrides, {}, varsyms, labels)
    out = tr.run()
    return out, pre, tr, varsyms


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("--write", metavar="OUT")
    ap.add_argument("--stdout", action="store_true")
    ap.add_argument("--set", action="append", metavar="NAME=VALUE")
    ap.add_argument("--switch", action="append", metavar="NAME=VALUE")
    a = ap.parse_args()
    overrides = parse_sets(a.set, "set")
    overrides.update(parse_sets(a.switch, "switch"))
    with open(a.src, encoding="ascii", newline="") as f:
        src_text = f.read()
    try:
        out, pre, tr, varsyms = convert(src_text, overrides)
    except ConvertError as e:
        print(f"m6502-to-ca65: {e}", file=sys.stderr)
        return 1
    header = [
        "; basic.s: Microsoft BASIC-M6502 8K 1.1 (1978), the Apple build (REALIO=4), in ca65.",
        f"; Converted line for line by project/tools/apple2/m6502-to-ca65.py from {os.path.basename(a.src)}.",
        "; Microsoft's source is MIT licensed: third-party/basic-m6502/LICENSE.",
        "; Tags: [MS nnnn] = line nnnn of m6502.asm; [OURS] = our change.",
        "; Values set by the converter: " + (", ".join(f"{k}={fmt_num(v, False)}" for k, v in sorted(overrides.items())) or "none") + ".",
        "",
    ]
    text = "\n".join(header + out) + "\n"
    if a.stdout:
        sys.stdout.write(text)
    s = sys.stderr if a.stdout else sys.stdout
    print(f"read {src_text.count(chr(10))} lines; wrote {len(out)} lines", file=s)
    print(f"instructions {tr.stats['instructions']}, labels {tr.stats['labels']}, symbols {tr.stats['symbols']}, bare data lines {tr.stats['data bytes (bare)']}", file=s)
    print("conditionals and macros: " + ", ".join(f"{k} {v}" for k, v in sorted(pre.stats.items())), file=s)
    print("set more than once (.set): " + " ".join(sorted(varsyms)), file=s)
    for w in sorted(set(ODD_DIGITS)) + LOW_BYTES:
        print(f"note: {w}", file=s)
    if a.write:
        if os.path.exists(a.write):
            print(f"m6502-to-ca65: {a.write} exists; not overwriting (use a new name)", file=sys.stderr)
            return 1
        with open(a.write, "x", encoding="ascii") as f:
            f.write(text)
        print(f"wrote {a.write}", file=s)
    elif not a.stdout:
        print("dry run: nothing written (use --write OUT.s or --stdout)", file=s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
