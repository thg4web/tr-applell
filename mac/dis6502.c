/* dis6502.c: one-line 6502 disassembler for the trace (documented opcodes; others "???").
 * Used only to read code that ran in the emulator; it reads memory with a2_peek. */
#include <stdio.h>
#include "../emu/a2.h"
#include "dis6502.h"

enum { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };
static const char *MN[256];
static unsigned char MODE[256];

static void op(int code, const char *mn, int mode) { MN[code] = mn; MODE[code] = (unsigned char)mode; }

static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    /* group: ORA AND EOR ADC STA LDA CMP SBC with their 8 modes */
    const char *alu[8] = { "ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC" };
    for (int g = 0; g < 8; g++) {
        int b = g << 5;
        op(b | 0x01, alu[g], IZX); op(b | 0x05, alu[g], ZP); if (g != 4) op(b | 0x09, alu[g], IMM);
        op(b | 0x0D, alu[g], ABS); op(b | 0x11, alu[g], IZY); op(b | 0x15, alu[g], ZPX);
        op(b | 0x19, alu[g], ABY); op(b | 0x1D, alu[g], ABX);
    }
    const char *sh[4] = { "ASL", "ROL", "LSR", "ROR" };
    for (int g = 0; g < 4; g++) {
        int b = g << 5;
        op(b | 0x06, sh[g], ZP); op(b | 0x0A, sh[g], ACC); op(b | 0x0E, sh[g], ABS);
        op(b | 0x16, sh[g], ZPX); op(b | 0x1E, sh[g], ABX);
    }
    op(0x86, "STX", ZP); op(0x8E, "STX", ABS); op(0x96, "STX", ZPY);
    op(0xA2, "LDX", IMM); op(0xA6, "LDX", ZP); op(0xAE, "LDX", ABS); op(0xB6, "LDX", ZPY); op(0xBE, "LDX", ABY);
    op(0xC6, "DEC", ZP); op(0xCE, "DEC", ABS); op(0xD6, "DEC", ZPX); op(0xDE, "DEC", ABX);
    op(0xE6, "INC", ZP); op(0xEE, "INC", ABS); op(0xF6, "INC", ZPX); op(0xFE, "INC", ABX);
    op(0x84, "STY", ZP); op(0x8C, "STY", ABS); op(0x94, "STY", ZPX);
    op(0xA0, "LDY", IMM); op(0xA4, "LDY", ZP); op(0xAC, "LDY", ABS); op(0xB4, "LDY", ZPX); op(0xBC, "LDY", ABX);
    op(0xC0, "CPY", IMM); op(0xC4, "CPY", ZP); op(0xCC, "CPY", ABS);
    op(0xE0, "CPX", IMM); op(0xE4, "CPX", ZP); op(0xEC, "CPX", ABS);
    op(0x24, "BIT", ZP); op(0x2C, "BIT", ABS);
    op(0x4C, "JMP", ABS); op(0x6C, "JMP", IND); op(0x20, "JSR", ABS);
    const char *br[8] = { "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ" };
    for (int g = 0; g < 8; g++) op(0x10 | (g << 5), br[g], REL);
    struct { int c; const char *m; } imp[] = {
        { 0x00, "BRK" }, { 0x08, "PHP" }, { 0x18, "CLC" }, { 0x28, "PLP" }, { 0x38, "SEC" }, { 0x40, "RTI" },
        { 0x48, "PHA" }, { 0x58, "CLI" }, { 0x60, "RTS" }, { 0x68, "PLA" }, { 0x78, "SEI" }, { 0x88, "DEY" },
        { 0x8A, "TXA" }, { 0x98, "TYA" }, { 0x9A, "TXS" }, { 0xA8, "TAY" }, { 0xAA, "TAX" }, { 0xB8, "CLV" },
        { 0xBA, "TSX" }, { 0xC8, "INY" }, { 0xCA, "DEX" }, { 0xD8, "CLD" }, { 0xE8, "INX" }, { 0xEA, "NOP" },
        { 0xF8, "SED" } };
    for (unsigned i = 0; i < sizeof imp / sizeof imp[0]; i++) op(imp[i].c, imp[i].m, IMP);
}

int dis6502(const a2 *m, uint16_t pc, char *out, size_t n) {
    init();
    uint8_t c = a2_peek(m, pc), lo = a2_peek(m, (uint16_t)(pc + 1)), hi = a2_peek(m, (uint16_t)(pc + 2));
    uint16_t w = (uint16_t)(lo | hi << 8);
    const char *mn = MN[c] ? MN[c] : "???";
    int len = 1;
    switch (MN[c] ? MODE[c] : IMP) {
    case IMP: snprintf(out, n, "%02X        %s", c, mn); break;
    case ACC: snprintf(out, n, "%02X        %s A", c, mn); break;
    case IMM: snprintf(out, n, "%02X %02X     %s #$%02X", c, lo, mn, lo); len = 2; break;
    case ZP:  snprintf(out, n, "%02X %02X     %s $%02X", c, lo, mn, lo); len = 2; break;
    case ZPX: snprintf(out, n, "%02X %02X     %s $%02X,X", c, lo, mn, lo); len = 2; break;
    case ZPY: snprintf(out, n, "%02X %02X     %s $%02X,Y", c, lo, mn, lo); len = 2; break;
    case IZX: snprintf(out, n, "%02X %02X     %s ($%02X,X)", c, lo, mn, lo); len = 2; break;
    case IZY: snprintf(out, n, "%02X %02X     %s ($%02X),Y", c, lo, mn, lo); len = 2; break;
    case REL: snprintf(out, n, "%02X %02X     %s $%04X", c, lo, mn, (uint16_t)(pc + 2 + (int8_t)lo)); len = 2; break;
    case ABS: snprintf(out, n, "%02X %02X %02X  %s $%04X", c, lo, hi, mn, w); len = 3; break;
    case ABX: snprintf(out, n, "%02X %02X %02X  %s $%04X,X", c, lo, hi, mn, w); len = 3; break;
    case ABY: snprintf(out, n, "%02X %02X %02X  %s $%04X,Y", c, lo, hi, mn, w); len = 3; break;
    case IND: snprintf(out, n, "%02X %02X %02X  %s ($%04X)", c, lo, hi, mn, w); len = 3; break;
    default: break;
    }
    return len;
}
