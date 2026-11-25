// apple2-module.cpp: TR ][ (our 64K Apple ][ Plus) as a TeensyROM+ extension module.
//
// The Teensy runs the Apple; the C64 client (c64client/apple2-client.a) shows its text
// screen and sends keys. Built on upstream TeensyROM's module ABI (vm/abi, MIT) and
// modelled on its reference module vm/hello/hello.cpp (MIT).
//
// One file on purpose: the emulator core is included here with A2_SMALL defined, so the
// machine's layout is the same in every translation unit (the packager compiles each
// --source separately and has no -D option).
//
// Link protocol, private to this module and our client:
//   packet type 1 (module -> client): payload[0] row 0-24, payload[1] colour,
//            payload[2..41] 40 C64 screen codes (rows 0-23 the Apple screen, 24 status).
//   packet type 2 (module -> client): payload[0] the first text row: 0 all text, 20 the
//            Apple's mixed hi-res (4 text lines), 24 full hi-res (the status line stays text).
//            Above that row the client shows a hires bitmap at $2000 with its colours at $0400.
// Hi-res: with the TR+ C64 transfer (service bit 20), the module converts the Apple's shown
// hi-res page into that bitmap and writes the changed rows into C64 RAM by DMA; the client
// grants the slices. The mode packet goes out once the picture has landed. Without the
// service the Apple's text page is all the C64 shows.
//   packet type 3 (module -> client): SID voice 1, sent only when it changes (emu/tone.c, the tone
//            tracker, decides). payload[0] = the voice's control register as the client writes it:
//            $40 pulse or $80 noise, plus bit 0 the gate; payload[1..2] Fn low, high ($D400/$D401);
//            payload[3..4] pulse width low, high, 12-bit ($D402/$D403). Length 5.
//            Sound goes before everything else (one packet is in flight at a time, so a sound change
//            waits at most for one row or mode packet), and only the latest state is kept: if packets
//            back up, notes in between are dropped rather than queued, so the sound never runs late.
//   input (client -> module), protocol byte $A2: buttons = Apple key (7-bit ASCII, 0 none),
//            display = flags (1 Reset, 2 quit to the TR menu, 4 type the demo program) and joystick
//            port 2 in bits 3-7 (8 up, $10 down, $20 left, $40 right, $80 fire; every record carries
//            the stick as it is), overflow = event number; a repeated event number is ignored.
// Sound: the speaker hook feeds the tone tracker; its frames are 17,050 Apple cycles (the 60 Hz scan
// at this module's pace), closed after every pump, so a change goes out within a millisecond of its
// frame's end. Joystick (emu/stick.c): paddles 0/1 at 0, 127 or 255, fire is button 0, and in Oregon
// Trail's hunt the stick also types the aiming keys and Space.
// F5 (flag 4) types programs/cave-diver.txt into BASIC as keystrokes, when BASIC's "]"
// prompt is waiting (typed text works with any BASIC layout).
// Disk: launched from a disk image chosen in the TR menu (content_path, e.g. /OREGON.PO), the
// slot-5 block card (rom/blockcard.s) serves its 512-byte blocks through the host's file service,
// reads and writes at offsets, one block at a time; each write is flushed. emu/diskimg.c says what the
// file is (.dsk/.do 140K DOS order, .po 140K or any whole blocks, .2mg by its header) and where each
// block is in it, so a 5.25-inch image in either order reaches the card as its 280 ProDOS blocks. The list beside it
// (OREGON.BAD, emu/badlist.h) names blocks the real drive cannot read: they answer an I/O
// error, as on the Mac. Copy protection is emulated, never patched. Launched from the client
// cartridge there is no disk and the Monitor starts as before.
#define A2_SMALL
#include "../teensyrom-build/vm/abi/vm_abi.h"   // the teensyrom-build link (upstream TeensyROM)
#include "../emu/cpu6502.c"
#include "../emu/a2.c"
#include "../emu/tone.c"
#include "../emu/stick.c"
#include "../emu/diskimg.c"
#include "../build/rom-images.h"
#include "../build/demo-program.h"
#include "../emu/badlist.h"

namespace {

constexpr uint8_t kPacketRow = 1, kPacketSound = 3, kProtocol = 0xA2;
constexpr uint8_t kFlagReset = 1, kFlagQuit = 2, kFlagType = 4;
constexpr uint16_t kPrompt = 0x33;                 // the Monitor's PROMPT cell: ']' while BASIC waits
constexpr uint32_t kNoteFrames = 180;              // a status note shows for 3 seconds
const char kStatus[] = "TR ][  F1 RESET  F7 QUIT";
constexpr uint8_t kGreen = 5, kGrey = 15;
constexpr uint32_t kSlice = 1000;                  // Apple cycles per run slice
constexpr uint32_t kFrameMicros = 16667;           // screen scan, 60 Hz

const VmHost *host;
const VmHostExit *exitHost;
a2 M;                                              // the Apple, about 80K in .bss
uint8_t shown[25][40];                             // screen codes, as last sent
uint32_t dirty;                                    // one bit per row 0-24
bool awaitingAck;
uint8_t keyq[32], kqHead, kqTail;
uint8_t lastEvent; bool haveEvent;
uint32_t lastMicros, frameMicros, frames, budget;
int shownStop;
const unsigned char *typing;                       // next character of the demo, or null
uint32_t noteUntil;                                // frame when a status note goes back to kStatus
tone_tracker tones;                                // the speaker's clicks, as SID voice 1
bool soundDirty;                                   // a sound packet waits
uint8_t soundCtrl; uint16_t soundFn, soundPw;      // the latest state, as the packet carries it
uint32_t soundPackets;                             // sent, for the tests
uint8_t stickBits;                                 // port 2 as last applied (emu/stick.c)

constexpr int kMaxBadFlags = 8192;                 // the unreadable-block list covers blocks 0-8191 (4 MB)
uint32_t diskHandle;                               // the disk image on the SD card, 0 = none
bool diskWritable;
dimg disk;                                         // what the image is and where its blocks are (emu/diskimg.c)
uint8_t badFlags[kMaxBadFlags];
char diskNote[41];                                 // shown on the status line at start

// ---- hi-res on the C64 ----
constexpr uint8_t kPacketMode = 2;
constexpr uint16_t kC64Bitmap = 0x2000, kC64Screen = 0x0400;
constexpr uint32_t kSliceBytes = 128;              // per grant: the C64 stops about 128 cycles (2 lines)
constexpr int kLeft = 16;                          // Apple x 0 is C64 x 16: 280 dots in cells 2-36
const VmHostC64Dma *dmaHost;                       // null: no C64 transfer, text only
uint8_t bmNext[24 * 320], colNext[24 * 40];        // the picture as the C64 should hold it (rows 0-23)
uint8_t bmSent[24 * 320], colSent[24 * 40];        // what the C64 holds, for rows marked valid
bool rowValid[24];
uint8_t jobRows;                                   // rows the job under way carries (0..jobRows-1)
uint32_t ticket;                                   // the DMA job under way, 0 none
VmC64Span spans[48];
int wantSplit, shownSplit;                         // first text row: wanted, and as the client has it
bool modeDirty;                                    // a mode packet waits
int inflightRow = -1;                              // the text row whose packet awaits its ACK, or -1
uint32_t lastConvert;
void hiresStep();
void invalidateText(int from, int to);

uint8_t screenCode(char c) {                       // ASCII to C64 screen code (upper-case set)
    if (c >= 'a' && c <= 'z') return uint8_t(c - 96);
    if (c >= '@' && c <= '_') return uint8_t(c - 64);   // @ A-Z [ \ ] ^ _ are $00-$1F (bug 002)
    return uint8_t(c);
}

void statusLine(const char *text) {
    for (int c = 0; c < 40; ++c) shown[24][c] = 32;
    for (int c = 0; c < 40 && text[c]; ++c) shown[24][c] = screenCode(text[c]);
    dirty |= 1u << 24;
}

// Apple ][ Plus glyph (low 6 bits) maps straight onto C64 screen codes 0-63 in the
// upper-case set; $00-$3F is inverse, $40-$7F flashing, $80-$FF normal. The C64 has no
// backslash: $1C shows as a pound sign.
uint8_t c64Code(uint8_t b, bool flashOn) {
    const bool inverse = b < 0x40 || (b < 0x80 && flashOn);
    return uint8_t((b & 0x3F) | (inverse ? 0x80 : 0));
}

void scanScreen() {
    const bool flashOn = (frames / 16) & 1;
    for (int r = 0; r < 24; ++r) {
        const uint16_t base = a2_text_row(&M, r);
        for (int c = 0; c < 40; ++c) {
            const uint8_t code = c64Code(M.ram[base + c], flashOn);
            if (shown[r][c] != code) { shown[r][c] = code; dirty |= 1u << r; }
        }
    }
    if (M.stopped != shownStop) {                  // tell the user if the Apple stopped
        shownStop = M.stopped;
        if (M.stopped) {
            static const char hex[] = "0123456789ABCDEF";
            const char *prefix = M.stopped == 1 ? "STOPPED: MISSING ROM ROUTINE $" : "STOPPED: BAD OPCODE AT $";
            char line[41];
            int n = 0;
            while (prefix[n]) { line[n] = prefix[n]; ++n; }
            for (int i = 0; i < 4; ++i) line[n + i] = hex[(M.stop_pc >> (12 - 4 * i)) & 15];
            line[n + 4] = 0;
            statusLine(line);
        } else if (!noteUntil) statusLine(kStatus);
    }
    if (noteUntil && frames >= noteUntil && !M.stopped) { noteUntil = 0; statusLine(kStatus); }
}

void note(const char *text) {                      // a status-line message for 3 seconds
    statusLine(text);
    noteUntil = frames + kNoteFrames;
}

void speakerClick(void *, uint64_t cycle) { tone_click(&tones, cycle); }

void toneFrame(void *, const tone_frame *f) {      // each closed frame; a change becomes a packet
    if (!f->changed) return;
    soundCtrl = uint8_t((f->wave == TONE_NOISE ? 0x80 : 0x40) | (f->gate ? 1 : 0));
    soundFn = f->fn;
    soundPw = f->pw;
    soundDirty = true;
}

void queueKey(uint8_t key) {
    const uint8_t next = uint8_t((kqTail + 1) % 32);
    if (key && next != kqHead) { keyq[kqTail] = key; kqTail = next; }
}

void feedKey() {
    if (a2_key_waiting(&M)) return;                // the Apple has not read the last key yet
    if (kqHead != kqTail) { a2_key(&M, keyq[kqHead]); kqHead = uint8_t((kqHead + 1) % 32); return; }
    if (!typing) return;
    if (*typing) { a2_key(&M, *typing++); return; }
    typing = nullptr;
    note("TR ][  CAVE DIVER IS IN: TYPE RUN");
}

void startTyping() {
    if (typing) return;
    if (M.ram[kPrompt] != (']' | 0x80) || M.stopped) { note("F5: START BASIC FIRST (CTRL-B)"); return; }
    typing = demo_program_bin;
    statusLine("TR ][  TYPING CAVE DIVER...");
    noteUntil = 0;
}

void input(const VmInput *in) {
    if (in->protocol != kProtocol) return;
    if (haveEvent && in->overflow == lastEvent) return;
    haveEvent = true;
    lastEvent = in->overflow;
    uint8_t huntKeys[2];                           // the stick first: paddles, button, the hunt's keys
    const int nHunt = stick_apply(&M, &stickBits, uint8_t(in->display & STICK_BITS), huntKeys);
    if ((in->display & kFlagQuit) && exitHost) exitHost->exit_to_menu(0);
    if (in->display & kFlagReset) {
        kqHead = kqTail;
        if (typing) { typing = nullptr; noteUntil = frames; }   // Reset stops the typing too
        a2_reset(&M);
        tones.gate = 0;                            // the client silenced itself: the next note is news
        return;
    }
    if (in->display & kFlagType) { startTyping(); return; }
    if (typing) return;                            // keys wait until the demo is typed in
    for (int i = 0; i < nHunt; ++i) queueKey(huntKeys[i]);
    queueKey(in->buttons & 0x7F);
}

void pump() {
    const uint32_t now = host->micros_now();
    uint32_t dt = now - lastMicros;
    lastMicros = now;
    if (dt > 50000) dt = 50000;                    // after a stall, catch up at most 50 ms
    budget += dt * 1023 / 1000;                    // 1.0227 MHz
    while (budget >= kSlice && !M.stopped) {
        feedKey();
        a2_run(&M, kSlice);
        budget -= kSlice;
        if (host->should_yield()) break;
    }
    if (M.stopped) budget = 0;
    tone_advance(&tones, M.cpu.cycles);            // close the sound frames that have ended
    if (now - frameMicros >= kFrameMicros) { frameMicros = now; ++frames; scanScreen(); }
    hiresStep();
}

bool packet(VmPacket *out) {
    if (awaitingAck) return false;
    if (soundDirty) {                              // sound first: it is the one that can be late
        soundDirty = false;
        *out = {};
        out->type = kPacketSound;
        out->payload[0] = soundCtrl;
        out->payload[1] = uint8_t(soundFn);
        out->payload[2] = uint8_t(soundFn >> 8);
        out->payload[3] = uint8_t(soundPw);
        out->payload[4] = uint8_t(soundPw >> 8);
        out->length = 5;
        ++soundPackets;
        awaitingAck = true;
        return true;
    }
    if (modeDirty) {                               // the picture has landed (or text is back): switch
        modeDirty = false;
        *out = {};
        out->type = kPacketMode;
        out->payload[0] = uint8_t(wantSplit);
        out->length = 1;
        if (wantSplit < shownSplit) {              // rows going back to text: repaint them, and their
            const int to = shownSplit < 24 ? shownSplit : 24;   // colours will need sending again later
            invalidateText(wantSplit, to);
            for (int r = wantSplit; r < to; ++r) rowValid[r] = false;
        }
        inflightRow = -1;
        shownSplit = wantSplit;
        awaitingAck = true;
        return true;
    }
    const int textFrom = wantSplit > shownSplit ? wantSplit : shownSplit;   // rows above it are bitmap
    const uint32_t sendable = dirty & ~((1u << textFrom) - 1);
    if (!sendable) return false;
    const int row = __builtin_ctz(sendable);
    dirty &= ~(1u << row);
    inflightRow = row;
    *out = {};
    out->type = kPacketRow;
    out->payload[0] = uint8_t(row);
    out->payload[1] = row == 24 ? kGrey : kGreen;
    for (int c = 0; c < 40; ++c) out->payload[2 + c] = shown[row][c];
    out->length = 42;
    awaitingAck = true;
    return true;
}

void ack() { awaitingAck = false; inflightRow = -1; }

// The Apple's hi-res colours by the standard model (Apple II Reference Manual 1979: a lit dot alone shows its
// column's colour, even columns violet and odd green, the eighth bit swapping in blue and orange; two lit dots
// side by side are white): 0 black, 1 white, 2 violet, 3 green, 4 blue, 5 orange. A lone colour dot shows two
// dots wide, as on a colour TV, where the Apple's colour is 140 across: the unlit dot after it takes its colour
// (so a colour area, every other dot lit, is solid, and sparse colour keeps its weight). As the C64 shows them:
constexpr uint8_t kC64Of[6] = { 0, 1, 4, 13, 3, 8 };    // black, white, purple, light green, cyan, orange
// The same six as RGB (the C64 palette as Philip Timmermann measured it, "Pepto"), to say which of a cell's
// two colours a third one is nearer to.
constexpr uint8_t kRgbOf[6][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 111, 61, 134 }, { 154, 210, 132 },
                                   { 112, 164, 178 }, { 111, 79, 37 } };

int colourDistance(int a, int b) {
    int d = 0;
    for (int k = 0; k < 3; ++k) { const int e = int(kRgbOf[a][k]) - int(kRgbOf[b][k]); d += e * e; }
    return d;
}

// One hi-res line as model colours, C64 pixel for pixel: Apple dot x lands on C64 pixel x + kLeft.
void appleLine(int y, uint8_t *out) {
    const uint8_t *src = &M.ram[a2_hires_row(&M, y)];
    uint8_t on[284], hue[280];                     // on[2 + x], two blank dots either side; hue: dot x's colour if alone
    on[0] = on[1] = on[282] = on[283] = 0;
    for (int i = 0, x = 0; i < 40; ++i) {
        const uint8_t b = src[i], h = (b & 0x80) ? 4 : 2;    // blue/orange with the palette bit, else violet/green
        for (int bit = 0; bit < 7; ++bit, ++x) { on[2 + x] = (b >> bit) & 1; hue[x] = uint8_t(h + (x & 1)); }
    }
    for (int x = 0; x < kLeft; ++x) out[x] = 0;
    for (int x = kLeft + 280; x < 320; ++x) out[x] = 0;
    for (int x = 0; x < 280; ++x) {
        const uint8_t *o = &on[2 + x];
        uint8_t c = 0;
        if (o[0]) c = (o[-1] | o[1]) ? 1 : hue[x];
        else if (o[-1] & !o[-2]) c = hue[x - 1];   // the dot after a lone colour dot: its colour, two wide
        out[kLeft + x] = c;
    }
}

// The shown page into bmNext and colNext. A C64 hi-res cell (8 x 8 pixels) has two colours, so each cell
// takes the two model colours that cover most of it; a pixel of a third colour takes whichever of the two is
// nearer. The darker of the two (the lower number above: black first) is the cell's background, so a cell's
// colours and its bits do not swap from one picture to the next. Dots map one to one: white lines and the
// game's hi-res text keep their full width, and colour areas come out solid.
void convertHires() {
    static uint8_t line[8][320];
    for (int cy = 0; cy < 24; ++cy) {
        for (int l = 0; l < 8; ++l) appleLine(cy * 8 + l, line[l]);
        for (int cx = 0; cx < 40; ++cx) {
            // Six 8-bit counters in one word (no store-to-load chain per pixel); a table, not a variable 64-bit
            // shift, because the module links without the runtime library (-nostdlib).
            static const uint64_t kOne[6] = { 1ull, 1ull << 8, 1ull << 16, 1ull << 24, 1ull << 32, 1ull << 40 };
            uint64_t packed = 0;
            for (int l = 0; l < 8; ++l) {
                const uint8_t *q = &line[l][cx * 8];
                for (int p = 0; p < 8; ++p) packed += kOne[q[p]];
            }
            uint8_t n[6];
            for (int c = 0; c < 6; ++c) n[c] = uint8_t(packed >> (c * 8));
            int a = 0;
            for (int c = 1; c < 6; ++c) if (n[c] > n[a]) a = c;
            int b = -1;
            for (int c = 0; c < 6; ++c) if (c != a && n[c] && (b < 0 || n[c] > n[b])) b = c;
            if (b < 0) b = 0;                      // one colour only: black behind it (or black alone)
            const int bg = a < b ? a : b, fg = a < b ? b : a;
            colNext[cy * 40 + cx] = uint8_t(kC64Of[fg] << 4 | kC64Of[bg]);
            uint8_t bit[6];                        // each model colour's bit in this cell: 1 = shown in fg
            for (int c = 0; c < 6; ++c)
                bit[c] = c == fg ? fg != bg : c == bg ? 0 : n[c] && colourDistance(c, fg) < colourDistance(c, bg);
            for (int l = 0; l < 8; ++l) {
                const uint8_t *q = &line[l][cx * 8];
                bmNext[cy * 320 + cx * 8 + l] = uint8_t(bit[q[0]] << 7 | bit[q[1]] << 6 | bit[q[2]] << 5 | bit[q[3]] << 4 |
                                                        bit[q[4]] << 3 | bit[q[5]] << 2 | bit[q[6]] << 1 | bit[q[7]]);
            }
        }
    }
}

void invalidateText(int from, int to) {            // rows the client shows as text again: repaint them
    for (int r = from; r < to; ++r) { for (auto &c : shown[r]) c = 0x7F; dirty |= 1u << r; }
}

void hiresStep() {                                 // from pump: keep the C64's bitmap in step
    if (!dmaHost) return;
    if (ticket) {
        const int32_t st = dmaHost->c64_status(ticket);
        if (st == VM_C64_PENDING) return;
        ticket = 0;
        if (st == VM_C64_DONE) {
            for (int r = 0; r < jobRows; ++r) {
                for (int i = 0; i < 320; ++i) bmSent[r * 320 + i] = bmNext[r * 320 + i];
                for (int i = 0; i < 40; ++i) colSent[r * 40 + i] = colNext[r * 40 + i];
                rowValid[r] = true;
            }
        } else for (auto &v : rowValid) v = false;   // part of it may have landed: send it all again
    }
    const int split = (!M.text && M.hires) ? (M.mixed ? 20 : 24) : 0;
    if (split != wantSplit) {
        if (split < wantSplit) {                   // rows leaving the bitmap: text again, colours stale
            const int to = wantSplit < 24 ? wantSplit : 24;
            invalidateText(split, to);
            for (int r = split; r < to; ++r) rowValid[r] = false;
        }
        if (split < shownSplit) modeDirty = true;  // fewer bitmap rows than the client shows: switch now
        wantSplit = split;
    }
    if (split == 0) return;
    if (wantSplit > shownSplit && !modeDirty) {    // more bitmap rows: switch once every one has landed
        bool all = true;
        for (int r = 0; r < wantSplit; ++r) if (!rowValid[r]) all = false;
        if (all) modeDirty = true;
    }
    if (frames - lastConvert < 2) return;          // about 30 pictures a second
    lastConvert = frames;
    convertHires();
    int n = 0;
    for (int r = 0; r < split; ++r) {
        int first = -1, last = -1;
        for (int i = 0; i < 320; ++i)
            if (!rowValid[r] || bmSent[r * 320 + i] != bmNext[r * 320 + i]) { if (first < 0) first = i; last = i; }
        if (first >= 0) spans[n++] = { &bmNext[r * 320 + first], uint16_t(kC64Bitmap + r * 320 + first), uint16_t(last - first + 1) };
        first = last = -1;
        for (int i = 0; i < 40; ++i)
            if (!rowValid[r] || colSent[r * 40 + i] != colNext[r * 40 + i]) { if (first < 0) first = i; last = i; }
        if (first >= 0) spans[n++] = { &colNext[r * 40 + first], uint16_t(kC64Screen + r * 40 + first), uint16_t(last - first + 1) };
    }
    if (n == 0) return;
    if (inflightRow >= 0 && inflightRow < split) return;   // that row's text could land over the colours
    jobRows = uint8_t(split);
    ticket = dmaHost->c64_write(spans, uint32_t(n), kSliceBytes, 0);
}

const VmModule module = { VM_ABI, sizeof(VmModule), input, pump, packet, ack };

// The file at offsets, for emu/diskimg.c.
int fileRead(void *, uint32_t offset, uint8_t *buf, uint32_t len) { return host->read(diskHandle, offset, buf, len); }
int fileWrite(void *, uint32_t offset, const uint8_t *buf, uint32_t len) { return host->write(diskHandle, offset, buf, len); }

// The card's transfers: one 512-byte block, wherever the image keeps it. 0 or a ProDOS error code.
int diskIo(void *, int write, uint32_t block, uint8_t *buf) {
    if (write) {
        if (!diskWritable || disk.locked) return 0x2B; // write protected
        if (dimg_write_block(&disk, block, buf) != 0) return 0x27;
        VmFsRequest flush{ VmFsOp::Flush, diskHandle, 0, 0, nullptr, nullptr };
        return host->file_op(&flush) == 0 ? 0 : 0x27;
    }
    return dimg_read_block(&disk, block, buf) == 0 ? 0 : 0x27;
}

void setNote(const char *a, const char *b) {       // diskNote = a + b, at most 40 characters
    int n = 0;
    for (const char *p = a; *p && n < 40; ++p) diskNote[n++] = *p;
    for (const char *p = b; p && *p && n < 40; ++p) diskNote[n++] = *p;
    diskNote[n] = 0;
}

void openDisk(const VmHost *h) {
    const char *path = h->content_path;
    if (!path || !path[0]) return;                 // launched from the client cartridge: no disk
    const char *name = path;                       // the file name alone, for the status line
    for (const char *p = path; *p; ++p) if (*p == '/') name = p + 1;
    VmFileInfo info{};
    uint32_t handle = 0;
    diskWritable = false;
    if ((h->services & VM_SERVICE_WRITE) && h->open_flags) {
        handle = h->open_flags(path, VM_OPEN_READ | VM_OPEN_WRITE, &info);
        diskWritable = handle != 0;
    }
    if (!handle) handle = h->open(path, &info);
    if (!handle) { setNote("DISK: CANNOT OPEN ", name); return; }
    diskHandle = handle;                           // fileRead needs it for the header
    uint8_t head[DIMG_HEAD_BYTES];
    const uint32_t headLen = info.bytes < DIMG_HEAD_BYTES ? uint32_t(info.bytes) : DIMG_HEAD_BYTES;
    if ((headLen && h->read(handle, 0, head, headLen) != int32_t(headLen)) ||
        dimg_open(&disk, name, uint32_t(info.bytes), head, headLen, fileRead, fileWrite, nullptr) != 0) {
        diskHandle = 0;
        h->close(handle);
        setNote("DISK: NOT AN IMAGE: ", name);
        return;
    }
    const int blocks = int(disk.blocks);
    const int nflags = blocks < kMaxBadFlags ? blocks : kMaxBadFlags;
    int nbad = 0;
    bool haveList = false, listBad = false;
    char listPath[400];
    if (a2_badlist_path(path, listPath, sizeof listPath)) {
        VmFileInfo li{};
        const uint32_t lh = h->open(listPath, &li);
        if (lh) {
            static char text[2048];
            const int32_t n = li.bytes <= sizeof text ? h->read(lh, 0, text, li.bytes) : -1;
            h->close(lh);
            nbad = n >= 0 ? a2_parse_badlist(text, uint32_t(n), badFlags, nflags) : -1;
            if (nbad < 0) { for (auto &f : badFlags) f = 0; nbad = 0; listBad = true; }
            else haveList = true;
        }
    }
    diskHandle = handle;
    a2_attach_blockdev(&M, 5, blockcard_bin, blocks, haveList ? badFlags : nullptr, nflags, diskIo, nullptr);
    char count[24];                                // "  1 BAD BLOCK", "  2 BAD BLOCKS"
    int n = 0;
    const char *lead = listBad ? " BAD LIST UNREADABLE" : "";
    if (!listBad) {
        count[n++] = ' ';
        char digits[6]; int d = 0, v = nbad;
        do { digits[d++] = char('0' + v % 10); v /= 10; } while (v);
        while (d) count[n++] = digits[--d];
        const char *word = nbad == 1 ? " BAD BLOCK" : " BAD BLOCKS";
        for (const char *p = word; *p; ++p) count[n++] = *p;
    }
    count[n] = 0;
    setNote(diskWritable && !disk.locked ? "DISK " : "DISK (READ ONLY) ", name);
    int len = 0; while (diskNote[len]) ++len;
    for (const char *p = listBad ? lead : count; *p && len < 40; ++p) diskNote[len++] = *p;
    diskNote[len] = 0;
}

}  // namespace

VM_MODULE_ENTRY const VmModule *vm_entry(const VmHost *h) {
    if (!h || h->abi != VM_ABI || h->bytes < VM_HOST_BASE_BYTES) return nullptr;
    if ((h->services & VM_SERVICES) != VM_SERVICES) return nullptr;
    host = h;
    exitHost = (h->bytes >= VM_HOST_EXIT_BYTES && (h->services & VM_SERVICE_EXIT))
                   ? reinterpret_cast<const VmHostExit *>(h) : nullptr;
    dmaHost = (h->bytes >= VM_HOST_C64_DMA_BYTES && (h->services & (VM_SERVICE_C64_DMA | VM_SERVICE_EXIT))
                   == (VM_SERVICE_C64_DMA | VM_SERVICE_EXIT)) ? reinterpret_cast<const VmHostC64Dma *>(h) : nullptr;
    for (auto &v : rowValid) v = false;
    ticket = 0;
    wantSplit = shownSplit = 0;
    modeDirty = false;
    inflightRow = -1;
    lastConvert = 0;
    a2_init(&M, tr_rom_bin);
    a2_load_ext(&M, tr_ext_bin);
    tone_init(&tones, TONE_FRAME_NUM, TONE_FRAME_DEN, TONE_CLOCK_NTSC_NUM, TONE_CLOCK_NTSC_DEN, toneFrame, nullptr);
    M.speaker_hook = speakerClick;
    M.speaker_ctx = nullptr;
    soundDirty = false;
    soundPackets = 0;
    stickBits = 0;
    a2_set_paddle(&M, 0, 127);                     // a digital stick at rest: both axes centred
    a2_set_paddle(&M, 1, 127);
    diskHandle = 0;
    diskNote[0] = 0;
    openDisk(h);
    for (int r = 0; r < 25; ++r) for (int c = 0; c < 40; ++c) shown[r][c] = 32;
    dirty = (1u << 25) - 1;                        // paint everything once
    shownStop = -1;                                // forces the status line on the first scan
    awaitingAck = haveEvent = false;
    kqHead = kqTail = 0;
    typing = nullptr;
    noteUntil = 0;
    lastMicros = frameMicros = h->micros_now();
    budget = frames = 0;
    if (diskNote[0]) note(diskNote);               // which disk, for the first 3 seconds
    return &module;
}
