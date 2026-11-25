/* test-disk.c: the disk image layer (emu/diskimg.c, drop-and-play step D1).
 * Usage: test-disk [DOS33.dsk ...]
 *   Synthetic disks first (made here, in memory): a 140K disk whose every sector names itself, in DOS order;
 *   the same disk put in ProDOS order here from the published sector skews (not from the layer's table); and
 *   both wrapped as .2mg. The layer must give the same sectors, blocks and physical sectors from each, refuse
 *   what doesn't fit, and keep a .2mg's lock.
 *   Then each DOS 3.3 disk named (read straight from its file, a sector at a time, never copied): its VTOC and
 *   catalog through the layer, the catalog printed as tools/dos33-files.py prints it (tests/test-disk.sh
 *   compares the two), and the same disk reordered to ProDOS order in memory read back identically.
 * Exit 0 only if every check passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../emu/diskimg.h"

static int failures;
static void expect(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }

/* Memory files. */
typedef struct mem { uint8_t *p; uint32_t n; } mem;
static int mread(void *ctx, uint32_t off, uint8_t *buf, uint32_t len) {
    mem *m = (mem *)ctx; if ((uint64_t)off + len > m->n) return -1; memcpy(buf, m->p + off, len); return (int)len;
}
static int mwrite(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len) {
    mem *m = (mem *)ctx; if ((uint64_t)off + len > m->n) return -1; memcpy(m->p + off, buf, len); return (int)len;
}
/* A real file, read at offsets (the module reads the SD card the same way). */
static int fread_at(void *ctx, uint32_t off, uint8_t *buf, uint32_t len) {
    FILE *f = (FILE *)ctx; if (fseek(f, (long)off, SEEK_SET)) return -1; return (int)fread(buf, 1, len, f);
}
static int open_mem(dimg *d, const char *name, mem *m) {
    return dimg_open(d, name, m->n, m->p, m->n < DIMG_HEAD_BYTES ? m->n : DIMG_HEAD_BYTES, mread, mwrite, m);
}

/* The published skews, here on their own: DOS 3.3 sector s lies at physical sector DOS_SKEW[s] (Beneath Apple
   DOS); ProDOS-order sector i lies at physical (2i) mod 15, sector 15 at 15 (Beneath Apple ProDOS). */
static const int DOS_SKEW[16] = { 0, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 15 };
static int prodos_physical(int i) { return i == 15 ? 15 : (2 * i) % 15; }
/* A DOS-order image to ProDOS order, through the physical sectors. */
static void to_prodos(const uint8_t *dos, uint8_t *po) {
    for (int t = 0; t < 35; t++)
        for (int i = 0; i < 16; i++)
            for (int s = 0; s < 16; s++)
                if (DOS_SKEW[s] == prodos_physical(i)) memcpy(po + (t * 16 + i) * 256, dos + (t * 16 + s) * 256, 256);
}
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static mem make_2mg(const uint8_t *data, uint32_t n, uint32_t format, uint32_t flags) {
    mem m = { calloc(64 + n, 1), 64 + n };
    memcpy(m.p, "2IMGTEST", 8);
    m.p[8] = 64; m.p[10] = 1;                        /* header length, version */
    put_le32(m.p + 12, format); put_le32(m.p + 16, flags);
    put_le32(m.p + 20, format == 1 ? n / 512 : 0); put_le32(m.p + 24, 64); put_le32(m.p + 28, n);
    memcpy(m.p + 64, data, n);
    return m;
}

/* Every sector, block and physical sector of a and b the same; returns 1 if so. */
static int same_disk(const dimg *a, const dimg *b) {
    uint8_t x[512], y[512];
    for (int t = 0; t < 35; t++)
        for (int s = 0; s < 16; s++) {
            if (dimg_read_sector(a, t, s, x) || dimg_read_sector(b, t, s, y) || memcmp(x, y, 256)) return 0;
            if (dimg_read_physical(a, t, s, x) || dimg_read_physical(b, t, s, y) || memcmp(x, y, 256)) return 0;
        }
    for (uint32_t blk = 0; blk < 280; blk++)
        if (dimg_read_block(a, blk, x) || dimg_read_block(b, blk, y) || memcmp(x, y, 512)) return 0;
    return 1;
}

/* A DOS 3.3 disk's catalog, as tools/dos33-files.py lists it. Returns -1 on a bad VTOC. */
static int catalog(const dimg *d, int print) {
    uint8_t v[256], c[256], tsl[256], sec[256];
    if (dimg_read_sector(d, 17, 0, v) || v[3] != 3 || v[0x34] != 35 || v[0x35] != 16) return -1;
    int n = 0, t = v[1], s = v[2], guard = 0;
    char lines[128][80];
    while (t && guard++ < 64) {
        if (dimg_read_sector(d, t, s, c)) return -1;
        for (int e = 0; e < 7; e++) {
            const uint8_t *ent = c + 0x0B + e * 35;
            if (ent[0] == 0 || ent[0] == 0xFF) continue;
            char name[31];
            for (int k = 0; k < 30; k++) name[k] = (char)(ent[3 + k] & 0x7F);
            name[30] = 0;
            for (int k = 29; k >= 0 && name[k] == ' '; k--) name[k] = 0;
            const int ty = ent[2] & 0x7F;
            const char *tn = ty == 0 ? "T" : ty == 1 ? "I" : ty == 2 ? "A" : ty == 4 ? "B" : ty == 8 ? "S" : ty == 0x10 ? "R" :
                             ty == 0x20 ? "A2" : ty == 0x40 ? "B2" : "?";
            /* the file's bytes: the track/sector list's data sectors, then DOS's header for A and B files */
            long raw = 0; int tt = ent[0], ts = ent[1], g2 = 0, done = 0;
            uint8_t first[4] = { 0, 0, 0, 0 }; int have_first = 0;
            while (tt && !done && g2++ < 200) {
                if (dimg_read_sector(d, tt, ts, tsl)) return -1;
                for (int k = 0x0C; k < 256; k += 2) {
                    if (tsl[k] == 0 && tsl[k + 1] == 0) { done = 1; break; }
                    if (!have_first) { if (dimg_read_sector(d, tsl[k], tsl[k + 1], sec)) return -1; memcpy(first, sec, 4); have_first = 1; }
                    raw += 256;
                }
                tt = tsl[1]; ts = tsl[2];
            }
            long size = raw;
            if (!strcmp(tn, "A")) { long want = first[0] | first[1] << 8; size = raw - 2 < want ? raw - 2 : want; }
            if (!strcmp(tn, "B")) { long want = first[2] | first[3] << 8; size = raw - 4 < want ? raw - 4 : want; }
            if (size < 0) size = 0;
            snprintf(lines[n], sizeof lines[n], "%c%-2s %03d %6ld bytes  %s", (ent[2] & 0x80) ? '*' : ' ', tn,
                     ent[33] | ent[34] << 8, size, name);
            if (++n == 128) break;
        }
        t = c[1]; s = c[2];
    }
    if (print) { printf("volume %d, %d file(s)\n", v[6], n); for (int i = 0; i < n; i++) printf("%s\n", lines[i]); }
    return n;
}

int main(int argc, char **argv) {
    /* ---- synthetic: every sector of the DOS-order disk holds its track, its DOS sector, then a pattern ---- */
    static uint8_t dos[DIMG_525_BYTES], po[DIMG_525_BYTES];
    for (int t = 0; t < 35; t++)
        for (int s = 0; s < 16; s++) {
            uint8_t *p = dos + (t * 16 + s) * 256;
            p[0] = (uint8_t)t; p[1] = (uint8_t)s;
            for (int k = 2; k < 256; k++) p[k] = (uint8_t)(t * 7 + s * 13 + k);
        }
    to_prodos(dos, po);
    mem mdsk = { dos, DIMG_525_BYTES }, mpo = { po, DIMG_525_BYTES };
    dimg ddsk, ddo, dpo;
    expect(open_mem(&ddsk, "GAME.DSK", &mdsk) == 0 && ddsk.kind == DIMG_525 && ddsk.order == DIMG_DOS && ddsk.blocks == 280,
           ".dsk of 143,360 bytes: a 5.25-inch disk in DOS order, 280 blocks");
    expect(open_mem(&ddo, "game.do", &mdsk) == 0 && ddo.kind == DIMG_525 && ddo.order == DIMG_DOS, ".do: the same");
    expect(open_mem(&dpo, "game.po", &mpo) == 0 && dpo.kind == DIMG_525 && dpo.order == DIMG_PRODOS, ".po of 143,360 bytes: 5.25-inch in ProDOS order");
    uint8_t b[512];
    int named = 1;
    for (int t = 0; t < 35; t++) for (int s = 0; s < 16; s++) if (dimg_read_sector(&dpo, t, s, b) || b[0] != t || b[1] != s) named = 0;
    expect(named, "the ProDOS-order disk gives every DOS sector by its DOS number (each names itself)");
    expect(same_disk(&ddsk, &dpo), "the .dsk and the .po give the same sectors, physical sectors and blocks");
    int blocks_raw = 1;
    for (uint32_t blk = 0; blk < 280; blk++) if (dimg_read_block(&dpo, blk, b) || memcmp(b, po + blk * 512, 512)) blocks_raw = 0;
    expect(blocks_raw, "a ProDOS-order disk's block b is simply bytes b x 512 of the file");
    int phys = 1;
    for (int t = 0; t < 35; t += 17) for (int p = 0; p < 16; p++) {
        dimg_read_physical(&ddsk, t, p, b);
        int s = 0; while (DOS_SKEW[s] != p) s++;
        if (b[0] != t || b[1] != s) phys = 0;
    }
    expect(phys, "physical sectors follow DOS 3.3's skew (for the Disk II drive in D2)");
    int involution = 1;
    for (int s = 0; s < 16; s++) if (kDimgDosProdos[kDimgDosProdos[s]] != s) involution = 0;
    expect(involution, "one table converts DOS order to ProDOS order and back (it is its own inverse)");

    /* writes land where reads find them, in either order */
    static uint8_t dos2[DIMG_525_BYTES], po2[DIMG_525_BYTES];
    memcpy(dos2, dos, sizeof dos2); memcpy(po2, po, sizeof po2);
    mem mdos2 = { dos2, DIMG_525_BYTES }, mpo2 = { po2, DIMG_525_BYTES };
    dimg w1, w2;
    open_mem(&w1, "w.dsk", &mdos2); open_mem(&w2, "w.po", &mpo2);
    uint8_t blk[512];
    for (int k = 0; k < 512; k++) blk[k] = (uint8_t)(k * 3 + 1);
    const int wrote = dimg_write_block(&w1, 123, blk) == 0 && dimg_write_block(&w2, 123, blk) == 0;
    uint8_t s0[256];
    dimg_write_sector(&w1, 20, 5, s0); dimg_write_sector(&w2, 20, 5, s0);
    expect(wrote && same_disk(&w1, &w2), "the same block and sector written to both: they stay the same disk");

    /* ---- .2mg ---- */
    mem g0 = make_2mg(dos, DIMG_525_BYTES, 0, 0), g1 = make_2mg(po, DIMG_525_BYTES, 1, 0x80000000u);
    dimg d0, d1;
    expect(open_mem(&d0, "game.2mg", &g0) == 0 && d0.kind == DIMG_525 && d0.order == DIMG_DOS && d0.offset == 64 && !d0.locked &&
           same_disk(&d0, &ddsk), ".2mg, format 0: DOS order at the header's offset, the same disk");
    expect(open_mem(&d1, "GAME.2IMG", &g1) == 0 && d1.kind == DIMG_525 && d1.order == DIMG_PRODOS && d1.locked &&
           same_disk(&d1, &ddsk) && dimg_write_block(&d1, 0, blk) != 0, ".2img, format 1, locked: ProDOS order, the same disk, writes refused");
    static uint8_t big[819200];
    for (uint32_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i >> 9);
    mem g2 = make_2mg(big, sizeof big, 1, 0);
    dimg d2;
    expect(open_mem(&d2, "disk.2mg", &g2) == 0 && d2.kind == DIMG_BLOCKS && d2.blocks == 1600 && dimg_read_block(&d2, 1599, b) == 0 && b[0] == (uint8_t)1599,
           ".2mg of 800K in ProDOS order: 1,600 blocks");
    mem bad = make_2mg(dos, DIMG_525_BYTES, 2, 0);
    dimg dx;
    expect(open_mem(&dx, "nib.2mg", &bad) != 0 && strstr(dx.why, "nibble"), ".2mg holding nibbles: refused (not supported)");
    put_le32(bad.p + 12, 0); put_le32(bad.p + 24, 200000);
    expect(open_mem(&dx, "far.2mg", &bad) != 0, ".2mg whose data starts past the file: refused");
    put_le32(bad.p + 24, 64); put_le32(bad.p + 28, 1000);
    expect(open_mem(&dx, "short.2mg", &bad) != 0, ".2mg in DOS order that is not 143,360 bytes: refused");
    bad.p[0] = 'X';
    expect(open_mem(&dx, "magic.2mg", &bad) != 0, ".2mg without \"2IMG\": refused");

    /* ---- sizes and names ---- */
    mem m800 = { big, sizeof big }, m140short = { dos, DIMG_525_BYTES - 256 }, modd = { big, 1000 };
    dimg dd;
    expect(open_mem(&dd, "/OREGON.PO", &m800) == 0 && dd.kind == DIMG_BLOCKS && dd.order == DIMG_PRODOS && dd.offset == 0 && dd.blocks == 1600,
           "OREGON.PO (800K): blocks in ProDOS order from byte 0, as before");
    expect(open_mem(&dd, "x.dsk", &m140short) != 0 && open_mem(&dd, "x.dsk", &m800) != 0, "a .dsk that is not 143,360 bytes: refused");
    expect(open_mem(&dd, "x.po", &modd) != 0, "a .po that is not whole blocks: refused");
    expect(open_mem(&dd, "/OREGON.A2M", &m800) == 0 && dd.kind == DIMG_BLOCKS && dd.order == DIMG_PRODOS && dd.offset == 0 && dd.blocks == 1600 &&
           dimg_read_block(&dd, 1599, b) == 0 && b[0] == (uint8_t)1599,
           "a package's own extension (OREGON.A2M, 800K): ProDOS-order blocks, as before the layer");
    expect(open_mem(&dd, "game.xyz", &mpo) == 0 && dd.kind == DIMG_525 && dd.order == DIMG_PRODOS && same_disk(&dd, &dpo),
           "any other extension at 143,360 bytes: read as a .po (5.25-inch, ProDOS order)");
    expect(open_mem(&dd, "x.a2m", &modd) != 0, "an unknown extension that is not whole blocks: refused");
    expect(open_mem(&dd, "x.woz", &mdsk) != 0 && open_mem(&dd, "x.nib", &mdsk) != 0 && open_mem(&dd, "x.d13", &mdsk) != 0,
           ".woz, .nib and .d13: refused (this layer does not read them as blocks)");
    expect(open_mem(&dd, "noext", &mdsk) != 0 && open_mem(&dd, "dir.po/file", &mdsk) != 0,
           "no extension, or a dot only in a folder name: refused");
    expect(dimg_read_block(&ddsk, 280, b) != 0 && dimg_read_sector(&ddsk, 35, 0, b) != 0 && dimg_read_sector(&dd, 0, 0, b) != 0,
           "past the end, or a sector asked of a block device: refused");

    /* ---- DOS 3.3 disks given on the command line (your own), read from the file a sector at a time ---- */
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { printf("skip %s (absent)\n", argv[a]); continue; }
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t head[DIMG_HEAD_BYTES];
        const size_t hn = fread(head, 1, sizeof head, f);
        dimg d;
        const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        char what[200];
        snprintf(what, sizeof what, "%s: a 5.25-inch DOS-order disk, read from the file", base);
        expect(dimg_open(&d, argv[a], (uint32_t)n, head, (uint32_t)hn, fread_at, NULL, f) == 0 && d.kind == DIMG_525 && d.order == DIMG_DOS, what);
        uint8_t v[256];
        snprintf(what, sizeof what, "%s: the VTOC at track 17 sector 0 (release 3, 35 tracks, 16 sectors)", base);
        expect(dimg_read_sector(&d, 17, 0, v) == 0 && v[3] == 3 && v[0x34] == 35 && v[0x35] == 16, what);
        printf("--- catalog %s\n", base);
        const int files = catalog(&d, 1);
        printf("--- end\n");
        snprintf(what, sizeof what, "%s: the catalog through the layer (%d files)", base, files);
        expect(files > 0, what);
        /* the same disk in ProDOS order, made in memory only */
        static uint8_t whole[DIMG_525_BYTES], reord[DIMG_525_BYTES];
        fseek(f, 0, SEEK_SET);
        if (fread(whole, 1, sizeof whole, f) == sizeof whole) {
            to_prodos(whole, reord);
            mem mr = { reord, DIMG_525_BYTES };
            dimg dr;
            open_mem(&dr, "reordered.po", &mr);
            snprintf(what, sizeof what, "%s put in ProDOS order (in memory): every sector and block the same, the same catalog", base);
            expect(same_disk(&d, &dr) && catalog(&dr, 0) == files, what);
        }
        fclose(f);
    }
    printf("%s\n", failures ? "DISK TEST FAILED" : "disk test passed");
    return failures ? 1 : 0;
}
