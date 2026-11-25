/* test-client.c: run the C64 client's key mapping and input sending on our 6502 core,
 * against a fake host whose input check is upstream's commandWrite (VMHostCommand.h)
 * line for line: token not zero, token not the last one taken, checksum over
 * $A5 ^ $DFF8 ^ $DFF9 ^ $DFFA ^ $DFFD ^ $DFFE, and nothing taken while one is pending.
 * Also the sound (packet type 3 to SID voice 1, in the order the client writes it) and joystick port 2,
 * against a model of the C64 keyboard: $DC00 drives the columns, $DC01 reads the rows; the stick pulls
 * columns low; a held key joins its column and row, so a low column pulls its row low and a low row
 * pulls the key's column low (which is how phantom keys and ghost stick bits arise).
 * Usage: test-client build/apple2-client.bin build/apple2-client.sym
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../emu/cpu6502.h"

static uint8_t mem[65536];
static uint8_t io2[256];                 /* the $DF00 window */
static int host_busy, taken_count, grants;
static uint8_t got[4];                   /* buttons, display, overflow, protocol */
static uint8_t stickPull;                /* port 2: $DC00 bits the stick holds low (0 up, 1 down, 2 left, 3 right, 4 fire) */
static int keyDown[8][8];                /* [column][row]: keys held */
static uint16_t sidLog[64]; static uint8_t sidVal[64]; static int nsid;   /* writes to $D400-$D418, in order */

static void keyboardLines(uint8_t *cols, uint8_t *rows) {
    uint8_t c = mem[0xDC00] & (uint8_t)~stickPull, r = 0xFF;
    for (int pass = 0; pass < 8; pass++)
        for (int col = 0; col < 8; col++)
            for (int row = 0; row < 8; row++)
                if (keyDown[col][row]) {
                    if (!(c >> col & 1)) r &= (uint8_t)~(1 << row);
                    if (!(r >> row & 1)) c &= (uint8_t)~(1 << col);
                }
    *cols = c; *rows = r;
}

static uint8_t rd(void *ctx, uint16_t a) {
    (void)ctx;
    if ((a & 0xFF00) == 0xDF00) return io2[a & 0xFF];
    if (a == 0xDC00 || a == 0xDC01) { uint8_t c, r; keyboardLines(&c, &r); return a == 0xDC00 ? c : r; }
    return mem[a];
}
static void wr(void *ctx, uint16_t a, uint8_t v) {
    (void)ctx;
    if (a >= 0xD400 && a <= 0xD418 && nsid < 64) { sidLog[nsid] = a; sidVal[nsid++] = v; }
    if ((a & 0xFF00) != 0xDF00) { mem[a] = v; return; }
    io2[a & 0xFF] = v;
    if ((a & 0xFF) == 0xF0) grants++;                 /* $DFF0: a DMA slice granted */
    if ((a & 0xFF) != 0xF4 || v != 3) return;       /* upstream commandWrite, value 3 */
    if (host_busy || !io2[0xFE] || io2[0xFE] == io2[0xFC]) return;
    uint8_t expected = 0xA5 ^ io2[0xF8] ^ io2[0xF9] ^ io2[0xFA] ^ io2[0xFD] ^ io2[0xFE];
    if (expected != io2[0xFF]) return;
    got[0] = io2[0xF8]; got[1] = io2[0xF9]; got[2] = io2[0xFA]; got[3] = io2[0xFD];
    taken_count++;
    io2[0xFC] = io2[0xFE];
}

static unsigned sym(const char *path, const char *name) {
    FILE *f = fopen(path, "r");
    char line[256], label[128];
    unsigned value;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, " %127s = $%x", label, &value) == 2 && !strcmp(label, name)) { fclose(f); return value; }
    fprintf(stderr, "symbol %s not found\n", name);
    exit(2);
}

static cpu6502 c;
/* Call a subroutine: push a return to $FFF0, run until it gets there. */
static void call(uint16_t addr, uint8_t a, uint8_t x) {
    c.a = a; c.x = x; c.sp = 0xFD; c.pc = addr; c.jammed = 0;
    mem[0x1FE] = 0xEF; mem[0x1FF] = 0xFF;        /* RTS lands on $FFF0 */
    c.sp = 0xFD;
    for (long n = 0; n < 100000 && c.pc != 0xFFF0 && !c.jammed; n++) cpu_step(&c);
    if (c.pc != 0xFFF0) { printf("FAIL: call $%04X did not return (pc $%04X)\n", addr, c.pc); exit(1); }
}

static int failures;
/* Would the KERNAL's scan, driving column col alone, see row row low? */
static int scanSees(int col, int row) {
    mem[0xDC00] = (uint8_t)~(1 << col);
    uint8_t cc, rr;
    keyboardLines(&cc, &rr);
    return !(rr >> row & 1);
}
/* A packet as the host frames it ("M3", version 1, CRC-16 $1021 from $FFFF), into the $DF00 window. */
static void publish(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len) {
    uint8_t pk[240] = { 'M', '3', 1, type, seq, 0, len, 0 };
    memcpy(pk + 8, payload, len);
    unsigned crc = 0xFFFF;
    for (int i = 0; i < 8 + len; i++) { crc ^= (unsigned)pk[i] << 8; for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF; }
    pk[8 + len] = (uint8_t)crc; pk[9 + len] = (uint8_t)(crc >> 8);
    memcpy(io2, pk, (size_t)(10 + len));
}
static void expect(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: test-client BIN SYM\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(mem + 0x8000, 1, 0x2000, f) != 0x2000) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    unsigned mapKey = sym(argv[2], "mapKey"), queueEvent = sym(argv[2], "queueEvent"), trySend = sym(argv[2], "trySend");
    unsigned pending = sym(argv[2], "pending"), evseq = sym(argv[2], "evseq");
    c.read = rd; c.write = wr; c.p = FLAG_U | FLAG_I;

    struct { uint8_t petscii, key, flags; const char *what; } keys[] = {
        { 0x41, 0x41, 0, "A stays A" },              { 0xC1, 0x41, 0, "shifted A is upper-case A" },
        { 0x30, 0x30, 0, "digit 0" },                { 0x3A, 0x3A, 0, "colon" },
        { 0x0D, 0x0D, 0, "RETURN" },                 { 0x14, 0x08, 0, "DEL is the Apple left arrow" },
        { 0x9D, 0x08, 0, "CRSR left is the left arrow" }, { 0x1D, 0x15, 0, "CRSR right is the right arrow" },
        { 0x03, 0x03, 0, "RUN/STOP is Ctrl-C" },     { 0x5F, 0x1B, 0, "<- key is Esc" },
        { 0x18, 0x18, 0, "CTRL+X is Ctrl-X" },       { 0x5C, 0x5C, 0, "pound is backslash" },
        { 0x85, 0x00, 1, "F1 is Apple Reset" },      { 0x88, 0x00, 2, "F7 is quit" },
        { 0x87, 0x00, 4, "F5 asks the module to type the demo" },
        { 0x91, 0x00, 0, "CRSR up is ignored" },     { 0x90, 0x00, 0, "a colour code is ignored" },
    };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        call((uint16_t)mapKey, keys[i].petscii, 0);
        char what[96];
        snprintf(what, sizeof what, "map $%02X: %s (got A=$%02X X=%d)", keys[i].petscii, keys[i].what, c.a, c.x);
        expect(c.a == keys[i].key && c.x == keys[i].flags, what);
    }

    /* F3: the key readout, hidden at start; hiding it again restores the module's row 24. */
    unsigned showkeys = sym(argv[2], "showkeys"), row24save = sym(argv[2], "row24save");
    const uint16_t status = 0x0400 + 24 * 40 + 28;
    call((uint16_t)mapKey, 0x86, 0);
    expect(c.a == 0 && c.x == 0 && mem[showkeys] == 1, "map $86: F3 shows the key readout and sends the Apple nothing");
    for (int i = 0; i < 12; i++) { mem[row24save + i] = (uint8_t)(0x10 + i); mem[status + i] = 0x20; }
    call((uint16_t)mapKey, 0x86, 0);
    int restored = 1;
    for (int i = 0; i < 12; i++) if (mem[status + i] != 0x10 + i) restored = 0;
    expect(c.a == 0 && c.x == 0 && mem[showkeys] == 0 && restored,
           "F3 again hides it and puts back what the module sent for columns 28-39");

    call((uint16_t)queueEvent, 0, 0);
    expect(mem[pending] == 0, "no key and no flags: no event");
    call((uint16_t)queueEvent, 0x41, 0);
    expect(mem[pending] == 1 && mem[evseq] == 1, "a key starts event 1");
    host_busy = 1;
    call((uint16_t)trySend, 0, 0);
    expect(taken_count == 0 && mem[pending] == 1, "host busy: the record is dropped, the event stays pending");
    host_busy = 0;
    call((uint16_t)trySend, 0, 0);
    expect(taken_count == 1 && mem[pending] == 0, "host free: taken (token and checksum accepted), event done");
    expect(got[0] == 0x41 && got[1] == 0 && got[2] == 1 && got[3] == 0xA2, "host got key $41, flags 0, event 1, protocol $A2");
    call((uint16_t)queueEvent, 0, 1);
    call((uint16_t)trySend, 0, 0);
    expect(taken_count == 2 && got[1] == 1 && got[2] == 2, "F1 goes out as flags 1, event 2");
    for (int i = 0; i < 300; i++) { call((uint16_t)queueEvent, 0x42, 0); call((uint16_t)trySend, 0, 0); }
    expect(taken_count == 302, "300 more events, each taken once (the token never repeats or hits zero)");

    /* Hi-res: a type-2 packet (built as the host frames it: "M3", version 1, CRC-16 $1021 from $FFFF). */
    unsigned readPacket = sym(argv[2], "readPacket"), irq = sym(argv[2], "irq"), grant = sym(argv[2], "grant");
    unsigned splitv = sym(argv[2], "split"), splitLine = sym(argv[2], "splitLine");
    mem[0xEA81] = 0x60; mem[0xEA31] = 0x60;          /* the KERNAL's IRQ exits, as RTS here */
    for (int s2 = 0; s2 < 3; s2++) {
        const uint8_t want = s2 == 0 ? 24 : s2 == 1 ? 20 : 0;
        uint8_t pk[16] = { 'M', '3', 1, 2, (uint8_t)(9 + s2), 0, 1, 0, want };
        unsigned crc = 0xFFFF;
        for (int i = 0; i < 9; i++) { crc ^= (unsigned)pk[i] << 8; for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF; }
        pk[9] = (uint8_t)crc; pk[10] = (uint8_t)(crc >> 8);
        memcpy(io2, pk, 11);
        call((uint16_t)readPacket, 0, 0);
        char what[120];
        if (want) {
            snprintf(what, sizeof what, "mode packet %d: raster interrupts on, the split at line %d", want, 49 + 8 * want);
            expect((c.p & FLAG_C) && mem[splitv] == want && mem[0xD01A] == 1 && mem[splitLine] == 49 + 8 * want && mem[0xD012] == 20, what);
            mem[0xD019] = 0x81;                           /* a raster interrupt: the top one */
            call((uint16_t)irq, 0, 0);
            int top = mem[0xD011] == 0x3B && mem[0xD018] == 0x18 && mem[0xD012] == 49 + 8 * want;
            mem[0xD019] = 0x81;                           /* then the split */
            call((uint16_t)irq, 0, 0);
            int bottom = mem[0xD011] == 0x1B && mem[0xD018] == 0x15 && mem[0xD012] == 20;
            snprintf(what, sizeof what, "row %d: bitmap from the top, text from the split, then back to the top", want);
            expect(top && bottom, what);
            int before = grants;
            mem[0xD011] = 0x1B; mem[0xD012] = 17; call((uint16_t)grant, 0, 0);   /* 3 lines before the top interrupt */
            int nearTop = grants == before;
            mem[0xD012] = (uint8_t)(49 + 8 * want - 2); call((uint16_t)grant, 0, 0);
            int nearSplit = grants == before;
            mem[0xD012] = 120; call((uint16_t)grant, 0, 0);
            int mid = grants == before + 1;
            mem[0xD011] = 0x9B; mem[0xD012] = 4; call((uint16_t)grant, 0, 0);    /* line 260 */
            int low = grants == before + 2;
            snprintf(what, sizeof what, "row %d: no DMA grant just before an interrupt; grants elsewhere", want);
            expect(nearTop && nearSplit && mid && low, what);
        } else {
            expect(mem[splitv] == 0 && mem[0xD01A] == 0 && mem[0xD011] == 0x1B && mem[0xD018] == 0x15,
                   "mode packet 0: all text, raster interrupts off");
            int before = grants;
            mem[0xD012] = 18; call((uint16_t)grant, 0, 0);
            expect(grants == before + 1, "all text: grants anywhere (a DMA job may be landing before the switch)");
        }
    }
    mem[0xD011] = 0x1B; mem[0xD018] = 0x15;
    mem[0xD019] = 0x01;                                  /* raster flag latched, raster interrupts off */
    call((uint16_t)irq, 0, 0);
    expect(mem[0xD011] == 0x1B && mem[0xD018] == 0x15, "a latched raster flag with raster interrupts off is not taken for one");
    mem[0xD019] = 0x00; mem[0xDC0D] = 0;                 /* a CIA interrupt goes on to the KERNAL */
    c.p |= FLAG_I;
    call((uint16_t)irq, 0, 0);
    expect(!(c.p & FLAG_I), "a timer interrupt: acknowledged, interrupts back on, then the KERNAL's handler");

    /* ---- Sound (S3): SID set-up, type 3 packets, silence ---- */
    unsigned sidInit = sym(argv[2], "sidInit"), sidCtrl = sym(argv[2], "sidCtrl"), sound = sym(argv[2], "sound");
    for (int a = 0xD400; a <= 0xD418; a++) mem[a] = 0x55;
    call((uint16_t)sidInit, 0, 0);
    int quiet = 1;
    for (int a = 0xD407; a <= 0xD417; a++) if (mem[a] != 0) quiet = 0;
    expect(mem[0xD405] == 0x00 && mem[0xD406] == 0xF0 && mem[0xD418] == 0x0F && mem[0xD404] == 0 && quiet,
           "start: voice 1 AD $00 SR $F0, volume 15; voices 2 and 3 and the filter all 0");
    const uint8_t on[5] = { 0x41, 0xE2, 0x3B, 0x00, 0x08 };          /* pulse, gate on, Fn 15330, width 2048 */
    nsid = 0;
    publish(3, 50, on, 5);
    uint64_t t0 = c.cycles;
    call((uint16_t)readPacket, 0, 0);
    const uint64_t soundCycles = c.cycles - t0;
    int order = nsid == 5 && sidLog[0] == 0xD400 && sidLog[1] == 0xD401 && sidLog[2] == 0xD402 && sidLog[3] == 0xD403 && sidLog[4] == 0xD404;
    expect((c.p & FLAG_C) && order && sidVal[0] == 0xE2 && sidVal[1] == 0x3B && sidVal[2] == 0x00 && sidVal[3] == 0x08 && sidVal[4] == 0x41,
           "a sound packet writes Fn low, Fn high, width low, width high, then the control ($41: pulse, gate on)");
    const uint8_t noise[5] = { 0x81, 0x40, 0x06, 0x00, 0x08 }, both[5] = { 0xC1, 0x40, 0x06, 0x00, 0x08 }, off[5] = { 0x40, 0xE2, 0x3B, 0x00, 0x08 };
    publish(3, 51, noise, 5); nsid = 0; call((uint16_t)readPacket, 0, 0);
    const int toNoise = nsid == 5 && sidVal[4] == 0x81 && mem[0xD401] == 0x06 && mem[0xD400] == 0x40;
    publish(3, 52, both, 5); nsid = 0; call((uint16_t)readPacket, 0, 0);
    const int alone = mem[0xD404] == 0x81;
    publish(3, 53, off, 5); nsid = 0; call((uint16_t)readPacket, 0, 0);
    const int gateOff = mem[0xD404] == 0x40 && mem[sidCtrl] == 0x40;
    expect(toNoise && alone && gateOff, "noise in one control write ($81); pulse and noise both asked is noise alone; gate off is $40");
    publish(3, 54, on, 5); call((uint16_t)readPacket, 0, 0);
    call((uint16_t)mapKey, 0x88, 0);
    const int quitQuiet = c.x == 2 && mem[0xD404] == 0x40;
    publish(3, 55, on, 5); call((uint16_t)readPacket, 0, 0);
    call((uint16_t)mapKey, 0x85, 0);
    const int resetQuiet = c.x == 1 && mem[0xD404] == 0x40;
    expect(quitQuiet && resetQuiet, "F7 (quit) and F1 (Reset) take voice 1's gate off at once");
    publish(3, 56, on, 5); call((uint16_t)readPacket, 0, 0);
    /* cycle costs, for the report */
    uint8_t row[42] = { 3, 5 };
    for (int i = 0; i < 40; i++) row[2 + i] = (uint8_t)(0x01 + i);
    publish(1, 57, row, 42);
    t0 = c.cycles; call((uint16_t)readPacket, 0, 0);
    const uint64_t rowCycles = c.cycles - t0;
    memcpy(mem + sym(argv[2], "packetbuf") + 8, on, 5);
    t0 = c.cycles; call((uint16_t)sound, 0, 0);
    const uint64_t writeCycles = c.cycles - t0;
    printf("cycles: a sound packet %llu (of which the SID writes %llu), a text row packet %llu\n",
           (unsigned long long)soundCycles, (unsigned long long)writeCycles, (unsigned long long)rowCycles);
    expect(soundCycles < rowCycles / 2, "a sound packet costs under half a text row (it is 5 bytes to check, not 42)");

    /* ---- Joystick port 2 (J2) ---- */
    unsigned readStick = sym(argv[2], "readStick"), checkStick = sym(argv[2], "checkStick");
    unsigned stickv = sym(argv[2], "stick"), sentStick = sym(argv[2], "sentStick");
    mem[0xEA31] = 0xEE; mem[0xEA32] = 0xA0; mem[0xEA33] = 0x02; mem[0xEA34] = 0x60;   /* the KERNAL's handler: INC $02A0, RTS */
    mem[0xFFEA] = 0xEE; mem[0xFFEB] = 0xA1; mem[0xFFEC] = 0x02; mem[0xFFED] = 0x60;   /* UDTIM, the clock: INC $02A1, RTS */
    mem[0xEA81] = 0x60;
    mem[0xDC00] = 0x7F;                              /* as the KERNAL leaves it */
    memset(keyDown, 0, sizeof keyDown);
    stickPull = 0x01 | 0x08;                         /* up and right */
    mem[0xD019] = 0; c.p |= FLAG_I;
    call((uint16_t)irq, 0, 0);
    const int bounce = mem[stickv] == 0;
    call((uint16_t)irq, 0, 0);
    expect(bounce && mem[stickv] == 0x48 && mem[0xDC00] == 0x7F, "port 2 up-right reads as $48 (bits 3 and 6) once two frames agree; $DC00 is $7F again after");
    call((uint16_t)checkStick, 0, 0);
    const int started = (c.p & FLAG_C) && mem[pending] == 1;
    call((uint16_t)trySend, 0, 0);
    expect(started && mem[pending] == 0 && got[0] == 0 && got[1] == 0x48 && mem[sentStick] == 0x48,
           "the stick moving is an event of its own: no key, flags $48, taken by the host");
    call((uint16_t)checkStick, 0, 0);
    const int noRepeat = !(c.p & FLAG_C);
    call((uint16_t)queueEvent, 0x41, 0);
    call((uint16_t)trySend, 0, 0);
    expect(noRepeat && got[0] == 0x41 && got[1] == 0x48, "no second event for a held stick; a key's record carries the stick too ($41, flags $48)");
    stickPull = 0x10;                                /* fire */
    call((uint16_t)irq, 0, 0); call((uint16_t)irq, 0, 0);
    call((uint16_t)checkStick, 0, 0); call((uint16_t)trySend, 0, 0);
    expect(got[1] == 0x80, "fire alone is $80");

    /* phantom keys: what the KERNAL's scan would see, one column at a time (the model of the matrix) */
    memset(keyDown, 0, sizeof keyDown);
    stickPull = 0x01;                                /* up: column 0 held low */
    keyDown[0][1] = 1;                               /* RETURN: column 0, row 1 */
    const int phantomW = scanSees(1, 1);            /* scanning column 1 would also see row 1: W, a key nobody pressed */
    mem[0xDC00] = 0x7F;
    const unsigned scans = mem[0x2A0], clock = mem[0x2A1];
    call((uint16_t)irq, 0, 0);
    expect(phantomW && mem[0x2A0] == scans && mem[0x2A1] == (uint8_t)(clock + 1),
           "stick up with RETURN held: the scan would see W as well, so the scan is left out (the clock still runs)");
    keyDown[0][1] = 0;
    keyDown[3][2] = 1;                               /* G: column 3, row 2, not a column the stick pulls */
    int clean = 1;
    for (int col = 0; col < 8; col++) for (int r = 0; r < 8; r++) if (scanSees(col, r) != (col == 3 && r == 2)) clean = 0;
    mem[0xDC00] = 0x7F;
    call((uint16_t)irq, 0, 0);
    expect(clean && mem[0x2A0] == (uint8_t)(scans + 1), "stick up with G held: the scan sees exactly G, and it runs");
    memset(keyDown, 0, sizeof keyDown);
    stickPull = 0;
    keyDown[7][4] = 1; keyDown[4][4] = 1;            /* SPACE and M: columns 7 and 4, both on row 4 */
    mem[0xDC00] = 0x7F;
    uint8_t naiveCols, naiveRows;
    keyboardLines(&naiveCols, &naiveRows);           /* read with column 7 still driven, M pulls column 4 = fire */
    call((uint16_t)irq, 0, 0); call((uint16_t)irq, 0, 0);
    expect(!(naiveCols & 0x10) && mem[stickv] == 0 && mem[0x2A0] == (uint8_t)(scans + 3),
           "SPACE and M held, no stick: read with column 7 driven they fake fire; read with none driven, no stick bits");
    memset(keyDown, 0, sizeof keyDown);
    t0 = c.cycles; call((uint16_t)readStick, 0, 0);
    printf("cycles: the stick read %llu, once a frame in the timer interrupt (after CLI, so raster interrupts stay on time)\n",
           (unsigned long long)(c.cycles - t0));

    printf("%s\n", failures ? "CLIENT TEST FAILED" : "client test passed");
    return failures ? 1 : 0;
}
