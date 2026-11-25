/* cpu6502.c: NMOS 6502 core. See cpu6502.h.
 * Sources: MOS MCS6500 Microcomputer Family Programming Manual (instruction set, flags,
 * cycle counts); Bruce Clark, "Decimal Mode" (6502.org tutorial) for NMOS decimal ADC/SBC.
 */
#include "cpu6502.h"

static const uint8_t CYC[256] = {
/*      0 1 2 3 4 5 6 7 8 9 A B C D E F */
/*0*/   7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
/*1*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*2*/   6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
/*3*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*4*/   6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
/*5*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*6*/   6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
/*7*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*8*/   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*9*/   2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
/*A*/   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*B*/   2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
/*C*/   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*D*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*E*/   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*F*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

#define RD(a)     c->read(c->ctx, (uint16_t)(a))
#define WR(a, v)  c->write(c->ctx, (uint16_t)(a), (uint8_t)(v))

static uint8_t fetch(cpu6502 *c) { return RD(c->pc++); }
static uint16_t fetch16(cpu6502 *c) { uint16_t lo = fetch(c); return (uint16_t)(lo | (fetch(c) << 8)); }
static uint16_t zp16(cpu6502 *c, uint8_t z) { return (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8)); }
static void push(cpu6502 *c, uint8_t v) { WR(0x100 | c->sp, v); c->sp--; }
static uint8_t pull(cpu6502 *c) { c->sp++; return RD(0x100 | c->sp); }
static void setnz(cpu6502 *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(FLAG_N | FLAG_Z)) | (v & FLAG_N) | (v ? 0 : FLAG_Z));
}
static void setflag(cpu6502 *c, uint8_t f, int on) { c->p = (uint8_t)(on ? (c->p | f) : (c->p & ~f)); }

void cpu_reset(cpu6502 *c) {
    c->a = c->x = c->y = 0;
    c->sp = 0xFD;
    c->p = FLAG_U | FLAG_I;
    c->pc = (uint16_t)(RD(0xFFFC) | (RD(0xFFFD) << 8));
    c->jammed = 0;
    c->cycles += 7;
}

static void interrupt(cpu6502 *c, uint16_t vector, int brk) {
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p | FLAG_U | (brk ? FLAG_B : 0)) & (brk ? 0xFF : ~FLAG_B)));
    c->p |= FLAG_I;
    c->pc = (uint16_t)(RD(vector) | (RD(vector + 1) << 8));
    c->cycles += 7;
}

void cpu_irq(cpu6502 *c) { if (!(c->p & FLAG_I) && !c->jammed) interrupt(c, 0xFFFE, 0); }
void cpu_nmi(cpu6502 *c) { if (!c->jammed) interrupt(c, 0xFFFA, 0); }

static void adc(cpu6502 *c, uint8_t m) {
    unsigned carry = c->p & FLAG_C;
    if (c->p & FLAG_D) {                         /* NMOS decimal (Clark, sequences 1 and 2) */
        int al = (c->a & 0x0F) + (m & 0x0F) + (int)carry;
        if (al >= 0x0A) al = ((al + 0x06) & 0x0F) + 0x10;
        int sum = (c->a & 0xF0) + (m & 0xF0) + al;
        int ssum = (int8_t)(c->a & 0xF0) + (int8_t)(m & 0xF0) + al;
        setflag(c, FLAG_Z, ((c->a + m + carry) & 0xFF) == 0);
        setflag(c, FLAG_N, ssum & 0x80);
        setflag(c, FLAG_V, ssum < -128 || ssum > 127);
        if (sum >= 0xA0) sum += 0x60;
        setflag(c, FLAG_C, sum >= 0x100);
        c->a = (uint8_t)sum;
    } else {
        unsigned sum = c->a + m + carry;
        setflag(c, FLAG_V, (~(c->a ^ m) & (c->a ^ sum)) & 0x80);
        setflag(c, FLAG_C, sum > 0xFF);
        c->a = (uint8_t)sum;
        setnz(c, c->a);
    }
}

static void sbc(cpu6502 *c, uint8_t m) {
    unsigned borrow = (c->p & FLAG_C) ? 0 : 1;
    unsigned diff = (unsigned)c->a - m - borrow;
    uint8_t bin = (uint8_t)diff;
    /* flags are the binary ones on NMOS, in both modes */
    setflag(c, FLAG_V, ((c->a ^ m) & (c->a ^ bin)) & 0x80);
    setflag(c, FLAG_C, diff < 0x100);
    setnz(c, bin);
    if (c->p & FLAG_D) {                         /* NMOS decimal (Clark, sequence 3) */
        int al = (c->a & 0x0F) - (m & 0x0F) - (int)borrow;
        if (al < 0) al = ((al - 0x06) & 0x0F) - 0x10;
        int r = (c->a & 0xF0) - (m & 0xF0) + al;
        if (r < 0) r -= 0x60;
        c->a = (uint8_t)r;
    } else {
        c->a = bin;
    }
}

static void cmp(cpu6502 *c, uint8_t reg, uint8_t m) {
    setflag(c, FLAG_C, reg >= m);
    setnz(c, (uint8_t)(reg - m));
}

static uint8_t asl(cpu6502 *c, uint8_t v) { setflag(c, FLAG_C, v & 0x80); v <<= 1; setnz(c, v); return v; }
static uint8_t lsr(cpu6502 *c, uint8_t v) { setflag(c, FLAG_C, v & 1); v >>= 1; setnz(c, v); return v; }
static uint8_t rol(cpu6502 *c, uint8_t v) {
    uint8_t r = (uint8_t)((v << 1) | (c->p & FLAG_C)); setflag(c, FLAG_C, v & 0x80); setnz(c, r); return r;
}
static uint8_t ror(cpu6502 *c, uint8_t v) {
    uint8_t r = (uint8_t)((v >> 1) | ((c->p & FLAG_C) << 7)); setflag(c, FLAG_C, v & 1); setnz(c, r); return r;
}

int cpu_step(cpu6502 *c) {
    if (c->jammed) return 0;
    uint16_t at = c->pc;
    c->op_pc = at;
    uint8_t op = fetch(c);
    int cyc = CYC[op];
    uint16_t ea = 0;
    int cross = 0;

    /* Addressing mode, from the opcode's column/row pattern (documented opcodes only). */
    switch (op) {
    /* immediate */
    case 0x09: case 0x29: case 0x49: case 0x69: case 0xA0: case 0xA2: case 0xA9:
    case 0xC0: case 0xC9: case 0xE0: case 0xE9:
        ea = c->pc++; break;
    /* zero page */
    case 0x05: case 0x06: case 0x24: case 0x25: case 0x26: case 0x45: case 0x46: case 0x65: case 0x66:
    case 0x84: case 0x85: case 0x86: case 0xA4: case 0xA5: case 0xA6: case 0xC4: case 0xC5: case 0xC6:
    case 0xE4: case 0xE5: case 0xE6:
        ea = fetch(c); break;
    /* zero page,X */
    case 0x15: case 0x16: case 0x35: case 0x36: case 0x55: case 0x56: case 0x75: case 0x76:
    case 0x94: case 0x95: case 0xB4: case 0xB5: case 0xD5: case 0xD6: case 0xF5: case 0xF6:
        ea = (uint8_t)(fetch(c) + c->x); break;
    /* zero page,Y */
    case 0x96: case 0xB6:
        ea = (uint8_t)(fetch(c) + c->y); break;
    /* absolute */
    case 0x0D: case 0x0E: case 0x20: case 0x2C: case 0x2D: case 0x2E: case 0x4C: case 0x4D: case 0x4E:
    case 0x6D: case 0x6E: case 0x8C: case 0x8D: case 0x8E: case 0xAC: case 0xAD: case 0xAE:
    case 0xCC: case 0xCD: case 0xCE: case 0xEC: case 0xED: case 0xEE:
        ea = fetch16(c); break;
    /* absolute,X */
    case 0x1D: case 0x1E: case 0x3D: case 0x3E: case 0x5D: case 0x5E: case 0x7D: case 0x7E:
    case 0x9D: case 0xBC: case 0xBD: case 0xDD: case 0xDE: case 0xFD: case 0xFE: {
        uint16_t base = fetch16(c); ea = (uint16_t)(base + c->x); cross = (base ^ ea) & 0xFF00; break; }
    /* absolute,Y */
    case 0x19: case 0x39: case 0x59: case 0x79: case 0x99: case 0xB9: case 0xBE: case 0xD9: case 0xF9: {
        uint16_t base = fetch16(c); ea = (uint16_t)(base + c->y); cross = (base ^ ea) & 0xFF00; break; }
    /* (zp,X) */
    case 0x01: case 0x21: case 0x41: case 0x61: case 0x81: case 0xA1: case 0xC1: case 0xE1:
        ea = zp16(c, (uint8_t)(fetch(c) + c->x)); break;
    /* (zp),Y */
    case 0x11: case 0x31: case 0x51: case 0x71: case 0x91: case 0xB1: case 0xD1: case 0xF1: {
        uint16_t base = zp16(c, fetch(c)); ea = (uint16_t)(base + c->y); cross = (base ^ ea) & 0xFF00; break; }
    default: break;
    }

    /* Loads and ALU ops pay one cycle on an indexed page cross; stores and RMW do not. */
    switch (op) {
    case 0x1D: case 0x3D: case 0x5D: case 0x7D: case 0xBC: case 0xBD: case 0xDD: case 0xFD:
    case 0x19: case 0x39: case 0x59: case 0x79: case 0xB9: case 0xBE: case 0xD9: case 0xF9:
    case 0x11: case 0x31: case 0x51: case 0x71: case 0xB1: case 0xD1: case 0xF1:
        if (cross) cyc++;
        break;
    default: break;
    }

    uint8_t v;
    switch (op) {
    /* loads and stores */
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1:
        c->a = RD(ea); setnz(c, c->a); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        c->x = RD(ea); setnz(c, c->x); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        c->y = RD(ea); setnz(c, c->y); break;
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91:
        WR(ea, c->a); break;
    case 0x86: case 0x96: case 0x8E: WR(ea, c->x); break;
    case 0x84: case 0x94: case 0x8C: WR(ea, c->y); break;
    /* ALU */
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11:
        c->a |= RD(ea); setnz(c, c->a); break;
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31:
        c->a &= RD(ea); setnz(c, c->a); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51:
        c->a ^= RD(ea); setnz(c, c->a); break;
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71:
        adc(c, RD(ea)); break;
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1:
        sbc(c, RD(ea)); break;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1:
        cmp(c, c->a, RD(ea)); break;
    case 0xE0: case 0xE4: case 0xEC: cmp(c, c->x, RD(ea)); break;
    case 0xC0: case 0xC4: case 0xCC: cmp(c, c->y, RD(ea)); break;
    case 0x24: case 0x2C:
        v = RD(ea);
        c->p = (uint8_t)((c->p & ~(FLAG_N | FLAG_V | FLAG_Z)) | (v & (FLAG_N | FLAG_V)) | ((c->a & v) ? 0 : FLAG_Z));
        break;
    /* shifts and increments: accumulator */
    case 0x0A: c->a = asl(c, c->a); break;
    case 0x4A: c->a = lsr(c, c->a); break;
    case 0x2A: c->a = rol(c, c->a); break;
    case 0x6A: c->a = ror(c, c->a); break;
    /* read-modify-write on memory: the NMOS part writes the old value back first */
    case 0x06: case 0x16: case 0x0E: case 0x1E: v = RD(ea); WR(ea, v); WR(ea, asl(c, v)); break;
    case 0x46: case 0x56: case 0x4E: case 0x5E: v = RD(ea); WR(ea, v); WR(ea, lsr(c, v)); break;
    case 0x26: case 0x36: case 0x2E: case 0x3E: v = RD(ea); WR(ea, v); WR(ea, rol(c, v)); break;
    case 0x66: case 0x76: case 0x6E: case 0x7E: v = RD(ea); WR(ea, v); WR(ea, ror(c, v)); break;
    case 0xE6: case 0xF6: case 0xEE: case 0xFE:
        v = RD(ea); WR(ea, v); v++; setnz(c, v); WR(ea, v); break;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE:
        v = RD(ea); WR(ea, v); v--; setnz(c, v); WR(ea, v); break;
    /* registers */
    case 0xE8: c->x++; setnz(c, c->x); break;
    case 0xCA: c->x--; setnz(c, c->x); break;
    case 0xC8: c->y++; setnz(c, c->y); break;
    case 0x88: c->y--; setnz(c, c->y); break;
    case 0xAA: c->x = c->a; setnz(c, c->x); break;
    case 0x8A: c->a = c->x; setnz(c, c->a); break;
    case 0xA8: c->y = c->a; setnz(c, c->y); break;
    case 0x98: c->a = c->y; setnz(c, c->a); break;
    case 0xBA: c->x = c->sp; setnz(c, c->x); break;
    case 0x9A: c->sp = c->x; break;
    /* stack */
    case 0x48: push(c, c->a); break;
    case 0x68: c->a = pull(c); setnz(c, c->a); break;
    case 0x08: push(c, c->p | FLAG_B | FLAG_U); break;
    case 0x28: c->p = (uint8_t)((pull(c) & ~FLAG_B) | FLAG_U); break;
    /* flags */
    case 0x18: c->p &= (uint8_t)~FLAG_C; break;
    case 0x38: c->p |= FLAG_C; break;
    case 0x58: c->p &= (uint8_t)~FLAG_I; break;
    case 0x78: c->p |= FLAG_I; break;
    case 0xB8: c->p &= (uint8_t)~FLAG_V; break;
    case 0xD8: c->p &= (uint8_t)~FLAG_D; break;
    case 0xF8: c->p |= FLAG_D; break;
    /* jumps and calls */
    case 0x4C: c->pc = ea; break;
    case 0x6C: {                                 /* JMP (ind), with the NMOS page-wrap quirk */
        uint16_t ptr = fetch16(c);
        uint16_t hi = (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
        c->pc = (uint16_t)(RD(ptr) | (RD(hi) << 8));
        break; }
    case 0x20: {
        uint16_t ret = (uint16_t)(c->pc - 1);
        push(c, (uint8_t)(ret >> 8)); push(c, (uint8_t)ret);
        c->pc = ea; break; }
    case 0x60: { uint16_t lo = pull(c); c->pc = (uint16_t)((lo | (pull(c) << 8)) + 1); break; }
    case 0x40: {
        c->p = (uint8_t)((pull(c) & ~FLAG_B) | FLAG_U);
        uint16_t lo = pull(c); c->pc = (uint16_t)(lo | (pull(c) << 8)); break; }
    case 0x00:                                   /* BRK skips a padding byte */
        c->pc++;
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
        push(c, c->p | FLAG_B | FLAG_U);
        c->p |= FLAG_I;
        c->pc = (uint16_t)(RD(0xFFFE) | (RD(0xFFFF) << 8));
        break;
    /* branches */
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: {
        int8_t off = (int8_t)fetch(c);
        static const uint8_t mask[4] = { FLAG_N, FLAG_V, FLAG_C, FLAG_Z };
        int taken = ((c->p & mask[op >> 6]) != 0) == ((op & 0x20) != 0);
        if (taken) {
            uint16_t dest = (uint16_t)(c->pc + off);
            cyc += ((dest ^ c->pc) & 0xFF00) ? 2 : 1;
            c->pc = dest;
        }
        break; }
    case 0xEA: break;
    default:                                     /* undocumented: stop and report */
        c->jammed = op + 1;
        c->jam_pc = at;
        c->pc = at;
        return 0;
    }
    c->cycles += (uint64_t)cyc;
    return cyc;
}
