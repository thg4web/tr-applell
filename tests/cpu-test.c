/* cpu-test.c: run Klaus Dormann's 6502 functional test (decimal tests on) against our core.
 *
 * Usage: cpu-test [path/to/6502_functional_test.bin] [success-address-hex]
 * Defaults: ../third-party/dormann/6502_functional_test.bin and 3469 (from the .lst).
 * The test loops forever on one address: at the success address it passed; anywhere
 * else is the failing test (look that address up in the .lst).
 */
#include <stdio.h>
#include <stdlib.h>
#include "../emu/cpu6502.h"

static uint8_t mem[65536];
static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "third-party/dormann/6502_functional_test.bin";
    unsigned success = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 16) : 0x3469;
    FILE *f = fopen(path, "rb");
    if (!f || fread(mem, 1, sizeof mem, f) != sizeof mem) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    fclose(f);
    cpu6502 c = { 0 };
    c.read = rd; c.write = wr;
    c.p = FLAG_U | FLAG_I; c.sp = 0xFF; c.pc = 0x0400;
    uint16_t last = 0xFFFF;
    unsigned same = 0;
    while (!c.jammed) {
        uint16_t pc = c.pc;
        cpu_step(&c);
        if (pc == last && c.pc == pc) { if (++same > 3) break; } else same = 0;
        last = pc;
    }
    if (c.jammed) {
        printf("FAIL: undocumented opcode $%02X at $%04X\n", c.jammed - 1, c.jam_pc);
        return 1;
    }
    if (c.pc == success) {
        printf("PASS: 6502 functional test, %llu cycles, ended at $%04X\n", (unsigned long long)c.cycles, c.pc);
        return 0;
    }
    printf("FAIL: stuck at $%04X (A=%02X X=%02X Y=%02X P=%02X SP=%02X), see the .lst\n",
           c.pc, c.a, c.x, c.y, c.p, c.sp);
    return 1;
}
