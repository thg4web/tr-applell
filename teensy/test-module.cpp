// test-module.cpp: run the real apple2-module.cpp on the Mac against upstream TeensyROM's
// native fake host (vm/tests/native_host.h, MIT), the way the firmware would drive it.
// Usage: build/test-module <sandbox-dir>     (make module-test builds and runs it)
// A fake clock advances 1 ms per pump, so a run is quick and repeatable.
#include "native_host.h"
#include "apple2-module.cpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static uint32_t fakeNow;
static uint32_t fakeMicros() { return fakeNow; }
static const VmModule *mod;
static char screen[25][41];
static uint8_t raw[25][40];                        // the screen codes as sent, undecoded
static uint8_t eventNo;
static int failures;
struct SoundPacket { uint8_t ctrl; uint16_t fn, pw; uint64_t cycle; };
static std::vector<SoundPacket> sounds;            // every type 3 packet, with the Apple cycle count it left at
static std::vector<uint64_t> clicks;               // the module's speaker clicks, recorded on the way to its tracker
static void recordClick(void *ctx, uint64_t cycle) { clicks.push_back(cycle); speakerClick(ctx, cycle); }
// The same tracker run here over the recorded clicks: the changes the module should have sent.
static std::vector<SoundPacket> offline;
static void offlineFrame(void *, const tone_frame *f) {
    if (f->changed) offline.push_back({ uint8_t((f->wave == TONE_NOISE ? 0x80 : 0x40) | f->gate), f->fn, f->pw, f->end });
}

// A fake C64 for the hi-res path: text packets land in its screen RAM as the client puts them, and a fake
// C64 transfer (service bit 20) copies a job's spans when its status is asked for the second time, so the
// module sees PENDING first and its sources are read late, as on the real host.
static uint8_t c64[65536];
static int lastSplit = -1, modePackets;
static const VmC64Span *jobSpans;
static uint32_t jobCount, fakeTicket, jobPolls, dmaJobs, dmaBytes;
static uint32_t fakeC64Write(const VmC64Span *spans, uint32_t count, uint32_t slice, uint32_t flags) {
    if (!count || count > VM_C64_SPANS_MAX || flags || !slice || jobSpans) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t a = spans[i].address, end = a + spans[i].bytes;
        if (!spans[i].bytes || end > 0x10000 || (a < 0xE000 && end > 0xDE00)) return 0;
    }
    jobSpans = spans; jobCount = count; jobPolls = 0;
    return ++fakeTicket;
}
static int32_t fakeC64Status(uint32_t ticket) {
    if (ticket != fakeTicket) return VM_C64_UNKNOWN;
    if (!jobSpans) return VM_C64_DONE;
    if (++jobPolls < 2) return VM_C64_PENDING;
    for (uint32_t i = 0; i < jobCount; ++i) {
        std::memcpy(c64 + jobSpans[i].address, jobSpans[i].source, jobSpans[i].bytes);
        dmaBytes += jobSpans[i].bytes;
    }
    jobSpans = nullptr;
    ++dmaJobs;
    return VM_C64_DONE;
}
// Is Apple dot (x, y) on the shown hi-res page lit, and is C64 bitmap pixel (x, y) set?
static bool appleDot(int x, int y) { return (M.ram[a2_hires_row(&M, y) + x / 7] >> (x % 7)) & 1; }
static bool c64Dot(int x, int y) { return (c64[0x2000 + (y >> 3) * 320 + (x >> 3) * 8 + (y & 7)] >> (7 - (x & 7))) & 1; }
// The fake C64's picture (bitmap rows 0-23, colours from screen RAM) as a PPM, to look at (C64 palette:
// the "Pepto" measurements, published by Philip Timmermann).
static void saveC64View(const std::filesystem::path &file) {
    static const uint8_t pal[16][3] = { {0,0,0},{255,255,255},{104,55,43},{112,164,178},{111,61,134},{88,141,67},
        {53,40,121},{184,199,111},{111,79,37},{67,57,0},{154,103,89},{68,68,68},{108,108,108},{154,210,132},
        {108,94,181},{149,149,149} };
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "P6\n320 192\n255\n";
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 320; ++x) {
            const uint8_t col = c64[0x0400 + (y >> 3) * 40 + (x >> 3)];
            const uint8_t *rgb = pal[c64Dot(x, y) ? col >> 4 : col & 15];
            out.write(reinterpret_cast<const char *>(rgb), 3);
        }
}
// The Apple's colour at dot x of line y by the standard model, worked out here on its own (not with the module's
// code): a lit dot next to a lit dot is white, a lone lit dot is its column's colour (palette bit: blue/orange,
// else violet/green) and shows two dots wide, so the unlit dot after it takes its colour. As C64 colours: black 0,
// white 1, violet 4, green 13, blue 3, orange 8. Dot x shows at C64 pixel x + 16.
static uint8_t appleC64Colour(int x, int y) {
    if (x < 0 || x >= 280) return 0;
    auto on = [&](int i) { return i >= 0 && i < 280 && appleDot(i, y); };
    auto col = [&](int i) {
        const bool pal = M.ram[a2_hires_row(&M, y) + i / 7] >> 7;
        return uint8_t(pal ? ((i & 1) ? 8 : 3) : ((i & 1) ? 13 : 4));
    };
    if (on(x)) return (on(x - 1) || on(x + 1)) ? 1 : col(x);
    if (on(x - 1) && !on(x - 2)) return col(x - 1);
    return 0;
}
// Pixels the C64 shows in another colour than the Apple's, counting only cells that hold one or two model colours
// (a C64 hi-res cell has two); *clash gets the number of cells with three or more, where some pixels must give.
static int bitmapMismatches(int *clash = nullptr) {
    int bad = 0, clashes = 0;
    for (int cy = 0; cy < 24; ++cy)
        for (int cx = 0; cx < 40; ++cx) {
            int seen = 0, kinds = 0, wrong = 0;
            for (int y = cy * 8; y < cy * 8 + 8; ++y)
                for (int x = cx * 8; x < cx * 8 + 8; ++x) {
                    const uint8_t want = appleC64Colour(x - 16, y), col = c64[0x0400 + cy * 40 + cx];
                    const uint8_t got = c64Dot(x, y) ? col >> 4 : col & 15;
                    if (!(seen & (1 << want))) { seen |= 1 << want; ++kinds; }
                    if (got != want) ++wrong;
                }
            if (kinds >= 3) ++clashes; else bad += wrong;
        }
    if (clash) *clash = clashes;
    return bad;
}

static char ascii(uint8_t code) { code &= 0x7F; return code < 0x20 ? char('@' + code) : char(code); }

static void run(double seconds) {
    for (long i = 0; i < long(seconds * 1000); ++i) {
        fakeNow += 1000;
        mod->pump();
        VmPacket p;
        while (mod->packet(&p)) {
            const int row = p.payload[0];
            if (p.type == 2 && p.length == 1) { lastSplit = p.payload[0]; ++modePackets; }
            else if (p.type == 3 && p.length == 5)
                sounds.push_back({ p.payload[0], uint16_t(p.payload[1] | p.payload[2] << 8),
                                   uint16_t(p.payload[3] | p.payload[4] << 8), M.cpu.cycles });
            else if (row < 25 && p.length == 42)
                for (int c = 0; c < 40; ++c) {
                    raw[row][c] = p.payload[2 + c]; screen[row][c] = ascii(p.payload[2 + c]);
                    c64[0x0400 + row * 40 + c] = p.payload[2 + c];
                }
            mod->ack();
        }
    }
}

static uint8_t stickNow;                            // the joystick bits every record carries, as the client does
static void send(uint8_t key, uint8_t flags) {
    const VmInput in{ key, uint8_t(flags | stickNow), ++eventNo, 0xA2 };
    mod->input(&in);
}
static void stick(uint8_t bits) { stickNow = bits; send(0, 0); }   // the client's record when the stick moves

// The Oregon Trail play-through's keys (build/oregon-keys.txt, from tests/oregon-keys.sh) played the way
// a2term plays --keys: a key goes in once the Apple has taken the one before; \w waits one Apple second
// (60 x 17,030 cycles). Stops before the first key of the tail it is given.
static void runCycles(uint64_t n) { const uint64_t end = M.cpu.cycles + n; while (M.cpu.cycles < end && !M.stopped) run(0.001); }
static void playKeys(const std::string &keys) {
    for (size_t i = 0; i < keys.size(); ++i) {
        uint8_t k = uint8_t(keys[i]);
        if (k == '\\' && i + 1 < keys.size()) {
            const char e = keys[++i];
            if (e == 'w') { runCycles(60u * 17030u); continue; }
            k = e == 'r' ? '\r' : uint8_t(e);
        }
        send(k, 0);
        for (int t = 0; t < 2000 && (kqHead != kqTail || a2_key_waiting(&M)); ++t) run(0.001);
        runCycles(17030);
    }
}

static void type(const char *text) {
    for (const char *p = text; *p; ++p) { send(uint8_t(*p == '\n' ? '\r' : *p), 0); run(0.05); }
}

static bool onScreen(const char *text) {
    for (auto &row : screen) if (std::string(row).find(text) != std::string::npos) return true;
    return false;
}

static void expect(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void dump() {
    std::printf("+----------------------------------------+\n");
    for (auto &row : screen) std::printf("|%s|\n", row);
    std::printf("+----------------------------------------+\n");
}

int main(int argc, char **argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: test-module <sandbox-dir>\n"); return 2; }
    const std::filesystem::path sandbox = std::filesystem::absolute(argv[1]);
    const std::filesystem::path bootdisk = std::filesystem::absolute("build/bootdisk.po");
    const std::filesystem::path oregon = std::filesystem::absolute("build/sd/OREGON.PO");
    const std::filesystem::path oregonKeys = std::filesystem::absolute("build/oregon-keys.txt");
    const std::filesystem::path package = sandbox / "VMS/APPLE2";
    std::filesystem::create_directories(package);
    std::filesystem::current_path(package.root_path());
    const std::string root = "/" + package.relative_path().generic_string();
    for (auto &row : screen) { std::memset(row, ' ', 40); row[40] = 0; }

    std::vector<uint8_t> workspace(64 * 1024), guest(VM_RAM_BYTES);
    VmHostExit host = VmNativeHost::makeWithExit(root, "", workspace.data(), uint32_t(workspace.size()),
                                                 guest.data(), uint32_t(guest.size()));
    host.base.micros_now = fakeMicros;
    mod = vm_entry(&host.base);
    expect(mod != nullptr, "module accepts the host");
    if (!mod) return 1;
    std::printf("module .bss for the Apple: %zu bytes\n", sizeof(M));

    run(1.5);
    expect(onScreen("TR ][ MONITOR 0.3"), "boots to our banner");
    expect(screen[2][0] == '*', "shows the * prompt");
    expect(onScreen("TR ][  F1 RESET  F7 QUIT"), "status line on row 24");
    // The decoder above reads $5D as ']' too, so check the codes themselves (bug 002).
    expect(raw[24][3] == 0x1D && raw[24][4] == 0x1B,   // "TR ][": columns 3 and 4
           "status line sends ][ as C64 screen codes $1D $1B (bug 002)");

    // ---- Sound (S3): the bell as type 3 packets. Fn 15330 = 2^24 x 910 / (912 x 1092), worked with bc in test-tone.sh
    auto bellPair = [](size_t i) {
        return sounds.size() >= i + 2 && sounds[i].ctrl == 0x41 && sounds[i].fn == 15330 && sounds[i].pw == 2048 &&
               sounds[i + 1].ctrl == 0x40 && sounds[i + 1].fn == 15330 && sounds[i + 1].pw == 2048;
    };
    expect(sounds.size() == 2 && bellPair(0), "the power-on beep: a sound packet with the gate on (pulse, Fn 15330, width 2048), then one with it off");
    type("\x07\x18");                             // Ctrl-G rings the bell; Ctrl-X clears the line again
    run(0.5);
    expect(sounds.size() == 4 && bellPair(2), "Ctrl-G at the Monitor: the same two packets again");
    if (sounds.size() >= 4)
        std::printf("the bell on the link: gate on for %.1f ms of Apple time (the beep is 107 ms; frames are 16.7 ms)\n",
                    double(sounds[3].cycle - sounds[2].cycle) / 1020.4844);

    // ---- Joystick (J2): port 2's bits to paddles 0/1 and button 0; outside the hunt it types nothing ----
    {
        char before[25][41];
        std::memcpy(before, screen, sizeof before);
        stick(0x08 | 0x20); run(0.05);
        const bool upLeft = M.paddle[0] == 0 && M.paddle[1] == 0 && !M.button[0];
        stick(0x10 | 0x40 | 0x80); run(0.05);
        const bool downRightFire = M.paddle[0] == 255 && M.paddle[1] == 255 && M.button[0];
        stick(0); run(0.2);
        const bool centre = M.paddle[0] == 127 && M.paddle[1] == 127 && !M.button[0];
        expect(upLeft && downRightFire && centre, "stick bits to paddles and button: up-left 0,0; down-right 255,255 with fire; centre 127,127");
        expect(kqHead == kqTail && !a2_key_waiting(&M) && std::memcmp(before, screen, sizeof before) == 0,
               "at the Monitor the stick types nothing (no key queued, the screen unchanged)");
        // The hunt's keys, through the module's input: as if the Apple had just read the keyboard at $EC21 from
        // language-card RAM (Oregon Trail's hunt; a2term plays the real hunt with the same emu/stick.c).
        M.key_read_pc = STICK_HUNT_PC; M.key_read_lc = 1; M.key_read_cycle = M.cpu.cycles;
        stick(0x40);
        const bool right = kqTail == uint8_t((kqHead + 1) % 32) && keyq[kqHead] == ';';
        kqHead = kqTail;
        stick(0x40 | 0x80);
        const bool fire = kqTail == uint8_t((kqHead + 1) % 32) && keyq[kqHead] == ' ';
        kqHead = kqTail;
        stick(0x40);
        const bool letGo = kqHead == kqTail;
        stick(0x08 | 0x20);
        const bool upLeft2 = kqTail == uint8_t((kqHead + 1) % 32) && keyq[kqHead] == 'I';
        kqHead = kqTail;
        stick(0);
        const bool centred = kqHead == kqTail;
        expect(right && fire && letGo && upLeft2 && centred,
               "in the hunt the module types ';' for right, Space for fire, nothing on letting go, I for up-left, nothing for centre");
        run(0.1);                                  // the Monitor reads the keyboard again: the hunt is over
        stick(0x40);
        expect(!stick_hunting(&M) && kqHead == kqTail, "once the Apple reads keys anywhere else, the stick types nothing again");
        stick(0);
        stick(0x40);                               // right: PREAD reads 255 on paddle 0, 127 on paddle 1
        type("300:A2 00 20 1E FB A2 01 84 06 20 1E FB 00\n300G\n");
        run(0.5);
        expect(onScreen("Y=7F") && M.ram[6] == 0xFF, "the Apple reads the stick: PREAD gives 255 (right) on paddle 0, 127 on paddle 1");
        stick(0);
        run(0.1);
    }

    send(0, 4);                                    // F5 at the Monitor's * prompt: not typed
    run(0.1);
    expect(onScreen("F5: START BASIC FIRST (CTRL-B)"), "F5 at the Monitor says start BASIC first");
    run(3.5);
    expect(onScreen("TR ][  F1 RESET  F7 QUIT"), "the note goes back to the status line");

    type("300:A9 C1 20 ED FD 00\n300G\n");
    run(0.5);
    expect(onScreen("0305- A=C1"), "keys reach the Apple: 300G prints A and the BRK registers");

    send(0, 1);                                    // F1: Apple Reset (warm start)
    run(0.5);
    type("302\n");
    run(0.3);
    expect(onScreen("0302- 20"), "Reset is a warm start: memory kept, Monitor answers");

    type("\x02\nPRINT 2+2\n");                     // Ctrl-B: BASIC, the way it is typed on the C64 (CTRL+B)
    run(4.0);
    expect(onScreen("]PRINT 2+2"), "Ctrl-B starts BASIC");
    bool four = false;
    for (auto &row : screen) if (std::string(row).rfind("4 ", 0) == 0) four = true;
    expect(four, "BASIC answers PRINT 2+2 with 4");

    send(0, 4);                                    // F5 at BASIC's ] prompt: types CAVE DIVER in
    run(5.0);
    expect(onScreen("CAVE DIVER IS IN: TYPE RUN"), "F5 types the demo into BASIC");
    type("LIST 10\n");
    run(0.5);
    expect(onScreen("10  REM  CAVE DIVER, BY TRACY"), "the demo is in BASIC's memory (LIST 10)");
    type("RUN\n");
    run(0.5);
    expect(onScreen("PRESS 1 EASY, 2 MEDIUM, 3 HARD"), "the demo runs");
    send(0, 1);                                    // F1: back to the Monitor for the tests below
    run(0.5);

    expect(VmNativeHost::exitCalls == 0, "no exit yet");
    send(0, 2);                                    // F7: quit to the TR menu
    expect(VmNativeHost::exitCalls == 1, "F7 asks the host to exit");

    send(0, 2);                                    // the same event number twice is ignored
    eventNo--;
    send(0, 2);
    expect(VmNativeHost::exitCalls == 2, "a repeated event number is ignored");

    // ---- Disk: launched from a block image chosen in the TR menu (content_path) ----
    // The boot test disk padded to 16 blocks, with a list beside it naming block 9 unreadable. Files are
    // written fresh into the sandbox each run, so the card's writes never reach build/.
    {
        std::ifstream in(bootdisk, std::ios::binary);
        std::vector<char> img((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        img.resize(16 * 512, 0);
        std::ofstream(sandbox / "BOOT.PO", std::ios::binary | std::ios::trunc).write(img.data(), std::streamsize(img.size()));
        std::ofstream(sandbox / "BOOT.BAD", std::ios::trunc) << "# test list\n9\n";
    }
    for (auto &row : screen) { std::memset(row, ' ', 40); row[40] = 0; }
    host = VmNativeHost::makeWithExit(root, (sandbox / "BOOT.PO").string(), workspace.data(), uint32_t(workspace.size()),
                                      guest.data(), uint32_t(guest.size()));
    host.base.micros_now = fakeMicros;
    mod = vm_entry(&host.base);
    run(1.0);
    expect(onScreen("DISK BOOT.PO 1 BAD BLOCK"), "a disk from the TR menu: its name and the list on the status line");
    expect(onScreen("BOOT OK FROM BLOCK 0") && onScreen("BLOCK 1 VIA PRODOS DRIVER") && onScreen("BLOCK 2 VIA SMARTPORT"),
           "the slot-5 card boots it from the file: ProDOS and SmartPort reads");
    // A Monitor program: write block 3 from $1000 ("ABC" at its start), then read block 9 and block 4.
    type("1000:C1 C2 C3\n");
    type("300:A9 02 85 42 A9 50 85 43 A9 00 85 44 A9 10 85 45 A9 03 85 46 A9 00 85 47 20 40 C5 00\n300G\n");
    run(0.5);
    expect(onScreen("031B- A=00"), "a block write answers no error");
    std::vector<char> back(512);
    std::ifstream(sandbox / "BOOT.PO", std::ios::binary).seekg(3 * 512).read(back.data(), 512);
    expect(uint8_t(back[0]) == 0xC1 && uint8_t(back[1]) == 0xC2 && uint8_t(back[2]) == 0xC3, "the write reached the file (block 3)");
    type("301:01\n311:09\n300G\n");
    run(0.5);
    expect(onScreen("031B- A=27"), "block 9, on the list, answers an I/O error ($27)");
    type("311:04\n300G\n");
    run(0.5);
    bool ok4 = false;
    for (int r = 0; r < 24; ++r) if (std::string(screen[r]).find("031B- A=00") != std::string::npos) ok4 = true;
    expect(ok4 && M.disk_errors == 1, "block 4, not on it, reads (one error in all)");
    expect(M.nbad_reads == 1 && M.bad_reads[0] == 9, "the module logs block 9 as asked for");

    // ---- D1: 5.25-inch images straight off the card, through emu/diskimg.c ----
    // The test disk again as a 140K disk: in DOS order (.DSK), and in a locked .2MG; plus a .DSK of the wrong size.
    {
        std::ifstream in(bootdisk, std::ios::binary);
        std::vector<char> po((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        po.resize(143360, 0);
        static const int skew[16] = { 0, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 15 };   // DOS sector -> physical
        std::vector<char> dos(143360);
        for (int t = 0; t < 35; ++t)
            for (int sct = 0; sct < 16; ++sct)
                for (int i = 0; i < 16; ++i)
                    if ((i == 15 ? 15 : (2 * i) % 15) == skew[sct])                              // ProDOS-order sector -> physical
                        std::memcpy(&dos[(t * 16 + sct) * 256], &po[(t * 16 + i) * 256], 256);
        std::ofstream(sandbox / "BOOT140.DSK", std::ios::binary | std::ios::trunc).write(dos.data(), std::streamsize(dos.size()));
        std::vector<char> g(64, 0);
        std::memcpy(g.data(), "2IMGTEST", 8); g[8] = 64; g[10] = 1;
        g[19] = char(0x80);                                // flags bit 31: locked
        g[24] = 64; g[28] = char(143360 & 0xFF); g[29] = char((143360 >> 8) & 0xFF); g[30] = char(143360 >> 16);
        g.insert(g.end(), dos.begin(), dos.end());          // format 0: DOS order
        std::ofstream(sandbox / "BOOT140.2MG", std::ios::binary | std::ios::trunc).write(g.data(), std::streamsize(g.size()));
        std::ofstream(sandbox / "JUNK.DSK", std::ios::binary | std::ios::trunc).write(dos.data(), 1000);
        std::ofstream(sandbox / "TEST.A2M", std::ios::binary | std::ios::trunc).write(po.data(), 3 * 512);   // a package's own extension
        struct Case { const char *file, *note; } cases[] = { { "TEST.A2M", "DISK TEST.A2M 0 BAD BLOCKS" },
                                                             { "BOOT140.DSK", "DISK BOOT140.DSK 0 BAD BLOCKS" },
                                                             { "BOOT140.2MG", "DISK (READ ONLY) BOOT140.2MG 0 BAD BLOCK" } };   // the note stops at 40 columns
        for (const Case &c : cases) {
            for (auto &row : screen) { std::memset(row, ' ', 40); row[40] = 0; }
            host = VmNativeHost::makeWithExit(root, (sandbox / c.file).string(), workspace.data(), uint32_t(workspace.size()),
                                              guest.data(), uint32_t(guest.size()));
            host.base.micros_now = fakeMicros;
            mod = vm_entry(&host.base);
            run(1.0);
            const bool booted = onScreen("BOOT OK FROM BLOCK 0") && onScreen("BLOCK 1 VIA PRODOS DRIVER") && onScreen("BLOCK 2 VIA SMARTPORT");
            char what[160];
            std::snprintf(what, sizeof what, "%s from the card: \"%s\", and the card boots it through the layer (ProDOS and SmartPort reads)", c.file, c.note);
            expect(onScreen(c.note) && booted && disk.blocks == (std::strstr(c.file, ".A2M") ? 3u : 280u), what);
        }
        type("300:A9 02 85 42 A9 50 85 43 A9 00 85 44 A9 10 85 45 A9 03 85 46 A9 00 85 47 20 40 C5 00\n300G\n");
        run(0.5);
        expect(onScreen("031B- A=2B"), "a locked .2MG refuses a block write: write protected ($2B)");
        host = VmNativeHost::makeWithExit(root, (sandbox / "JUNK.DSK").string(), workspace.data(), uint32_t(workspace.size()),
                                          guest.data(), uint32_t(guest.size()));
        host.base.micros_now = fakeMicros;
        mod = vm_entry(&host.base);
        run(1.0);
        expect(onScreen("DISK: NOT AN IMAGE: JUNK.DSK") && !M.card_slot, "a .DSK of the wrong size: refused on the status line, no disk attached");
    }

    // Oregon Trail (your own copy, never part of the repo) from the SD card's files, when make has built them.
    if (std::filesystem::exists(oregon) && std::filesystem::exists(oregon.parent_path() / "OREGON.BAD")) {
        std::filesystem::copy_file(oregon, sandbox / "OREGON.PO", std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(oregon.parent_path() / "OREGON.BAD", sandbox / "OREGON.BAD",
                                   std::filesystem::copy_options::overwrite_existing);
        for (auto &row : screen) { std::memset(row, ' ', 40); row[40] = 0; }
        host = VmNativeHost::makeWithExit(root, (sandbox / "OREGON.PO").string(), workspace.data(),
                                          uint32_t(workspace.size()), guest.data(), uint32_t(guest.size()));
        host.base.micros_now = fakeMicros;
        mod = vm_entry(&host.base);
        run(1.0);
        expect(onScreen("DISK OREGON.PO 1 BAD BLOCK"), "Oregon Trail from the SD files: the status line names it");
        run(29.0);
        expect(M.hires && M.page2 && !M.text, "Oregon Trail boots from the SD files to its hi-res menu (page 2)");
        expect(M.nbad_reads == 1 && M.bad_reads[0] == 8 && M.disk_errors == 1, "its protection asks for block 8, which fails as on the real drive");
        expect(M.disk_reads > 100 && M.stopped == 0, "it loads its programs and runs on");
        std::printf("Oregon Trail in the module: %u block reads, %d failed\n", M.disk_reads, M.disk_errors);
        // The whole play-through in the module, sound included, then the stick in its hunt.
        std::ifstream kf(oregonKeys);
        const std::string keys((std::istreambuf_iterator<char>(kf)), std::istreambuf_iterator<char>());
        if (!keys.empty()) {
            // Launched from a package's own extension (OREGON.A2M, a copy beside OREGON.PO; its list is still OREGON.BAD).
            std::filesystem::copy_file(oregon, sandbox / "OREGON.A2M", std::filesystem::copy_options::overwrite_existing);
            host = VmNativeHost::makeWithExit(root, (sandbox / "OREGON.A2M").string(), workspace.data(),
                                              uint32_t(workspace.size()), guest.data(), uint32_t(guest.size()));
            host.base.micros_now = fakeMicros;
            mod = vm_entry(&host.base);
            sounds.clear();
            clicks.clear();
            M.speaker_hook = recordClick;
            stickNow = 0;
            const size_t name = keys.find("OSCAR");
            expect(M.card_slot == 5 && disk.blocks == 1600 && M.disk_bad && M.disk_bad[8], "OREGON.A2M attaches as Oregon Trail's 1,600 blocks, block 8 on the list");
            playKeys(keys.substr(0, name));             // to the first name prompt
            run(0.5);
            const uint8_t keyBefore = M.key;
            for (uint8_t b : { 0x08, 0x48, 0x80, 0x00 }) { stick(b); run(0.1); }
            expect(M.key == keyBefore && kqHead == kqTail && !stick_hunting(&M),
                   "at Oregon Trail's name prompt the stick types nothing (the game reads keys in main RAM there, not at the hunt's $EC21)");
            playKeys(keys.substr(name));                // the rest, the hunt's own keys included
            run(1.0);
            // The game's random events follow the exact cycle each key arrives (the Apple stirs its seed while it
            // waits for one), and the module feeds keys between its 1,000-cycle slices where a2term feeds them per
            // 17,030-cycle step, so the same keys take a different trail here: the count differs a little from
            // a2term's 115. What must hold exactly: every change the tracker finds in the module's own clicks is a
            // packet, in order, with nothing lost or added on the way.
            static tone_tracker check;
            offline.clear();
            tone_init(&check, TONE_FRAME_NUM, TONE_FRAME_DEN, TONE_CLOCK_NTSC_NUM, TONE_CLOCK_NTSC_DEN, offlineFrame, nullptr);
            for (uint64_t c : clicks) tone_click(&check, c);
            tone_advance(&check, M.cpu.cycles);
            bool same = offline.size() == sounds.size();
            for (size_t i = 0; same && i < sounds.size(); ++i)
                same = offline[i].ctrl == sounds[i].ctrl && offline[i].fn == sounds[i].fn && offline[i].pw == sounds[i].pw &&
                       sounds[i].cycle >= offline[i].cycle && sounds[i].cycle - offline[i].cycle < 2100;
            std::printf("Oregon Trail in the module: %zu speaker clicks, %zu sound packets (a2term's play-through: 115)\n",
                        clicks.size(), sounds.size());
            expect(sounds.size() > 50 && same,
                   "Oregon Trail's tunes on the link: every change in the module's clicks is a packet, in order, within 2 ms of its frame");
        }
    } else std::printf("skip Oregon Trail in the module (make build/sd/OREGON.PO)\n");

    // ---- The hi-res converter on its own: two colours per C64 cell, the two that cover most of it ----
    {
        std::memset(&M.ram[0x2000], 0, 0x2000);
        M.text = 0; M.hires = 1; M.page2 = 0; M.mixed = 0;
        // Cell (row 0, column 4) is Apple dots 16-23, bytes 2 and 3. Orange is the palette bit with every odd dot lit
        // ($D5 in odd bytes, $AA in even ones); white is every dot lit. Column 4: lines 0-1 white, line 2 black,
        // lines 3-7 orange. Column 6 (dots 32-39, bytes 4 and 5) on cell row 1: lines 0-2 white, 3-7 orange (a hoof
        // over the ground: the old rule made it white, and the ground white stripes).
        auto put = [](int y, int i, uint8_t v) { M.ram[a2_hires_row(&M, y) + i] = v; };
        for (int i = 1; i <= 6; ++i) {
            const uint8_t orange = (i & 1) ? 0xD5 : 0xAA;
            for (int y = 0; y < 8; ++y) put(y, i, y < 2 ? 0xFF : y == 2 ? 0x00 : orange);
            for (int y = 8; y < 16; ++y) put(y, i, y < 11 ? 0xFF : orange);
        }
        convertHires();
        const uint8_t *a = &bmNext[0 * 320 + 4 * 8], *b = &bmNext[1 * 320 + 6 * 8];
        const bool three = colNext[4] == 0x81 && a[0] == 0x00 && a[1] == 0x00 && a[2] == 0xFF &&
                           a[3] == 0xFF && a[4] == 0xFF && a[5] == 0xFF && a[6] == 0xFF && a[7] == 0xFF;
        expect(three, "a cell of white (16 pixels), black (8) and orange (40): orange and white, the black line takes orange, the nearer");
        bool solid = true;
        for (int l = 3; l < 8; ++l) if (b[l] != 0xFF) solid = false;
        expect(colNext[40 + 6] == 0x81 && b[0] == 0 && b[1] == 0 && b[2] == 0 && solid,
               "white over orange: both kept, the orange solid (no stripes of lone dots)");
        std::memset(&M.ram[0x2000], 0, 0x2000);
        // White strokes (dots side by side) on black in byte 2: $3C lights dots 16-19 (C64 pixels 32-35, cell 4's
        // left half: $F0); $66 lights dots 15-16 and 19-20 (pixel 31 in cell 3, then 32, 35 and 36: $98).
        put(0, 2, 0x3C); put(1, 2, 0x66); put(2, 2, 0x3C);
        convertHires();
        expect(colNext[4] == 0x10 && bmNext[4 * 8] == 0xF0 && bmNext[4 * 8 + 1] == 0x98 && bmNext[4 * 8 + 2] == 0xF0 &&
               bmNext[3 * 8 + 1] == 0x01, "white strokes on black stay dot for dot (text keeps its full width)");
        // Sparse colour keeps its weight: one green dot in four lit (bytes $11 $22 $44 $08 ... on line 0) shows two
        // dots wide, as on a colour TV (Apple colour is 140 across): half the cell green, not a quarter.
        std::memset(&M.ram[0x2000], 0, 0x2000);
        for (int i = 0; i < 40; ++i) for (int bit = 0; bit < 7; ++bit) if ((i * 7 + bit) % 4 == 1) M.ram[a2_hires_row(&M, 0) + i] |= uint8_t(1 << bit);
        convertHires();
        int lit = 0;
        for (int cx = 4; cx < 34; ++cx) for (int b = 0; b < 8; ++b) lit += (bmNext[cx * 8] >> b) & 1;
        expect(lit == 30 * 4 && (colNext[10] >> 4) == 13, "sparse colour (one dot in four) lights two pixels in four, in green: as bright as a colour TV shows it");
        M.text = 1; M.hires = 0;
    }

    // ---- Hi-res on the C64, with the fake C64 transfer ----
    {
        for (auto &row : screen) { std::memset(row, ' ', 40); row[40] = 0; }
        VmHostC64Dma dhost{};
        dhost.base = VmNativeHost::makeWithExit(root, "", workspace.data(), uint32_t(workspace.size()),
                                                guest.data(), uint32_t(guest.size()));
        dhost.base.base.bytes = sizeof(VmHostC64Dma);
        dhost.base.base.services |= VM_SERVICE_C64_DMA;
        dhost.base.base.micros_now = fakeMicros;
        dhost.c64_write = fakeC64Write;
        dhost.c64_status = fakeC64Status;
        mod = vm_entry(&dhost.base.base);
        expect(mod != nullptr && dmaHost != nullptr, "with the C64 transfer the module takes it");
        run(1.0);
        type("\x02\n");
        run(3.0);
        type("HGR:HCOLOR=3:HPLOT 0,0 TO 279,159:HCOLOR=1:HPLOT 1,10 TO 277,10:HCOLOR=6:HPLOT 0,20 TO 278,20\n");
        run(2.0);
        expect(lastSplit == 20, "HGR (mixed): the client is told text starts at row 20");
        expect(dmaJobs > 0 && bitmapMismatches() == 0, "the C64 picture matches the Apple's page 1 pixel for pixel, in the Apple's colours");
        expect(c64[0x0400 + 0 * 40 + 2] == 0x10, "a white line's cell is white on black ($10)");
        expect(c64[0x0400 + 1 * 40 + 10] == 0xD0, "a green line's cell is light green ($D0)");
        expect(c64[0x0400 + 2 * 40 + 10] == 0x30, "a blue line's cell is cyan ($30)");
        type("VTAB 22:PRINT \"MIXED\"\n");
        run(1.0);
        bool textRows = std::string(screen[21]).find("MIXED") != std::string::npos;
        for (int r = 20; r < 24; ++r) for (int c = 0; c < 40; ++c) if (c64[0x0400 + r * 40 + c] != raw[r][c]) textRows = false;
        expect(textRows, "rows 20-23 stay text (VTAB 22: PRINT shows there)");
        const uint32_t jobsBefore = dmaJobs;
        run(1.0);
        expect(dmaJobs == jobsBefore, "a still picture sends nothing more");
        type("POKE -16302,0\n");                       // full-screen graphics
        run(1.0);
        expect(lastSplit == 24, "full-screen hi-res: text starts at row 24 (the status line)");
        bool colours = true;
        for (int r = 0; r < 24; ++r) for (int c = 0; c < 40; ++c) if (c64[0x0400 + r * 40 + c] != colNext[r * 40 + c]) colours = false;
        expect(colours, "every bitmap row holds its colours, none left over from text");
        type("TEXT\n");
        run(2.0);
        bool textBack = lastSplit == 0;
        for (int r = 0; r < 24; ++r) for (int c = 0; c < 40; ++c) if (c64[0x0400 + r * 40 + c] != raw[r][c]) textBack = false;
        expect(textBack && onScreen("]TEXT"), "TEXT: all text again, every row repainted");
        std::printf("hi-res: %u DMA jobs, %u bytes, %d mode packets\n", dmaJobs, dmaBytes, modePackets);

        // Oregon Trail's hi-res menu on the C64, from the SD files.
        if (std::filesystem::exists(sandbox / "OREGON.PO")) {
            std::filesystem::copy_file(oregon, sandbox / "OREGON.PO", std::filesystem::copy_options::overwrite_existing);
            dhost.base = VmNativeHost::makeWithExit(root, (sandbox / "OREGON.PO").string(), workspace.data(),
                                                    uint32_t(workspace.size()), guest.data(), uint32_t(guest.size()));
            dhost.base.base.bytes = sizeof(VmHostC64Dma);
            dhost.base.base.services |= VM_SERVICE_C64_DMA;
            dhost.base.base.micros_now = fakeMicros;
            mod = vm_entry(&dhost.base.base);
            run(30.0);
            int clash = 0;
            const int wrong = bitmapMismatches(&clash);
            std::printf("Oregon Trail's menu on the C64: %d cells with three or more Apple colours\n", clash);
            expect(lastSplit == 24 && wrong == 0, "Oregon Trail's menu reaches the C64 in the Apple's colours, pixel for pixel wherever a cell has two");
            int white = 0;
            for (int i = 0; i < 24 * 40; ++i) if (c64[0x0400 + i] == 0x10) ++white;
            expect(white > 50, "its title and menu cells are white");
            std::printf("Oregon Trail on the C64: %u DMA jobs, %u bytes in all\n", dmaJobs, dmaBytes);
            saveC64View(sandbox / "oregon-c64-view.ppm");
        }
    }

    dump();
    std::printf("%s\n", failures ? "MODULE TEST FAILED" : "module test passed");
    return failures ? 1 : 0;
}
