/* a2term.c: run the TR ][ (Apple ][ Plus) emulator in a Mac terminal.
 *
 * Usage:
 *   a2term [options]                 interactive: the 40x24 text screen in the terminal
 *   a2term --headless [options]      run, then print the screen and the logs
 * Options:
 *   --rom FILE       12K ROM image for $D000-$FFFF (default build/tr-rom.bin)
 *   --ext FILE       2K extension ROM for $C800-$CFFF (default build/tr-ext.bin)
 *   --disk FILE      a 3.5-inch WOZ 2 image, or a disk image emu/diskimg.c knows (.dsk .do .po .2mg: 140K
 *                    5.25-inch in DOS or ProDOS order, or whole 512-byte blocks), in slot 5 as blocks. Beside
 *                    it, FILE.bad (the extension changed to bad) lists unreadable blocks (emu/badlist.h).
 *                    Read into memory: the file itself is never written.
 *   --card FILE      the slot card's 256-byte firmware (default build/blockcard.bin)
 *   --stub-missing   when a missing ROM routine is called, log it, fake an RTS, carry on
 *   --keys TEXT      (headless) type TEXT: \r = Return, \e = Esc, \xNN = any byte,
 *                    \w = wait one second of Apple time; the joystick, in order with the keys:
 *                    \pNHH = paddle N (0-3) to hex HH, \dN = button N (0-2) down, \uN = up;
 *                    \sHH = the C64 stick as the client sends it (hex, bits 3-7: 08 up, 10 down,
 *                    20 left, 40 right, 80 fire; 00 centre), through emu/stick.c as in the module:
 *                    paddles 0/1 and button 0, and Oregon Trail's hunt keys while the hunt reads keys
 *   --paddle N=V     the joystick before the run: paddle N (0-3) at V (0-255); repeatable
 *   --button N       button N (0-2) held down from the start; repeatable
 *   --seconds N      (headless) Apple seconds to run after the last key (default 1)
 *   --png FILE       (headless) also save the hi-res screen as a PNG
 *   --trace N        (headless) print the last N instructions run (up to 8192), disassembled
 *   --stop-on-brk    stop just before any BRK runs (handy with --trace)
 *   --stop-at ADDR   stop just before the instruction at hex ADDR runs
 *   --stop-on-peek   stop just after an instruction reads an unwritten ROM byte (handy with --trace)
 *   --stop-on-key-read  stop just after an instruction takes a waiting key (clears the strobe at $C010)
 *   --arm-after-keys (headless) --stop-at, --stop-on-peek and --stop-on-key-read count only once every
 *                    key has been typed
 *   --rom-calls      (headless) list every JSR/JMP from RAM into $D000-$FFFF ROM: target, caller, count
 *   --stop-unpinned  (headless) as --rom-calls, and stop at a call into $D000-$F7FF that lands on no pinned JMP
 *   --dump A-B       (headless) print memory from hex A to hex B at the end
 *   --bas FILE       (headless) with \b in --keys: at that point load FILE (an Applesoft program as
 *                    saved, from $0801) and set the pointers and line links the way DOS's LOAD does
 *   --cout-log FILE  (headless) write every character sent through COUT ($FDED) to FILE
 *   --wav FILE       (headless) save what the Apple's speaker played as a 16-bit mono WAV (22050 Hz)
 *   --sound-report   (headless) list the sound events and how much of the sound is steady tones
 *                    (mac/speaker.c). The report always ends with the game I/O reads ($C060-$C07F).
 *                    It ends with the tone tracker's summary (emu/tone.c) and its check at S1's frame.
 *   --sid-wav FILE   (headless) save what SID voice 1 would play from the tone tracker (mac/sid-voice.c):
 *                    the same rate, length and level as --wav, to compare by ear
 *   --tone-log FILE  (headless) the tone tracker's changes, one line each: what an S3 sound packet carries
 *   --tone-frame N[/D]  the tracker's frame in Apple cycles (default 17050, the module's 60 Hz scan)
 *   --click-log FILE (headless) every speaker click's Apple cycle count, one a line (for looking into a frame)
 *   --rom-patch ADDR:HEX  probe only: overwrite ROM bytes in memory for this run, for example
 *                    E000:4C00E0 (repeatable; the ROM files are not changed)
 * Interactive keys: Return; Delete or Left arrow = Apple left arrow; Right arrow = Apple
 * right arrow; Esc; Ctrl-letter as on the Apple. Ctrl-\ is the Reset key. Ctrl-] quits.
 * Exit status (headless): 0 ran clean, 1 stopped (missing ROM routine or bad opcode).
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../emu/a2.h"
#include "../emu/woz.h"
#include "../emu/badlist.h"
#include "dis6502.h"
#include "speaker.h"
#include "sid-voice.h"
#include "../emu/tone.h"
#include "../emu/stick.h"
#include "../emu/diskimg.h"

static a2 M;
static int trace_show;
static woz_disk WD;
#define KEYQ 65536                         /* room for a typed-in program listing */
static uint16_t queue[KEYQ];            /* key codes; 0x100 = wait one second, 0x101 = --bas,
                                           0x1000 | N<<8 | V = paddle N to V, 0x1400 | N / 0x1500 | N = button N down / up,
                                           0x1600 | bits = the C64 stick (emu/stick.c) */
static uint8_t stick_last;              /* the stick bits applied last */
static uint8_t inject[2]; static int ninject;   /* hunt keys the stick typed, fed before the next queued key */
static int qhead, qtail;
static const char *disk_note = "";
static spk_log SPK;                     /* every speaker click, for --wav and --sound-report */
static const char *wav_path;
static int sound_report;
static const char *sid_wav_path, *tone_log_path, *click_log_path;
static uint32_t tone_fnum = TONE_FRAME_NUM, tone_fden = TONE_FRAME_DEN;

/* ---- the tone tracker over the click log (S2): the same streaming calls the module will make ---- */
typedef struct tone_run { tone_frame *f; size_t n, cap; } tone_run;
static void tone_collect(void *ctx, const tone_frame *fr) {
    tone_run *R = (tone_run *)ctx;
    if (R->n == R->cap) {
        size_t cap = R->cap ? R->cap * 2 : 65536;
        tone_frame *g = realloc(R->f, cap * sizeof *g);
        if (!g) return;
        R->f = g; R->cap = cap;
    }
    R->f[R->n++] = *fr;
}
static void tone_track(tone_run *R, uint32_t num, uint32_t den, uint64_t end) {
    static tone_tracker T;
    R->n = 0;
    tone_init(&T, num, den, TONE_CLOCK_NTSC_NUM, TONE_CLOCK_NTSC_DEN, tone_collect, R);
    for (size_t i = 0; i < SPK.n; i++) tone_click(&T, SPK.t[i]);
    tone_advance(&T, end);
}
static const double C64_HZ = SPK_APPLE_HZ * TONE_CLOCK_NTSC_DEN / TONE_CLOCK_NTSC_NUM;   /* 1,022,727 */
static double fn_hz(unsigned fn) { return fn * C64_HZ / 16777216.0; }
static int pulse_kind(int k) { return k == TONE_STEADY || k == TONE_SPLIT || k == TONE_SLOW; }
static const char *kind_name(int k) {
    static const char *n[] = { "silent", "steady", "split", "slow", "irregular", "sparse" };
    return k >= 0 && k <= 5 ? n[k] : "?";
}
typedef struct tone_stats {
    long kind[6], on, on_pulse, on_noise, octave, too_high, carried, packets, max60, notes, played;
    double hz_min, hz_max; unsigned fn_min, fn_max;
} tone_stats;
static void tone_stats_of(const tone_run *R, tone_stats *S) {
    memset(S, 0, sizeof *S);
    unsigned char win[60] = { 0 };
    long inwin = 0, last_pulse = -2, last_on = -2;
    double note_f = 0;
    unsigned note_fn = 0;
    for (size_t i = 0; i < R->n; i++) {
        const tone_frame *f = &R->f[i];
        S->kind[f->kind]++;
        if (f->gate) {
            S->on++;
            if (f->wave == TONE_NOISE) S->on_noise++;
            else {
                S->on_pulse++;
                if (!S->fn_min || f->fn < S->fn_min) S->fn_min = f->fn;
                if (f->fn > S->fn_max) S->fn_max = f->fn;
                if (last_on != (long)i - 1 || fabs((double)f->fn - note_fn) > 0.03 * note_fn) { S->played++; note_fn = f->fn; }
                last_on = (long)i;
            }
        }
        if (f->octaves) S->octave++;
        if (f->too_high) S->too_high++;
        if (f->carried) S->carried++;
        if (f->changed) S->packets++;
        inwin += f->changed - win[i % 60]; win[i % 60] = f->changed;
        if (inwin > S->max60) S->max60 = inwin;
        if (pulse_kind(f->kind)) {                 /* notes as S1 counts them: a new one after a gap or a 3% step */
            const double hz = SPK_APPLE_HZ / f->period;
            if (!S->hz_min || hz < S->hz_min) S->hz_min = hz;
            if (hz > S->hz_max) S->hz_max = hz;
            if (last_pulse != (long)i - 1 || fabs(hz - note_f) > 0.03 * note_f) { S->notes++; note_f = hz; }
            last_pulse = (long)i;
        }
    }
}
static void frame_text(char *b, size_t n, uint32_t num, uint32_t den) {
    if (den == 1) snprintf(b, n, "%u", num); else snprintf(b, n, "%u/%u (%.2f)", num, den, (double)num / den);
}
static void tone_outputs(void) {
    static tone_run R;
    tone_stats S;
    char fr[64];
    tone_track(&R, tone_fnum, tone_fden, M.cpu.cycles);
    tone_stats_of(&R, &S);
    frame_text(fr, sizeof fr, tone_fnum, tone_fden);
    if (sound_report) {
        printf("tone tracker (S2): frame %s Apple cycles, NTSC SID clock; %zu frames\n", fr, R.n);
        printf("  kinds: steady %ld, split %ld (%ld carry the old note on), slow %ld, irregular %ld, sparse %ld, silent %ld\n",
               S.kind[TONE_STEADY], S.kind[TONE_SPLIT], S.carried, S.kind[TONE_SLOW], S.kind[TONE_IRREGULAR], S.kind[TONE_SPARSE],
               S.kind[TONE_SILENT]);
        printf("  voice 1 on: %ld frames (pulse %ld, noise %ld)", S.on, S.on_pulse, S.on_noise);
        if (S.on_pulse) printf("; %ld note%s played, Fn %u-%u (%.1f-%.1f Hz)", S.played, S.played == 1 ? "" : "s", S.fn_min, S.fn_max, fn_hz(S.fn_min), fn_hz(S.fn_max));
        printf("\n  out of the SID's range: %ld frames an octave or two down, %ld too high (gate off)\n", S.octave, S.too_high);
        printf("  packets (state changes): %ld, at most %ld in any 60 frames\n", S.packets, S.max60);
        static tone_run R1;                        /* the same tracker at S1's frame, to check against S1's report */
        tone_stats S1;
        tone_track(&R1, TONE_FRAME_S1_NUM, TONE_FRAME_S1_DEN, M.cpu.cycles);
        tone_stats_of(&R1, &S1);
        printf("  check at S1's frame (1/60 Apple second): steady %ld, split %ld, slow %ld, irregular %ld, sparse %ld frames; %ld notes",
               S1.kind[TONE_STEADY], S1.kind[TONE_SPLIT], S1.kind[TONE_SLOW], S1.kind[TONE_IRREGULAR], S1.kind[TONE_SPARSE], S1.notes);
        if (S1.notes) printf(", %.1f-%.1f Hz", S1.hz_min, S1.hz_max);
        printf("\n");
    }
    if (tone_log_path) {
        FILE *f = fopen(tone_log_path, "w");
        if (!f) printf("could not write %s\n", tone_log_path);
        else {
            fprintf(f, "# tone log (S2): one line per change of SID voice 1, what an S3 sound packet carries.\n");
            fprintf(f, "# Frame %s Apple cycles, NTSC SID clock; each line plays from its frame's end (plays-from).\n", fr);
            fprintf(f, "# Hz is what the SID plays: Fn x %.1f / 2^24. Gate off keeps the last Fn and width (the release).\n", C64_HZ);
            fprintf(f, "#   frame  plays-from-s  gate  wave      Fn    PW        Hz  kind       clicks\n");
            for (size_t i = 0; i < R.n; i++) {
                const tone_frame *t = &R.f[i];
                if (!t->changed) continue;
                fprintf(f, "%9u  %12.4f  %4s  %-5s  %6u  %4u  %8.1f  %-9s  %6u%s%s%s\n", t->index, (double)t->end / SPK_APPLE_HZ,
                        t->gate ? "on" : "off", t->wave == TONE_NOISE ? "noise" : "pulse", t->fn, t->pw, fn_hz(t->fn),
                        kind_name(t->kind), t->clicks, t->octaves ? (t->octaves == 1 ? "  1 octave down" : "  2 octaves down") : "",
                        t->too_high ? "  too high" : "", t->carried ? "  carried" : "");
            }
            printf(fclose(f) == 0 ? "tone log saved: %s\n" : "could not write %s\n", tone_log_path);
        }
    }
    if (sid_wav_path)
        printf(sid_write_wav(R.f, R.n, TONE_CLOCK_NTSC_NUM, TONE_CLOCK_NTSC_DEN, M.cpu.cycles, sid_wav_path)
               ? "SID voice saved: %s\n" : "could not write %s\n", sid_wav_path);
}

static void enqueue(uint16_t k) { if ((qtail + 1) % KEYQ != qhead) { queue[qtail] = k; qtail = (qtail + 1) % KEYQ; } }

/* Apple ][ Plus character generator: 64 glyphs, picked by the low 6 bits.
 * $00-$3F inverse, $40-$7F flashing, $80-$FF normal. */
static char glyph(uint8_t b) { uint8_t i = b & 0x3F; return (char)(i < 0x20 ? i + 0x40 : i); }

static void stop_reason(char *out, size_t n) {
    if (M.stopped == 1)
        snprintf(out, n, "STOPPED: missing ROM routine at $%04X (called from about $%04X)", M.stop_pc, M.stop_caller);
    else if (M.stopped == 2)
        snprintf(out, n, "STOPPED: undocumented opcode $%02X at $%04X", M.cpu.jammed - 1, M.stop_pc);
    else if (M.stopped == 3)
        snprintf(out, n, "STOPPED: BRK about to run at $%04X", M.stop_pc);
    else if (M.stopped == 4)
        snprintf(out, n, "STOPPED: reached $%04X (--stop-at)", M.stop_pc);
    else if (M.stopped == 6)
        snprintf(out, n, "STOPPED: the instruction at $%04X read unwritten ROM byte $%04X (--stop-on-peek)", M.stop_pc, M.stop_caller);
    else if (M.stopped == 7)
        snprintf(out, n, "STOPPED: the instruction at $%04X took the waiting key (--stop-on-key-read)", M.stop_pc);
    else if (M.stopped == 5)
        snprintf(out, n, "STOPPED: call from $%04X to $%04X in BASIC's space, no pinned entry point there", M.stop_caller, M.stop_pc);
    else
        out[0] = 0;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* The disk image in memory, read and written at offsets by emu/diskimg.c (the module reads the SD card). */
static uint8_t *img_data; static size_t img_len; static dimg IMG;
static int img_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len) {
    (void)ctx; if ((size_t)off + len > img_len) return -1; memcpy(buf, img_data + off, len); return (int)len;
}
static int img_write(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len) {
    (void)ctx; if ((size_t)off + len > img_len) return -1; memcpy(img_data + off, buf, len); return (int)len;
}
static int img_block(void *ctx, int write, uint32_t block, uint8_t *buf) {
    (void)ctx;
    if (write) return IMG.locked ? 0x2B : dimg_write_block(&IMG, block, buf) == 0 ? 0 : 0x27;
    return dimg_read_block(&IMG, block, buf) == 0 ? 0 : 0x27;
}

static int attach_disk(const char *disk_path, const char *card_path) {
    size_t clen, dlen;
    uint8_t *card = read_file(card_path, &clen);
    if (!card || clen != 256) { fprintf(stderr, "card firmware %s missing or not 256 bytes\n", card_path); return 0; }
    uint8_t *img = read_file(disk_path, &dlen);
    if (!img) { fprintf(stderr, "cannot read disk %s\n", disk_path); return 0; }
    static char note[1280];
    if (dlen >= 8 && !memcmp(img, "WOZ2", 4)) {
        const char *why;
        if (woz35_decode(img, dlen, &WD, &why) != 0) { fprintf(stderr, "%s: %s\n", disk_path, why); return 0; }
        a2_attach_disk(&M, 5, card, WD.blocks, WD.nblocks, WD.bad);
        snprintf(note, sizeof note, "disk: WOZ, %d blocks, %d unreadable", WD.nblocks, WD.nbad);
        free(img);
    } else if (dlen <= 0xFFFFFFFFu && dimg_open(&IMG, disk_path, (uint32_t)dlen, img, dlen < DIMG_HEAD_BYTES ? (uint32_t)dlen : DIMG_HEAD_BYTES,
                                               img_read, img_write, NULL) == 0) {
        int nblocks = (int)IMG.blocks, nbad = 0;
        uint8_t *bad = NULL;
        char bpath[1024];
        size_t blen;
        char *btext = a2_badlist_path(disk_path, bpath, sizeof bpath) ? (char *)read_file(bpath, &blen) : NULL;
        if (btext) {                                                   /* the unreadable-block list beside it */
            bad = calloc((size_t)nblocks, 1);
            nbad = bad ? a2_parse_badlist(btext, blen, bad, nblocks) : -1;
            free(btext);
            if (nbad < 0) { fprintf(stderr, "%s: not a list of block numbers below %d\n", bpath, nblocks); return 0; }
        }
        img_data = img; img_len = dlen;                                  /* img (and bad) stay: they are the disk */
        a2_attach_blockdev(&M, 5, card, nblocks, bad, bad ? nblocks : 0, img_block, NULL);
        snprintf(note, sizeof note, "disk: %s, %d blocks, %d unreadable%s%s", dimg_describe(&IMG), nblocks, nbad,
                 bad ? " (from " : "", bad ? bpath : "");
        if (bad) strncat(note, ")", sizeof note - strlen(note) - 1);
    } else { fprintf(stderr, "%s: not a WOZ 2 image, nor a disk image (%s)\n", disk_path, IMG.why ? IMG.why : "too big"); return 0; }
    disk_note = note;
    free(card);
    return 1;
}

/* ---- PNG (stored deflate, no library) ---- */
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c; while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    put32(f, (uint32_t)n); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
    uint32_t c = crc(0, (const uint8_t *)type, 4); c = crc(c, data, n); put32(f, c);
}
static int write_png(const char *path, int w, int h, const uint8_t *rgb) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) { raw[y * (1 + w * 3)] = 0; memcpy(raw + y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t nblocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + nblocks * 5 + 4;
    uint8_t *z = malloc(z_n), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t len = raw_n - off < 65535 ? raw_n - off : 65535;
        *p++ = off + len == raw_n; *p++ = (uint8_t)len; *p++ = (uint8_t)(len >> 8); *p++ = (uint8_t)~len; *p++ = (uint8_t)(~len >> 8);
        memcpy(p, raw + off, len); p += len;
    }
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    *p++ = (uint8_t)(ad >> 24); *p++ = (uint8_t)(ad >> 16); *p++ = (uint8_t)(ad >> 8); *p++ = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (size_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 1;
}

/* Hi-res page as 560x384 RGB, with a simple colour rule: two lit neighbours are white;
 * a lone lit pixel takes its colour from its column and the byte's high bit (violet/green,
 * or blue/orange); a dark pixel between two same-colour lone pixels is filled. */
static void render_hires(uint8_t *rgb) {
    static const uint8_t COL[4][3] = { { 0xD0, 0x40, 0xF0 }, { 0x40, 0xD0, 0x40 },   /* violet, green */
                                       { 0x40, 0x90, 0xF0 }, { 0xF0, 0x80, 0x20 } }; /* blue, orange */
    for (int y = 0; y < 192; y++) {
        uint16_t base = a2_hires_row(&M, y);
        uint8_t on[280], pal[280];
        for (int x = 0; x < 280; x++) { uint8_t b = M.ram[base + x / 7]; on[x] = (b >> (x % 7)) & 1; pal[x] = b >> 7; }
        for (int x = 0; x < 280; x++) {
            int l = x > 0 && on[x - 1], r = x < 279 && on[x + 1];
            const uint8_t *c = NULL; static const uint8_t W[3] = { 0xFF, 0xFF, 0xFF }, K[3] = { 0, 0, 0 };
            if (on[x]) c = (l || r) ? W : COL[pal[x] * 2 + (x & 1)];
            else if (l && r && !(x > 1 && on[x - 2]) && !(x < 278 && on[x + 2])) c = COL[pal[x - 1] * 2 + ((x - 1) & 1)];
            else c = K;
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) memcpy(rgb + (((size_t)(y * 2 + dy) * 560) + (size_t)(x * 2 + dx)) * 3, c, 3);
        }
    }
}

static void print_ranges(const uint16_t *v, int n) {
    int shown = 0;
    for (int i = 0; i < n && shown < 60; ) {
        int j = i;
        while (j + 1 < n && v[j + 1] == v[j] + 1) j++;
        if (j > i) printf(" %u-%u", v[i], v[j]); else printf(" %u", v[i]);
        shown++; i = j + 1;
        if (shown == 60 && i < n) printf(" ...");
    }
    printf("\n");
}

static void report(void) {
    printf("display: %s%s, page %d\n", M.text ? "TEXT" : (M.hires ? "HI-RES" : "LO-RES"),
           !M.text && M.mixed ? " + 4 text lines" : "", M.page2 ? 2 : 1);
    if (M.card_slot) {
        printf("%s\n", disk_note);
        printf("disk calls: %u reads (%u failed), %u writes, %u status, %u other\n",
               M.disk_reads, M.disk_errors, M.disk_writes, M.disk_status, M.disk_other);
        if (M.sp_calls[0] | M.sp_calls[1] | M.sp_calls[2] | M.sp_calls[4])
            printf("  SmartPort: STATUS %u, READ %u, WRITE %u, CONTROL %u\n", M.sp_calls[0], M.sp_calls[1], M.sp_calls[2], M.sp_calls[4]);
        if (M.nbad_reads) { printf("  unreadable blocks asked for:"); for (int i = 0; i < M.nbad_reads; i++) printf(" %u", M.bad_reads[i]); printf("\n"); }
        if (M.nread_order) { printf("  blocks read, in order:"); print_ranges(M.read_order, M.nread_order); }
    }
    if (M.log_romcalls) {
        printf("ROM entry points called from RAM: %d\n", M.nromcalls);
        for (int i = 0; i < M.nromcalls; i++)
            printf("  $%04X  from $%04X  x%u\n", M.romcalls[i].addr, M.romcalls[i].from, M.romcalls[i].count);
    }
    printf("missing ROM routines: %d\n", M.nmissing);
    for (int i = 0; i < M.nmissing; i++)
        printf("  $%04X  called from about $%04X  x%u\n", M.missing[i].addr, M.missing[i].from, M.missing[i].count);
    printf("reads of unwritten ROM bytes: %d\n", M.npeeks);
    for (int i = 0; i < M.npeeks; i++)
        printf("  $%04X  read by the instruction at $%04X  x%u\n", M.peeks[i].addr, M.peeks[i].from, M.peeks[i].count);
    static const char *gio[8] = { "tape in", "button 0", "button 1", "button 2", "paddle 0", "paddle 1", "paddle 2", "paddle 3" };
    int any_gio = 0;
    for (int i = 0; i < 32; i++) if (M.game_io[i]) any_gio = 1;
    printf("game I/O ($C060-$C07F, joystick and paddles):%s", any_gio ? "" : " none");
    for (int i = 0; i < 32; i++)
        if (M.game_io[i]) printf(" $C0%02X %s x%u", 0x60 + i, i < 16 ? gio[i & 7] : "trigger", M.game_io[i]);
    printf("\n");
    if (trace_show) {
        int n = trace_show < M.trace_n ? trace_show : M.trace_n;
        if (n > A2_TRACE) n = A2_TRACE;
        printf("last %d instructions (memory shown as it is now):\n", n);
        for (int i = M.trace_n - n; i < M.trace_n; i++) {
            const a2_step *s = &M.trace[i % A2_TRACE];
            char d[48];
            dis6502(&M, s->pc, d, sizeof d);
            printf("  %04X  %-26s A=%02X X=%02X Y=%02X P=%02X S=%02X\n", s->pc, d, s->a, s->x, s->y, s->p, s->sp);
        }
    }
}

static const char *bas_path, *cout_path, *dump_range;
static FILE *cout_file;
static void cout_to_file(uint8_t a) { fputc(a & 0x7F, cout_file); }
/* \b in --keys: an Applesoft program into memory as DOS's LOAD leaves it: bytes from $0801, $0800 = 0,
   TXTTAB $67 = $0801, VARTAB $69 / ARYTAB $6B / STREND $6D / PRGEND $AF = its end, line links fixed. */
static void load_bas(void) {
    size_t n; uint8_t *b = bas_path ? read_file(bas_path, &n) : NULL;
    if (!b || n < 2 || n > 0x9000) { printf("--bas: cannot read %s\n", bas_path ? bas_path : "(none)"); free(b); return; }
    memcpy(&M.ram[0x0801], b, n); M.ram[0x0800] = 0; free(b);
    unsigned p = 0x0801;
    while (p + 3 < 0x0801 + n && (M.ram[p] | M.ram[p + 1])) {   /* relink: each link points at the next line */
        unsigned q = p + 4; while (q < 0x0801 + n && M.ram[q]) q++;
        q++; M.ram[p] = (uint8_t)q; M.ram[p + 1] = (uint8_t)(q >> 8); p = q;
    }
    unsigned end = p + 2;                                           /* after the 00 00 end link */
    M.ram[0x67] = 0x01; M.ram[0x68] = 0x08;
    for (unsigned z = 0x69; z <= 0x6D; z += 2) { M.ram[z] = (uint8_t)end; M.ram[z + 1] = (uint8_t)(end >> 8); }
    M.ram[0xAF] = (uint8_t)end; M.ram[0xB0] = (uint8_t)(end >> 8);
}
static void dump_memory(void) {
    unsigned a = 0, b = 0;
    if (sscanf(dump_range, "%x-%x", &a, &b) != 2 || b < a || b > 0xFFFF) { printf("--dump: bad range %s\n", dump_range); return; }
    for (unsigned r = a; r <= b; r += 16) {
        printf("%04X-", r);
        for (unsigned i = r; i < r + 16 && i <= b; i++) printf(" %02X", a2_peek(&M, (uint16_t)i));
        printf("\n");
    }
}
static int arm_after_keys, armed_stop_at = -1, armed_peek, armed_keyread;
static int headless(const char *keys, double seconds, const char *png) {
    for (const char *p = keys; p && *p; p++) {
        if (*p == '\\' && p[1] == 'r') { enqueue(0x0D); p++; }
        else if (*p == '\\' && p[1] == 'e') { enqueue(0x1B); p++; }
        else if (*p == '\\' && p[1] == 'w') { enqueue(0x100); p++; }
        else if (*p == '\\' && p[1] == 'b') { enqueue(0x101); p++; }
        else if (*p == '\\' && p[1] == 'p' && p[2] >= '0' && p[2] <= '3' && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
            char hex[3] = { p[3], p[4], 0 }; enqueue((uint16_t)(0x1000 | (p[2] - '0') << 8 | strtoul(hex, NULL, 16))); p += 4;
        } else if (*p == '\\' && p[1] == 's' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = { p[2], p[3], 0 }; enqueue((uint16_t)(0x1600 | (strtoul(hex, NULL, 16) & STICK_BITS))); p += 3;
        } else if (*p == '\\' && (p[1] == 'd' || p[1] == 'u') && p[2] >= '0' && p[2] <= '2') {
            enqueue((uint16_t)((p[1] == 'd' ? 0x1400 : 0x1500) | (p[2] - '0'))); p += 2;
        }
        else if (*p == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = { p[2], p[3], 0 }; enqueue((uint16_t)strtoul(hex, NULL, 16)); p += 3;
        } else enqueue((uint16_t)toupper((unsigned char)*p));
    }
    long frames_after = (long)(seconds * 60), idle = 0, guard = 0, wait = 0;
    while (!M.stopped && idle < frames_after && guard++ < 60L * 3600) {
        if (wait > 0) wait--;
        else if (ninject && !a2_key_waiting(&M)) { a2_key(&M, inject[0]); inject[0] = inject[1]; ninject--; }
        else if (qhead != qtail && !a2_key_waiting(&M)) {
            uint16_t k = queue[qhead]; qhead = (qhead + 1) % KEYQ;
            if (k == 0x100) wait = 60; else if (k == 0x101) load_bas();
            else if ((k & 0xFC00) == 0x1000) a2_set_paddle(&M, (k >> 8) & 3, (uint8_t)k);
            else if ((k & 0xFF00) == 0x1400 || (k & 0xFF00) == 0x1500) a2_set_button(&M, k & 3, (k & 0xFF00) == 0x1400);
            else if ((k & 0xFF00) == 0x1600) {
                uint8_t keys[2];
                const int n = stick_apply(&M, &stick_last, (uint8_t)k, keys);
                if (n) { a2_key(&M, keys[0]); if (n > 1) inject[ninject++] = keys[1]; }
            }
            else a2_key(&M, (uint8_t)k);
        }
        if (arm_after_keys && qhead == qtail) {      /* every key typed: arm the stops now */
            M.stop_at = armed_stop_at; M.stop_on_peek = armed_peek; M.stop_on_keyread = armed_keyread; arm_after_keys = 0;
        }
        a2_run(&M, A2_CYCLES_PER_FRAME);
        if (qhead == qtail && !ninject && !a2_key_waiting(&M) && wait == 0) idle++;
    }
    printf("+----------------------------------------+\n");
    for (int r = 0; r < 24; r++) {
        uint16_t base = a2_text_row(&M, r);
        char line[41];
        for (int c = 0; c < 40; c++) line[c] = glyph(M.ram[base + c]);
        line[40] = 0;
        printf("|%s|\n", line);
    }
    printf("+----------------------------------------+\n");
    char why[128];
    stop_reason(why, sizeof why);
    printf("cycles %llu (%.1f Apple seconds), speaker clicks %llu\n", (unsigned long long)M.cpu.cycles,
           (double)M.cpu.cycles / 1022727.0, (unsigned long long)M.speaker_clicks);
    if (why[0]) printf("%s\n", why);
    report();
    if (sound_report) spk_report(&SPK, stdout, 1);
    if (wav_path) printf(spk_write_wav(&SPK, M.cpu.cycles, wav_path) ? "speaker saved: %s\n" : "could not write %s\n", wav_path);
    if (sound_report || sid_wav_path || tone_log_path) tone_outputs();
    if (click_log_path) {
        FILE *f = fopen(click_log_path, "w");
        if (f) for (size_t i = 0; i < SPK.n; i++) fprintf(f, "%llu\n", (unsigned long long)SPK.t[i]);
        printf(f && fclose(f) == 0 ? "click log saved: %s\n" : "could not write %s\n", click_log_path);
    }
    if (dump_range) dump_memory();
    if (cout_file) fclose(cout_file);
    if (png) {
        static uint8_t rgb[560 * 384 * 3];
        render_hires(rgb);
        printf(write_png(png, 560, 384, rgb) ? "hi-res screen saved: %s\n" : "could not write %s\n", png);
    }
    return M.stopped ? 1 : 0;
}

static struct termios saved_tio;
static void term_restore(void) {
    tcsetattr(0, TCSANOW, &saved_tio);
    printf("\x1b[0m\x1b[?25h\x1b[29;1H\n");
    fflush(stdout);
}

static void now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }
static double since(const struct timespec *a) {
    struct timespec b; now(&b);
    return (double)(b.tv_sec - a->tv_sec) + (double)(b.tv_nsec - a->tv_nsec) / 1e9;
}

static int interactive(void) {
    if (!isatty(0)) { fprintf(stderr, "interactive mode needs a terminal (try --headless)\n"); return 2; }
    tcgetattr(0, &saved_tio);
    struct termios raw = saved_tio;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    atexit(term_restore);
    printf("\x1b[2J\x1b[?25l\x1b[1;1H+----------------------------------------+");
    for (int r = 0; r < 24; r++) printf("\x1b[%d;1H|\x1b[%d;42H|", r + 2, r + 2);
    printf("\x1b[26;1H+----------------------------------------+");
    printf("\x1b[27;1HTR ][ emulator   Ctrl-\\ = Reset   Ctrl-] = quit");
    fflush(stdout);

    char shown[24][40]; int shown_attr[24][40];
    memset(shown, 0, sizeof shown); memset(shown_attr, -1, sizeof shown_attr);
    struct timespec start; now(&start);
    long frame = 0;
    char last_why[160] = "x";
    for (;;) {
        uint8_t in[64];
        ssize_t n = read(0, in, sizeof in);
        for (ssize_t i = 0; i < n; i++) {
            uint8_t b = in[i];
            if (b == 0x1D) return 0;                                       /* Ctrl-] */
            if (b == 0x1C) { qhead = qtail = 0; a2_reset(&M); continue; }  /* Ctrl-\ */
            if (b == 0x1B && i + 2 < n && in[i + 1] == '[') {              /* arrow keys */
                if (in[i + 2] == 'D') enqueue(0x08);
                else if (in[i + 2] == 'C') enqueue(0x15);
                i += 2; continue;
            }
            if (b == '\n') b = '\r';
            if (b == 0x7F) b = 0x08;
            enqueue((uint16_t)toupper(b));
        }
        if (qhead != qtail && !a2_key_waiting(&M)) { a2_key(&M, (uint8_t)queue[qhead]); qhead = (qhead + 1) % KEYQ; }
        a2_run(&M, A2_CYCLES_PER_FRAME);
        frame++;
        int flash_on = (frame / 16) & 1;
        for (int r = 0; r < 24; r++) {
            uint16_t base = a2_text_row(&M, r);
            for (int c = 0; c < 40; c++) {
                uint8_t b = M.ram[base + c];
                int inv = b < 0x40 || (b < 0x80 && flash_on);
                char g = glyph(b);
                if (shown[r][c] != g || shown_attr[r][c] != inv) {
                    printf("\x1b[%d;%dH%s%c\x1b[0m", r + 2, c + 2, inv ? "\x1b[7m" : "", g);
                    shown[r][c] = g; shown_attr[r][c] = inv;
                }
            }
        }
        char why[160], stop[128];
        stop_reason(stop, sizeof stop);
        snprintf(why, sizeof why, "%s%s", !M.text ? "[graphics mode: the terminal shows only the text page]  " : "", stop);
        if (strcmp(why, last_why) != 0) {
            printf("\x1b[28;1H\x1b[2K%s%s", why, stop[0] ? "   (Ctrl-\\ resets)" : "");
            strcpy(last_why, why);
        }
        fflush(stdout);
        double ahead = (double)frame / 60.0 - since(&start);
        if (ahead > 0 && ahead < 1) { struct timespec t = { 0, (long)(ahead * 1e9) }; nanosleep(&t, NULL); }
    }
}

int main(int argc, char **argv) {
    const char *rom_path = "build/tr-rom.bin", *card_path = "build/blockcard.bin", *ext_path = "build/tr-ext.bin";
    const char *disk_path = NULL, *keys = NULL, *png = NULL;
    int is_headless = 0, stub = 0, stop_at = -1, npatch = 0, romcalls = 0, paddle0[4] = { 0, 0, 0, 0 }, button0[3] = { 0, 0, 0 };
    const char *patches[16];
    double seconds = 1.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--ext") && i + 1 < argc) ext_path = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card_path = argv[++i];
        else if (!strcmp(argv[i], "--headless")) is_headless = 1;
        else if (!strcmp(argv[i], "--stub-missing")) stub = 1;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_show = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stop-on-brk")) M.stop_on_brk = 1;
        else if (!strcmp(argv[i], "--stop-on-peek")) M.stop_on_peek = 1;
        else if (!strcmp(argv[i], "--arm-after-keys")) arm_after_keys = 1;
        else if (!strcmp(argv[i], "--stop-on-key-read")) M.stop_on_keyread = 1;
        else if (!strcmp(argv[i], "--rom-patch") && i + 1 < argc && npatch < 16) patches[npatch++] = argv[++i];
        else if (!strcmp(argv[i], "--stop-at") && i + 1 < argc) stop_at = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--rom-calls")) romcalls = 1;
        else if (!strcmp(argv[i], "--stop-unpinned")) romcalls = 2;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_range = argv[++i];
        else if (!strcmp(argv[i], "--bas") && i + 1 < argc) bas_path = argv[++i];
        else if (!strcmp(argv[i], "--cout-log") && i + 1 < argc) cout_path = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--sound-report")) sound_report = 1;
        else if (!strcmp(argv[i], "--paddle") && i + 1 < argc) {
            const char *a = argv[++i]; char *e; long v;
            if (a[0] < '0' || a[0] > '3' || a[1] != '=' || (v = strtol(a + 2, &e, 10)) < 0 || v > 255 || *e || e == a + 2) {
                fprintf(stderr, "bad --paddle %s (N=V, N 0-3, V 0-255)\n", a); return 2;
            }
            paddle0[a[0] - '0'] = (int)v;
        }
        else if (!strcmp(argv[i], "--button") && i + 1 < argc) {
            const char *a = argv[++i];
            if (a[0] < '0' || a[0] > '2' || a[1]) { fprintf(stderr, "bad --button %s (0-2)\n", a); return 2; }
            button0[a[0] - '0'] = 1;
        }
        else if (!strcmp(argv[i], "--sid-wav") && i + 1 < argc) sid_wav_path = argv[++i];
        else if (!strcmp(argv[i], "--tone-log") && i + 1 < argc) tone_log_path = argv[++i];
        else if (!strcmp(argv[i], "--click-log") && i + 1 < argc) click_log_path = argv[++i];
        else if (!strcmp(argv[i], "--tone-frame") && i + 1 < argc) {
            char *e; tone_fnum = (uint32_t)strtoul(argv[++i], &e, 10);
            tone_fden = *e == '/' ? (uint32_t)strtoul(e + 1, NULL, 10) : 1;
            if (!tone_fnum || !tone_fden) { fprintf(stderr, "bad --tone-frame %s\n", argv[i]); return 2; }
        }
        else { fprintf(stderr, "usage: a2term [--headless] [--rom F] [--ext F] [--disk F] [--card F] [--stub-missing] [--keys T] [--seconds N] [--png F]\n"); return 2; }
    }
    size_t rlen;
    uint8_t *rom = read_file(rom_path, &rlen);
    if (!rom || rlen != 0x3000) { fprintf(stderr, "ROM %s missing or not 12288 bytes\n", rom_path); return 2; }
    int brk = M.stop_on_brk, peekstop = M.stop_on_peek, keystop = M.stop_on_keyread;
    a2_init(&M, rom);
    free(rom);
    size_t elen;
    uint8_t *ext = read_file(ext_path, &elen);
    if (!ext || elen != 0x800) { fprintf(stderr, "extension ROM %s missing or not 2048 bytes\n", ext_path); return 2; }
    a2_load_ext(&M, ext);
    free(ext);
    for (int p = 0; p < npatch; p++) {                 /* probe-only ROM overrides */
        unsigned addr = (unsigned)strtoul(patches[p], NULL, 16);
        const char *hex = strchr(patches[p], ':');
        if (!hex || addr < 0xD000 || addr > 0xFFFF) { fprintf(stderr, "bad --rom-patch %s\n", patches[p]); return 2; }
        for (hex++; isxdigit((unsigned char)hex[0]) && isxdigit((unsigned char)hex[1]) && addr <= 0xFFFF; hex += 2, addr++) {
            char b[3] = { hex[0], hex[1], 0 };
            M.rom[addr - 0xD000] = (uint8_t)strtoul(b, NULL, 16);
            M.rom_gap[addr - 0xD000] = 0;
        }
        printf("probe: ROM patched in memory: %s\n", patches[p]);
    }
    M.stub_missing = stub;
    for (int n = 0; n < 4; n++) a2_set_paddle(&M, n, (uint8_t)paddle0[n]);
    for (int n = 0; n < 3; n++) a2_set_button(&M, n, button0[n]);
    M.stop_on_brk = brk;
    M.stop_on_peek = peekstop;
    M.stop_on_keyread = keystop;
    M.trace_on = trace_show > 0;
    M.stop_at = stop_at;
    if (arm_after_keys) {
        armed_stop_at = stop_at; armed_peek = M.stop_on_peek; armed_keyread = M.stop_on_keyread;
        M.stop_at = -1; M.stop_on_peek = 0; M.stop_on_keyread = 0;
    }
    M.log_romcalls = romcalls > 0;
    M.stop_unpinned = romcalls == 2;
    static uint8_t known[0x2800];
    if (M.stop_unpinned) {                             /* build/entries.txt: applesoft-entries.py --write */
        FILE *ef = fopen("build/entries.txt", "r"); unsigned ea; int ne = 0;
        if (!ef) { fprintf(stderr, "--stop-unpinned needs build/entries.txt (tools/applesoft-entries.py --write)\n"); return 2; }
        while (fscanf(ef, "%x", &ea) == 1) if (ea >= 0xD000 && ea < 0xF800) { known[ea - 0xD000] = 1; ne++; }
        fclose(ef); M.known = known; printf("known entry points: %d\n", ne);
    }
    if (cout_path) {
        cout_file = fopen(cout_path, "wb");
        if (!cout_file) { fprintf(stderr, "cannot write %s\n", cout_path); return 2; }
        M.cout_hook = cout_to_file;
    }
    if (wav_path || sound_report || sid_wav_path || tone_log_path || click_log_path) { M.speaker_hook = spk_record; M.speaker_ctx = &SPK; }
    if (disk_path && !attach_disk(disk_path, card_path)) return 2;
    a2_reset(&M);
    return is_headless ? headless(keys, seconds, png) : interactive();
}
