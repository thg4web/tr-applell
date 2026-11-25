/* a2.c: 64K Apple ][ Plus machine with a block-device card. See a2.h. */
#include <string.h>
#include "a2.h"

static void log_hit(a2_hit *log, int *n, uint16_t addr, uint16_t from) {
    for (int i = 0; i < *n; i++)
        if (log[i].addr == addr && log[i].from == from) { log[i].count++; return; }
    if (*n < A2_LOG) { log[*n].addr = addr; log[*n].from = from; log[*n].count = 1; (*n)++; }
}

/* ---- language card ($C080-$C08F) ----
 * Address bit 3 picks the $D000 bank (set = bank 1). Bits 0-1: 0 read RAM, 1 read ROM and
 * write RAM, 2 read ROM, 3 read RAM and write RAM. Writing needs two reads of an odd
 * address in a row; an even address or a write access turns writing off.
 */
static void lc_switch(a2 *m, uint16_t addr, int is_read) {
    m->lc_bank1 = (addr & 8) != 0;
    m->lc_read_ram = ((addr & 3) == 0) || ((addr & 3) == 3);
    if (addr & 1) {
        if (is_read) { if (m->lc_prewrite) m->lc_write = 1; m->lc_prewrite = 1; }
        else m->lc_prewrite = 0;
    } else {
        m->lc_write = 0;
        m->lc_prewrite = 0;
    }
}

static uint8_t *lc_cell(a2 *m, uint16_t addr) {
    if (addr < 0xE000) return &m->lc[(m->lc_bank1 ? 0x0000 : 0x1000) + (addr - 0xD000)];
    return &m->lc[0x2000 + (addr - 0xE000)];
}

/* ---- the joystick ($C061-$C067, $C070) ----
 * A real paddle is a 558 timer: an access to $C070 starts all four, and each holds its bit 7 high
 * for a time set by the paddle's resistance. Here paddle n stays high while the cycle count is below
 * trigger + A2_PADDLE_K + 11 x value. The CPU core counts an instruction's cycles after it runs, so
 * every I/O access here sees the cycle count at the START of the instruction that made it.
 * The Monitor's PREAD (ours: rom/monitor.s, $FB1E plus pread_rest in the extension ROM):
 *     LDA PTRIG (4)  JMP pread_rest (3)  LDY #0 (2)  loop: LDA PADDL0,X (4) BPL (2) INY (2) BNE (3)
 * so with T = the start of LDA PTRIG, read i starts at T + 9 + 11i and PREAD returns the first i
 * whose bit 7 is clear. To return v it needs T + 9 + 11(v-1) < T + K + 11v <= T + 9 + 11v, so
 * -1 <= K <= 9. Apple's own PREAD has LDY #0, NOP, NOP (6 cycles) where ours has JMP, LDY (5): its
 * reads start at T + 10 + 11i, so 0 <= K <= 10. K = 4 sits in the middle of both (reads 5 or 6
 * cycles either side of the edge). Checked with bc for v = 255: read 254 at 9 + 11 x 254 = 2803 is
 * below 4 + 11 x 255 = 2809, read 255 at 2814 is not; for v = 0 the first read, at 9, is not below 4.
 * Simplified: no recovery time (a real timer must discharge before the next trigger; here a trigger
 * restarts all four at once), and the bits other than bit 7 read 0 instead of the floating bus.
 */
#define A2_PADDLE_K 4

static uint8_t paddle_bit(const a2 *m, int n) {
    return m->paddle_started && m->cpu.cycles < m->paddle_trigger + A2_PADDLE_K + 11u * m->paddle[n] ? 0x80 : 0;
}

void a2_set_paddle(a2 *m, int n, uint8_t value) { if (n >= 0 && n < 4) m->paddle[n] = value; }
void a2_set_button(a2 *m, int n, int pressed) { if (n >= 0 && n < 3) m->button[n] = pressed ? 1 : 0; }

/* ---- I/O page $C000-$C0FF: any access (read or write) flips a soft switch ---- */
static uint8_t io(a2 *m, uint16_t addr, int is_read) {
    switch (addr & 0xFFF0) {
    case 0xC000:
        if (is_read) { m->key_read_pc = m->cpu.op_pc; m->key_read_lc = m->lc_read_ram; m->key_read_cycle = m->cpu.cycles; }
        return m->key;
    case 0xC010:
#ifndef A2_SMALL
        if (m->stop_on_keyread && (m->key & 0x80) && !m->stopped) { m->stopped = 7; m->stop_pc = m->cpu.op_pc; m->stop_caller = 0; }
#endif
        m->key &= 0x7F; return m->key;
    case 0xC030: m->speaker_clicks++; if (m->speaker_hook) m->speaker_hook(m->speaker_ctx, m->cpu.cycles); return 0;
    case 0xC050:
        switch (addr & 0x0F) {
        case 0x0: m->text = 0; break;   case 0x1: m->text = 1; break;
        case 0x2: m->mixed = 0; break;  case 0x3: m->mixed = 1; break;
        case 0x4: m->page2 = 0; break;  case 0x5: m->page2 = 1; break;
        case 0x6: m->hires = 0; break;  case 0x7: m->hires = 1; break;
        default: break;                 /* $C058-$C05F annunciators: nothing attached */
        }
        return 0;
    case 0xC060:                        /* cassette in (nothing attached), buttons, paddles; $C068-$C06F repeat them */
#ifndef A2_SMALL
        m->game_io[addr & 0x1F]++;
#endif
        switch (addr & 7) {
        case 1: case 2: case 3: return m->button[(addr & 7) - 1] ? 0x80 : 0;
        case 4: case 5: case 6: case 7: return paddle_bit(m, addr & 3);
        default: return 0;
        }
    case 0xC070:                        /* paddle timer trigger */
#ifndef A2_SMALL
        m->game_io[addr & 0x1F]++;
#endif
        m->paddle_trigger = m->cpu.cycles; m->paddle_started = 1;
        return 0;
    case 0xC080: lc_switch(m, addr, is_read); return 0;
    default: return 0;                  /* slot device I/O: our card needs none */
    }
}

static uint8_t bus_read(void *ctx, uint16_t addr) {
    a2 *m = (a2 *)ctx;
    if (addr < 0xC000) return m->ram[addr];
    if (addr < 0xC100) return io(m, addr, 1);
    if (addr < 0xC800) {
        if (m->card_slot && (addr >> 8) == (0xC0 | m->card_slot)) return m->card_rom[addr & 0xFF];
        return 0;                       /* empty slot */
    }
    if (addr < 0xD000) return m->has_ext ? m->ext_rom[addr - 0xC800] : 0;
    if (m->lc_read_ram) return *lc_cell(m, addr);
#ifndef A2_SMALL
    if (m->rom_gap[addr - 0xD000] && (uint16_t)(addr - m->cpu.op_pc) >= 3)
    {   log_hit(m->peeks, &m->npeeks, addr, m->cpu.op_pc);     /* data read of an unwritten byte */
        if (m->stop_on_peek && !m->stopped && a2_peek(m, m->cpu.op_pc) != 0x2C) {   /* not BIT abs: a skip trick */ m->stopped = 6; m->stop_pc = m->cpu.op_pc; m->stop_caller = addr; }
    }
#endif
    return m->rom[addr - 0xD000];
}

static void bus_write(void *ctx, uint16_t addr, uint8_t v) {
    a2 *m = (a2 *)ctx;
    if (addr < 0xC000) { m->ram[addr] = v; return; }
    if (addr < 0xC100) { io(m, addr, 0); return; }
    if (addr >= 0xD000 && m->lc_write) *lc_cell(m, addr) = v;
}

/* ---- the block-device card ---- */

/* Pop the return address a JSR left and continue after the JSR plus `skip` inline bytes. */
static void return_from_trap(a2 *m, int skip) {
    cpu6502 *c = &m->cpu;
    uint16_t lo = m->ram[0x100 | (uint8_t)(c->sp + 1)], hi = m->ram[0x100 | (uint8_t)(c->sp + 2)];
    c->sp = (uint8_t)(c->sp + 2);
    c->pc = (uint16_t)(((hi << 8) | lo) + 1 + skip);
    c->jammed = 0;
}

static void set_result(a2 *m, uint8_t err) {
    cpu6502 *c = &m->cpu;
    c->a = err;
    c->p = (uint8_t)((c->p & ~(FLAG_C | FLAG_Z | FLAG_N)) | (err ? FLAG_C : 0) | (err ? 0 : FLAG_Z) | (err & FLAG_N));
}

/* Read or write one 512-byte block between the disk and Apple memory. Returns an error code. */
static uint8_t block_io(a2 *m, int write, uint32_t block, uint16_t buf) {
    if (block >= (uint32_t)m->disk_blocks) { m->disk_errors++; return 0x27; }
    uint8_t tmp[512];
    uint8_t *data = m->disk ? m->disk + (size_t)block * 512 : tmp;
    if (write) {
        m->disk_writes++;
        for (int i = 0; i < 512; i++) data[i] = bus_read(m, (uint16_t)(buf + i));
        if (!m->disk) {                 /* a device: the write goes to it */
            uint8_t e = (uint8_t)m->disk_io(m->disk_ctx, 1, block, tmp);
            if (e) m->disk_errors++;
            return e;
        }
        return 0;
    }
    m->disk_reads++;
    if (m->nread_order < A2_READ_LOG) m->read_order[m->nread_order++] = (uint16_t)block;
    if (m->disk_bad && block < (uint32_t)m->disk_bad_n && m->disk_bad[block]) {
        m->disk_errors++;
        if (m->nbad_reads < 64) m->bad_reads[m->nbad_reads++] = (uint16_t)block;
        return 0x27;                    /* I/O error: the drive cannot read this block */
    }
    if (!m->disk) {
        uint8_t e = (uint8_t)m->disk_io(m->disk_ctx, 0, block, tmp);
        if (e) { m->disk_errors++; return e; }
    }
    for (int i = 0; i < 512; i++) bus_write(m, (uint16_t)(buf + i), data[i]);
    return 0;
}

/* ProDOS block call: $42 command, $43 unit, $44/$45 buffer, $46/$47 block. */
static void prodos_call(a2 *m) {
    uint8_t cmd = m->ram[0x42], unit = m->ram[0x43];
    uint16_t buf = (uint16_t)(m->ram[0x44] | m->ram[0x45] << 8);
    uint32_t block = (uint32_t)(m->ram[0x46] | m->ram[0x47] << 8);
    uint8_t err = 0;
    if (unit & 0x80) err = 0x28;        /* drive 2: no device */
    else if (cmd == 0) { m->disk_status++; m->cpu.x = (uint8_t)m->disk_blocks; m->cpu.y = (uint8_t)(m->disk_blocks >> 8); }
    else if (cmd == 1) err = block_io(m, 0, block, buf);
    else if (cmd == 2) err = block_io(m, 1, block, buf);
    else if (cmd == 3) m->disk_other++;
    else { m->disk_other++; err = 0x27; }
    set_result(m, err);
    return_from_trap(m, 0);
}

/* SmartPort call: JSR entry / .byte command / .word parameter list. */
static void smartport_call(a2 *m) {
    cpu6502 *c = &m->cpu;
    uint16_t ret = (uint16_t)(m->ram[0x100 | (uint8_t)(c->sp + 1)] | m->ram[0x100 | (uint8_t)(c->sp + 2)] << 8);
    uint8_t cmd = bus_read(m, (uint16_t)(ret + 1));
    uint16_t pl = (uint16_t)(bus_read(m, (uint16_t)(ret + 2)) | bus_read(m, (uint16_t)(ret + 3)) << 8);
    uint8_t unit = bus_read(m, (uint16_t)(pl + 1));
    uint16_t buf = (uint16_t)(bus_read(m, (uint16_t)(pl + 2)) | bus_read(m, (uint16_t)(pl + 3)) << 8);
    uint8_t err = 0;
    m->sp_calls[cmd & 0x0F]++;
    if (cmd == 0x00) {                  /* STATUS */
        uint8_t code = bus_read(m, (uint16_t)(pl + 4));
        m->disk_status++;
        if (unit == 0 && code == 0) {   /* the SmartPort itself: one device */
            bus_write(m, buf, 1);
            for (int i = 1; i < 8; i++) bus_write(m, (uint16_t)(buf + i), 0);
            c->x = 8; c->y = 0;
        } else if (unit == 1 && (code == 0 || code == 3)) {
            uint8_t st[25] = { 0xF8, (uint8_t)m->disk_blocks, (uint8_t)(m->disk_blocks >> 8), 0 };
            int n = 4;
            if (code == 3) {            /* device information block */
                const char *name = "TR 3.5 DISK";
                st[4] = (uint8_t)strlen(name);
                for (int i = 0; i < 16; i++) st[5 + i] = (uint8_t)(name[i] && i < st[4] ? name[i] : ' ');
                st[21] = 0x01; st[22] = 0x00; st[23] = 0x00; st[24] = 0x01;
                n = 25;
            }
            for (int i = 0; i < n; i++) bus_write(m, (uint16_t)(buf + i), st[i]);
            c->x = (uint8_t)n; c->y = 0;
        } else err = unit > 1 ? 0x28 : 0x21;
    } else if (cmd == 0x01 || cmd == 0x02) {   /* READ BLOCK / WRITE BLOCK */
        uint32_t block = (uint32_t)(bus_read(m, (uint16_t)(pl + 4)) | bus_read(m, (uint16_t)(pl + 5)) << 8
                                    | bus_read(m, (uint16_t)(pl + 6)) << 16);
        err = unit == 1 ? block_io(m, cmd == 0x02, block, buf) : 0x28;
        c->x = 0; c->y = 2;
    } else if (cmd == 0x03 || cmd == 0x04 || cmd == 0x05) {   /* FORMAT, CONTROL, INIT */
        m->disk_other++;
    } else {
        m->disk_other++;
        err = 0x01;                     /* bad command */
    }
    set_result(m, err);
    return_from_trap(m, 3);
}

/* ---- machine ---- */

void a2_reset(a2 *m) {
    /* The language card goes back to: read ROM, write RAM, bank 2. */
    m->lc_read_ram = 0; m->lc_write = 1; m->lc_bank1 = 0; m->lc_prewrite = 0;
    m->stopped = 0;
    cpu_reset(&m->cpu);
}

void a2_init(a2 *m, const uint8_t *rom12k) {
    memset(m, 0, sizeof *m);
    memcpy(m->rom, rom12k, sizeof m->rom);
    /* A fill byte is a $02 with a $02 next to it; a lone $02 is real code or data. */
#ifndef A2_SMALL
    for (int i = 0; i < 0x3000; i++)
        m->rom_gap[i] = rom12k[i] == 0x02 && ((i > 0 && rom12k[i - 1] == 0x02) || (i < 0x2FFF && rom12k[i + 1] == 0x02));
#endif
    m->text = 1;
    m->stop_at = -1;
    m->cpu.ctx = m;
    m->cpu.read = bus_read;
    m->cpu.write = bus_write;
    a2_reset(m);
}

void a2_load_ext(a2 *m, const uint8_t *ext2k) { memcpy(m->ext_rom, ext2k, sizeof m->ext_rom); m->has_ext = 1; }

void a2_attach_disk(a2 *m, int slot, const uint8_t *card_rom256, uint8_t *blocks, int nblocks, const uint8_t *bad) {
    m->card_slot = slot;
    memcpy(m->card_rom, card_rom256, 256);
    m->card_rom[0xFC] = (uint8_t)nblocks;          /* total blocks, read by ProDOS-style code */
    m->card_rom[0xFD] = (uint8_t)(nblocks >> 8);
    m->disk = blocks;
    m->disk_blocks = nblocks;
    m->disk_bad = bad;
    m->disk_bad_n = bad ? nblocks : 0;
}

void a2_attach_blockdev(a2 *m, int slot, const uint8_t *card_rom256, int nblocks, const uint8_t *bad, int nbad,
                        int (*io)(void *ctx, int write, uint32_t block, uint8_t *buf), void *ctx) {
    a2_attach_disk(m, slot, card_rom256, NULL, nblocks, bad);
    m->disk_bad_n = bad ? nbad : 0;
    m->disk_io = io;
    m->disk_ctx = ctx;
}

uint8_t a2_peek(const a2 *m, uint16_t addr) {
    if (addr < 0xC000) return m->ram[addr];
    if (addr < 0xC100) return addr < 0xC010 ? m->key : 0;
    if (addr < 0xC800) return (m->card_slot && (addr >> 8) == (0xC0 | m->card_slot)) ? m->card_rom[addr & 0xFF] : 0;
    if (addr < 0xD000) return m->has_ext ? m->ext_rom[addr - 0xC800] : 0;
    if (m->lc_read_ram) {
        if (addr < 0xE000) return m->lc[(m->lc_bank1 ? 0x0000 : 0x1000) + (addr - 0xD000)];
        return m->lc[0x2000 + (addr - 0xE000)];
    }
    return m->rom[addr - 0xD000];
}

int a2_run(a2 *m, long cycles) {
    uint64_t end = m->cpu.cycles + (uint64_t)cycles;
    while (!m->stopped && m->cpu.cycles < end) {
        if (m->trace_on) {
            a2_step *s = &m->trace[m->trace_n++ % A2_TRACE];
            s->pc = m->cpu.pc; s->a = m->cpu.a; s->x = m->cpu.x; s->y = m->cpu.y; s->p = m->cpu.p; s->sp = m->cpu.sp;
        }
        if (m->stop_at >= 0 && m->cpu.pc == (uint16_t)m->stop_at) {
            m->stopped = 4; m->stop_pc = m->cpu.pc; m->stop_caller = 0;
            break;
        }
        if (m->stop_on_brk && a2_peek(m, m->cpu.pc) == 0x00) {
            m->stopped = 3; m->stop_pc = m->cpu.pc; m->stop_caller = 0;
            break;
        }
#ifndef A2_SMALL
        if (m->cout_hook && m->cpu.pc == 0xFDED) m->cout_hook(m->cpu.a);
#endif
        uint16_t before = m->cpu.pc;
        int stepped = cpu_step(&m->cpu);
#ifndef A2_SMALL
        if (m->log_romcalls && before >= 0x0100 && before < 0xC000 && m->cpu.pc >= 0xD000 && !m->lc_read_ram) {   /* zero page: BASIC's own JMPs */
            uint8_t op = m->ram[before];          /* a JSR or JMP from RAM into our ROM */
            if (op == 0x20 || op == 0x4C || op == 0x6C) {
                log_hit(m->romcalls, &m->nromcalls, m->cpu.pc, before);
                if (m->stop_unpinned && m->cpu.pc < 0xF800 && !(m->known && m->known[m->cpu.pc - 0xD000])) {
                    m->stopped = 5; m->stop_pc = m->cpu.pc; m->stop_caller = before;
                    break;
                }
            }
        }
#else
        (void)before;
#endif
        if (stepped != 0 || !m->cpu.jammed) continue;
        uint16_t pc = m->cpu.jam_pc;
        uint8_t op = (uint8_t)(m->cpu.jammed - 1);
        if (m->card_slot && (pc >> 8) == (0xC0 | m->card_slot) && (m->disk || m->disk_io)) {
            if (op == 0x12) { prodos_call(m); m->cpu.cycles += 400; continue; }
            if (op == 0x22) { smartport_call(m); m->cpu.cycles += 400; continue; }
        }
        uint8_t sp = m->cpu.sp;
        uint16_t ret = (uint16_t)(m->ram[0x100 | (uint8_t)(sp + 1)] | (m->ram[0x100 | (uint8_t)(sp + 2)] << 8));
        uint16_t caller = (uint16_t)(ret - 2);
        int missing = op == 0x02 && pc >= 0xD000 && !m->lc_read_ram;
        if (missing) log_hit(m->missing, &m->nmissing, pc, caller);
        if (missing && m->stub_missing) { return_from_trap(m, 0); m->cpu.cycles += 6; continue; }
        m->stopped = missing ? 1 : 2;
        m->stop_pc = pc;
        m->stop_caller = caller;
    }
    return m->stopped;
}

void a2_key(a2 *m, uint8_t ascii) { m->key = (uint8_t)(ascii | 0x80); }
int a2_key_waiting(const a2 *m) { return (m->key & 0x80) != 0; }

uint16_t a2_text_row(const a2 *m, int row) {
    /* Apple text rows are interleaved: $400 + (row mod 8) x $80 + (row div 8) x $28 */
    return (uint16_t)((m->page2 ? 0x800 : 0x400) + (row & 7) * 0x80 + (row >> 3) * 0x28);
}

uint16_t a2_hires_row(const a2 *m, int y) {
    /* Hi-res lines: $2000 + (y mod 8) x $400 + ((y div 8) mod 8) x $80 + (y div 64) x $28 */
    return (uint16_t)((m->page2 ? 0x4000 : 0x2000) + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28);
}
