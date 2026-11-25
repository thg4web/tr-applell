/* diskimg.c: disk images as the Apple asks for them (D1). See diskimg.h.
 *
 * The tables. DOS 3.3 lays a track's 16 sectors out with a skew: DOS sector s is physical sector
 * kDimgDosPhysical[s] (Beneath Apple DOS, the RWTS interleave). ProDOS numbers them its own way: ProDOS-order
 * sector i is physical (i * 2) mod 15, with 15 staying 15 (Beneath Apple ProDOS). Put together, DOS sector s
 * sits at ProDOS-order sector kDimgDosProdos[s]: 0 and 15 stay, 1-14 run backwards. Applied twice it gives s
 * back, so the one table converts both ways.
 * Blocks on a 5.25-inch disk: ProDOS block b is track b / 8, ProDOS-order sectors 2(b mod 8) and 2(b mod 8)+1.
 * Offsets are 32-bit (the largest block device, 65,535 blocks, is under 32 MB) and only shifts divide.
 */
#include "diskimg.h"

const uint8_t kDimgDosProdos[16]   = { 0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };
const uint8_t kDimgDosPhysical[16] = { 0, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 15 };

static uint32_t dimg_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The name's extension, lower case, in ext (up to 7 characters); 0 if there is none. */
static int dimg_extension(const char *name, char ext[8]) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) { if (*p == '.') dot = p + 1; if (*p == '/') dot = 0; }
    if (!dot || !*dot) return 0;
    int n = 0;
    for (; dot[n] && n < 7; n++) { const char c = dot[n]; ext[n] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    if (dot[n]) return 0;                           /* longer than any we know */
    ext[n] = 0;
    return 1;
}

static int dimg_same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int dimg_open(dimg *d, const char *name, uint32_t file_bytes, const uint8_t *head, uint32_t head_len,
              dimg_read_fn read, dimg_write_fn write, void *ctx) {
    d->kind = DIMG_NONE; d->order = DIMG_PRODOS; d->locked = 0; d->offset = 0; d->blocks = 0;
    d->read = read; d->write = write; d->ctx = ctx; d->why = 0;
    char ext[8];
    if (!dimg_extension(name, ext)) { d->why = "no extension"; return -1; }
    uint32_t bytes = file_bytes;
    if (dimg_same(ext, "2mg") || dimg_same(ext, "2img")) {
        if (head_len < 64 || head[0] != '2' || head[1] != 'I' || head[2] != 'M' || head[3] != 'G') { d->why = "not a 2IMG header"; return -1; }
        const uint32_t format = dimg_le32(head + 12), flags = dimg_le32(head + 16);
        const uint32_t offset = dimg_le32(head + 24), length = dimg_le32(head + 28);
        if (offset < 52 || offset > file_bytes || length > file_bytes - offset) { d->why = "2IMG data runs past the file"; return -1; }
        if (format == 2) { d->why = "2IMG nibble images are not supported"; return -1; }
        if (format > 1) { d->why = "2IMG format unknown"; return -1; }
        d->offset = offset;
        d->locked = (uint8_t)(flags >> 31);
        bytes = length;
        if (format == 0) {                          /* DOS order: only a 5.25-inch disk */
            if (bytes != DIMG_525_BYTES) { d->why = "2IMG in DOS order is not 143,360 bytes"; return -1; }
            d->kind = DIMG_525; d->order = DIMG_DOS;
        } else d->order = DIMG_PRODOS;
    } else if (dimg_same(ext, "dsk") || dimg_same(ext, "do")) {
        if (bytes != DIMG_525_BYTES) { d->why = "a .dsk or .do is 143,360 bytes"; return -1; }
        d->kind = DIMG_525; d->order = DIMG_DOS;
    } else if (dimg_same(ext, "woz") || dimg_same(ext, "nib") || dimg_same(ext, "d13")) {
        d->why = "a disk format this layer does not read as blocks (.woz .nib .d13)"; return -1;
    } else {
        /* .po, and any other extension (a package's own, for side-by-side test copies): ProDOS-order blocks,
           as the module read every image before this layer; the size must still be whole blocks. */
        d->order = DIMG_PRODOS;
    }
    if (d->kind == DIMG_NONE) {                     /* ProDOS order: 140K is a 5.25-inch disk, else blocks */
        if (bytes == 0 || (bytes & 511) || (bytes >> 9) > 65535) { d->why = "not a whole number of 512-byte blocks"; return -1; }
        d->kind = bytes == DIMG_525_BYTES ? DIMG_525 : DIMG_BLOCKS;
    }
    d->blocks = bytes >> 9;
    return 0;
}

/* Where DOS sector s of track t is in the file. */
static uint32_t dimg_sector_at(const dimg *d, int t, int s) {
    const int i = d->order == DIMG_DOS ? s : kDimgDosProdos[s];
    return d->offset + ((uint32_t)t << 12) + ((uint32_t)i << 8);
}

/* Where half h (0 or 1) of block b is in the file. */
static uint32_t dimg_half_at(const dimg *d, uint32_t b, int h) {
    if (d->kind != DIMG_525 || d->order == DIMG_PRODOS) return d->offset + (b << 9) + ((uint32_t)h << 8);
    const int i = (int)((b & 7) << 1) + h;          /* ProDOS-order sector on track b / 8 */
    return dimg_sector_at(d, (int)(b >> 3), kDimgDosProdos[i]);
}

int dimg_read_block(const dimg *d, uint32_t block, uint8_t buf[512]) {
    if (d->kind == DIMG_NONE || block >= d->blocks) return -1;
    const uint32_t a = dimg_half_at(d, block, 0), b = dimg_half_at(d, block, 1);
    if (b == a + 256) return d->read(d->ctx, a, buf, 512) == 512 ? 0 : -1;
    return d->read(d->ctx, a, buf, 256) == 256 && d->read(d->ctx, b, buf + 256, 256) == 256 ? 0 : -1;
}

int dimg_write_block(const dimg *d, uint32_t block, const uint8_t buf[512]) {
    if (d->kind == DIMG_NONE || block >= d->blocks || d->locked || !d->write) return -1;
    const uint32_t a = dimg_half_at(d, block, 0), b = dimg_half_at(d, block, 1);
    if (b == a + 256) return d->write(d->ctx, a, buf, 512) == 512 ? 0 : -1;
    return d->write(d->ctx, a, buf, 256) == 256 && d->write(d->ctx, b, buf + 256, 256) == 256 ? 0 : -1;
}

int dimg_read_sector(const dimg *d, int track, int sector, uint8_t buf[256]) {
    if (d->kind != DIMG_525 || track < 0 || track >= DIMG_525_TRACKS || sector < 0 || sector > 15) return -1;
    return d->read(d->ctx, dimg_sector_at(d, track, sector), buf, 256) == 256 ? 0 : -1;
}

int dimg_write_sector(const dimg *d, int track, int sector, const uint8_t buf[256]) {
    if (d->kind != DIMG_525 || track < 0 || track >= DIMG_525_TRACKS || sector < 0 || sector > 15 || d->locked || !d->write) return -1;
    return d->write(d->ctx, dimg_sector_at(d, track, sector), buf, 256) == 256 ? 0 : -1;
}

int dimg_read_physical(const dimg *d, int track, int physical, uint8_t buf[256]) {
    for (int s = 0; s < 16; s++)
        if (kDimgDosPhysical[s] == physical) return dimg_read_sector(d, track, s, buf);
    return -1;
}

const char *dimg_describe(const dimg *d) {
    if (d->kind == DIMG_525) return d->order == DIMG_DOS ? "5.25-inch, DOS order" : "5.25-inch, ProDOS order";
    if (d->kind == DIMG_BLOCKS) return "blocks, ProDOS order";
    return "not a disk";
}
