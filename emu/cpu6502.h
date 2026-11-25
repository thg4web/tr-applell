/* cpu6502.h: NMOS 6502 core (the CPU in an Apple ][ Plus).
 *
 * Portable C99, no allocation, no OS calls: the same file is meant to build on the Mac
 * and in the TeensyROM firmware. Memory goes through the read/write callbacks.
 *
 * Covers every documented opcode, NMOS decimal mode, cycle counts with page-cross and
 * branch penalties, and read-modify-write double writes (soft switches can see them).
 * Undocumented opcodes stop the CPU: `jammed` is set to the opcode and `jam_pc` to its
 * address, so the machine can report it (the ROM fills unwritten space with $02).
 * Not modelled yet: dummy reads on indexed page crosses.
 */
#ifndef CPU6502_H
#define CPU6502_H
#include <stdint.h>

enum { FLAG_C = 0x01, FLAG_Z = 0x02, FLAG_I = 0x04, FLAG_D = 0x08,
       FLAG_B = 0x10, FLAG_U = 0x20, FLAG_V = 0x40, FLAG_N = 0x80 };

typedef struct cpu6502 {
    uint16_t pc;
    uint16_t op_pc;        /* address of the instruction being run (for logging reads) */
    uint8_t a, x, y, sp, p;
    uint64_t cycles;
    int jammed;            /* 0, or opcode+1 of the undocumented opcode that stopped the CPU */
    uint16_t jam_pc;
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t val);
} cpu6502;

void cpu_reset(cpu6502 *c);          /* loads PC from $FFFC */
int  cpu_step(cpu6502 *c);           /* one instruction; returns cycles used (0 if jammed) */
void cpu_irq(cpu6502 *c);            /* ignored while I is set */
void cpu_nmi(cpu6502 *c);

#endif
