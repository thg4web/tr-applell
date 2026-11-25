/* a2.h: a 64K Apple ][ Plus (48K RAM + 16K language card) around the 6502 core,
 * with an optional 3.5-inch block-device card.
 *
 * Portable C99, no OS calls, like cpu6502.c: the Mac front end and (later) the TeensyROM
 * firmware both drive it through these calls.
 *
 * Hardware facts come from the Apple II Reference Manual (memory map, $C0xx soft switches,
 * keyboard at $C000/$C010, speaker $C030) and the Language Card manual ($C080-$C08F).
 * ROM ($D000-$FFFF, 12K) is whatever image is loaded: ours. Our Monitor also keeps its
 * routine bodies in a 2K extension ROM at $C800-$CFFF, a deliberate departure from a
 * stock ][ Plus (that space belongs to peripheral cards; our emulated card needs none).
 *
 * Block-device card: the card's 256-byte firmware (ours, rom/blockcard.s) follows the
 * ProDOS 8 Technical Reference conventions ($Cn01/$Cn03/$Cn05 = $20/$00/$03, $Cn07 = $00
 * for SmartPort, $CnFC-$CnFF). Its driver entries hold trap opcodes: $12 = ProDOS block
 * call (zero page $42-$47), $22 = SmartPort call (inline command + parameter list). The
 * machine does the transfer itself and returns. With a2_attach_disk (the Mac) changes stay in
 * memory and the image file is never written; with a2_attach_blockdev the caller's io does the
 * writes (the TeensyROM module writes them into the image on the SD card).
 *
 * Joystick (J1): two paddles' worth of game I/O, as the Reference Manual describes it. Buttons 0-2
 * on bit 7 of $C061-$C063; four paddle timers started by any access to $C070-$C07F, each paddle's
 * bit 7 ($C064-$C067) high while its timer runs. The timer length is set so the Monitor's PREAD
 * counts exactly the value given to a2_set_paddle (see a2.c for the arithmetic).
 *
 * Logs, for the ROM to-do list:
 *  - missing routines: our ROM fills unwritten bytes with $02; running one stops the
 *    machine (or, with stub_missing, logs it, fakes an RTS and carries on);
 *  - ROM peeks: data reads of unwritten ROM bytes, with the reading instruction;
 *  - disk calls: every read/write/status, and which blocks failed.
 */
#ifndef A2_H
#define A2_H
#include <stdint.h>
#include "cpu6502.h"

#define A2_CYCLES_PER_FRAME 17030       /* 65 cycles x 262 lines, NTSC */
#define A2_LOG 64

typedef struct a2_hit { uint16_t addr, from; uint32_t count; } a2_hit;
typedef struct a2_step { uint16_t pc; uint8_t a, x, y, p, sp; } a2_step;
/* A2_SMALL: the TeensyROM module build. No instruction trace, no ROM-gap map, a short
   read log: about 80K instead of 160K. Define it before including a2.h AND a2.c
   (the module includes both in one file, so the layout cannot disagree). */
#ifdef A2_SMALL
#define A2_TRACE 1
#define A2_GAP_BYTES 1
#define A2_READ_LOG 16
#else
#define A2_TRACE 8192
#define A2_GAP_BYTES 0x3000
#define A2_READ_LOG 4096
#endif

typedef struct a2 {
    cpu6502 cpu;
    uint8_t ram[0xC000];
    uint8_t lc[0x4000];                 /* language card: $D000 bank 1, $D000 bank 2, $E000-$FFFF */
    uint8_t rom[0x3000];                /* $D000-$FFFF */
    uint8_t rom_gap[A2_GAP_BYTES];      /* 1 = a fill byte our ROM never wrote (not in A2_SMALL) */
    uint8_t ext_rom[0x800];             /* our extension ROM at $C800-$CFFF (see rom/monitor.s) */
    int has_ext;
    uint8_t key;                        /* $C000: bit 7 = a key is waiting */
    uint8_t text, mixed, page2, hires;  /* display soft switches */
    uint8_t lc_read_ram, lc_write, lc_bank1, lc_prewrite;
    uint64_t speaker_clicks;
    /* each click ($C030, read or write), with the cycle count at the start of the instruction that
       made it: the sound path (Mac: mac/speaker.c; the module's tone tracker later). NULL = none. */
    void (*speaker_hook)(void *ctx, uint64_t cycle);
    void *speaker_ctx;
    uint32_t game_io[32];               /* accesses to $C060-$C07F: tape in, buttons, paddles, trigger (not in A2_SMALL) */
    /* joystick: a2_set_paddle / a2_set_button set these; the timers run from the last trigger */
    uint8_t paddle[4];                  /* 0-255: what PREAD (and BASIC's PDL) will count */
    uint8_t button[3];                  /* 1 = pressed */
    uint64_t paddle_trigger;            /* cycle count of the last $C070-$C07F access */
    int paddle_started;                 /* 0 until the first trigger: every timer reads as run out */
    /* the last read of the keyboard ($C000-$C00F): which instruction, whether language-card RAM was
       switched in for reading, and when. Recorded only; it changes nothing (emu/stick.c uses it to
       tell Oregon Trail's hunt from its other prompts). */
    uint16_t key_read_pc;
    uint8_t key_read_lc;
    uint64_t key_read_cycle;
    /* block-device card */
    int card_slot;                      /* 0 = no card */
    uint8_t card_rom[256];
    uint8_t *disk;                      /* nblocks x 512 bytes, owned by the caller */
    const uint8_t *disk_bad;            /* 1 = unreadable block, or NULL */
    int disk_bad_n;                     /* entries in disk_bad: blocks past them read normally */
    int disk_blocks;
    /* or, with disk == NULL, the block device itself (the TeensyROM module's file on the SD card):
       returns 0 or a ProDOS error code; buf holds the 512 bytes. */
    int (*disk_io)(void *ctx, int write, uint32_t block, uint8_t *buf);
    void *disk_ctx;
    /* why the machine stopped: 0 running, 1 missing ROM routine, 2 other undocumented opcode,
       3 a BRK about to run (only with stop_on_brk), 4 reached stop_at,
       5 a call from RAM into BASIC's space at an address with no pinned JMP (only with stop_unpinned),
       6 just after a data read of an unwritten ROM byte (only with stop_on_peek; not in A2_SMALL),
       7 just after the strobe of a waiting key is cleared at $C010 (only with stop_on_keyread; not in A2_SMALL) */
    int stopped;
    uint16_t stop_pc, stop_caller;
    int stub_missing, stop_on_brk, stop_on_peek, stop_on_keyread;
    int stop_at;                        /* -1, or stop (4) before running this address */
    /* instruction trace (ring buffer), for debugging */
    int trace_on, trace_n;
    a2_step trace[A2_TRACE];
    /* logs */
    a2_hit missing[A2_LOG]; int nmissing;
    a2_hit peeks[A2_LOG];   int npeeks;
    int log_romcalls;                   /* 1 = log JSR/JMP from RAM into $D000-$FFFF ROM (not in A2_SMALL) */
    int stop_unpinned;                  /* 1 = stop (5) on a JSR/JMP from RAM into $D000-$F7FF not listed in known[] */
    const uint8_t *known;               /* 0x2800 flags for $D000-$F7FF: 1 = an Applesoft entry point we answer at */
    a2_hit romcalls[A2_LOG]; int nromcalls;
    void (*cout_hook)(uint8_t a);        /* called with A whenever the CPU reaches COUT ($FDED); Mac only */
    uint32_t disk_reads, disk_writes, disk_status, disk_errors, disk_other;
    uint32_t sp_calls[16];              /* SmartPort calls by command number */
    uint16_t read_order[A2_READ_LOG]; int nread_order;   /* blocks in the order read */
    uint16_t bad_reads[64]; int nbad_reads;
} a2;

void a2_init(a2 *m, const uint8_t *rom12k);    /* power on: RAM cleared, CPU reset */
void a2_load_ext(a2 *m, const uint8_t *ext2k);  /* map our 2K extension ROM at $C800 */
void a2_attach_disk(a2 *m, int slot, const uint8_t *card_rom256,
                    uint8_t *blocks, int nblocks, const uint8_t *bad);
/* The same card, its blocks from io(ctx, write, block, buf) instead of memory; bad has nbad entries. */
void a2_attach_blockdev(a2 *m, int slot, const uint8_t *card_rom256, int nblocks, const uint8_t *bad, int nbad,
                        int (*io)(void *ctx, int write, uint32_t block, uint8_t *buf), void *ctx);
void a2_reset(a2 *m);                          /* the Reset key */
int  a2_run(a2 *m, long cycles);               /* run about this many cycles; returns m->stopped */
void a2_key(a2 *m, uint8_t ascii);             /* a key press, plain ASCII */
int  a2_key_waiting(const a2 *m);
uint16_t a2_text_row(const a2 *m, int row);    /* address of a text row on the shown page */
uint16_t a2_hires_row(const a2 *m, int y);     /* address of a hi-res line on the shown page */
uint8_t a2_peek(const a2 *m, uint16_t addr);   /* what the CPU would read, with no side effects */
void a2_set_paddle(a2 *m, int n, uint8_t value);   /* paddle 0-3: PREAD counts value (0-255) */
void a2_set_button(a2 *m, int n, int pressed);     /* button 0-2 */

#endif
