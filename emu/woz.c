/* woz.c: 3.5-inch WOZ 2 decoder. See woz.h. */
#include <string.h>
#include "woz.h"

static const uint8_t GCR62[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };
static int8_t DEC[256];

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int spt(int track) { return 12 - track / 16; }

#define NIB_CAP 65536
static uint8_t nib[NIB_CAP];

/* Disk bytes from the bitstream, read twice round for sectors that cross the index. */
static size_t to_nibbles(const uint8_t *bits, uint32_t bit_count) {
    size_t n = 0;
    uint8_t reg = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < bit_count; i++) {
            reg = (uint8_t)((reg << 1) | ((bits[i >> 3] >> (7 - (i & 7))) & 1));
            if (reg & 0x80) { if (n < NIB_CAP) nib[n++] = reg; reg = 0; }
        }
    return n;
}

/* 703 six-bit values (699 data + 4 checksum) -> 524 bytes; returns 1 if the checksum is good. */
static int decode_data(const int8_t *vals, uint8_t *out) {
    uint8_t b1[175], b2[175], b3[175];
    int p = 0;
    for (int i = 0; i < 175; i++) {
        int w4 = vals[p], w1 = vals[p + 1], w2 = vals[p + 2], w3 = 0;
        if (i < 174) { w3 = vals[p + 3]; p += 4; } else p += 3;
        b1[i] = (uint8_t)((w1 & 0x3F) | ((w4 << 2) & 0xC0));
        b2[i] = (uint8_t)((w2 & 0x3F) | ((w4 << 4) & 0xC0));
        b3[i] = (uint8_t)((w3 & 0x3F) | ((w4 << 6) & 0xC0));
    }
    unsigned c1 = 0, c2 = 0, c3 = 0;
    int n = 0, k = 0;
    for (;;) {
        c1 = (c1 & 0xFF) << 1;
        if (c1 & 0x100) c1++;
        uint8_t v = (uint8_t)(b1[k] ^ c1);
        c3 += v;
        if (c1 & 0x100) { c3++; c1 &= 0xFF; }
        out[n++] = v;
        v = (uint8_t)(b2[k] ^ c3);
        c2 += v;
        if (c3 > 0xFF) { c2++; c3 &= 0xFF; }
        out[n++] = v;
        if (n == 524) break;
        v = (uint8_t)(b3[k] ^ c2);
        c1 += v;
        if (c2 > 0xFF) { c1++; c2 &= 0xFF; }
        out[n++] = v;
        k++;
    }
    unsigned c4 = ((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2);
    return (unsigned)vals[p] == (c4 & 0x3F) && (unsigned)vals[p + 1] == (c3 & 0x3F)
        && (unsigned)vals[p + 2] == (c2 & 0x3F) && (unsigned)vals[p + 3] == (c1 & 0x3F);
}

/* Decode one track side into out (blocks for sectors 0..spt-1); sets got[sector]. */
static void decode_track(size_t n, int want_track, int want_side, uint8_t *out, uint8_t *got) {
    for (size_t i = 0; i + 8 <= n; i++) {
        if (nib[i] != 0xD5 || nib[i + 1] != 0xAA || nib[i + 2] != 0x96) continue;
        int f[5], okf = 1;
        for (int k = 0; k < 5; k++) { f[k] = DEC[nib[i + 3 + k]]; if (f[k] < 0) okf = 0; }
        if (!okf || f[4] != (f[0] ^ f[1] ^ f[2] ^ f[3])) continue;
        int track = f[0] | ((f[2] & 0x1F) << 6), side = (f[2] >> 5) & 1, sector = f[1];
        if (track != want_track || side != want_side || sector >= spt(track) || got[sector]) continue;
        size_t j, end = i + 8 + 100;
        for (j = i + 8; j + 3 <= end && j + 3 <= n; j++)
            if (nib[j] == 0xD5 && nib[j + 1] == 0xAA && nib[j + 2] == 0xAD) break;
        if (j + 3 > end || j + 4 + 703 > n) continue;
        if (DEC[nib[j + 3]] != sector) continue;
        int8_t vals[703];
        int okv = 1;
        for (int k = 0; k < 703; k++) { vals[k] = DEC[nib[j + 4 + k]]; if (vals[k] < 0) okv = 0; }
        if (!okv) continue;
        uint8_t payload[524];
        if (decode_data(vals, payload)) {
            memcpy(out + sector * 512, payload + 12, 512);
            got[sector] = 1;
        }
    }
}

int woz35_decode(const uint8_t *img, size_t len, woz_disk *d, const char **why) {
    memset(DEC, -1, sizeof DEC);
    for (int i = 0; i < 64; i++) DEC[GCR62[i]] = (int8_t)i;
    if (len < 12 || memcmp(img, "WOZ2\xFF\x0A\x0D\x0A", 8) != 0) { *why = "not a WOZ 2 image"; return -1; }
    const uint8_t *info = NULL, *tmap = NULL, *trks = NULL;
    for (size_t pos = 12; pos + 8 <= len;) {
        uint32_t size = le32(img + pos + 4);
        if (pos + 8 + size > len) break;
        if (!memcmp(img + pos, "INFO", 4)) info = img + pos + 8;
        if (!memcmp(img + pos, "TMAP", 4)) tmap = img + pos + 8;
        if (!memcmp(img + pos, "TRKS", 4)) trks = img + pos + 8;
        pos += 8 + size;
    }
    if (!info || !tmap || !trks) { *why = "image lacks INFO, TMAP or TRKS"; return -1; }
    if (info[1] != 2) { *why = "not a 3.5-inch disk image"; return -1; }
    d->sides = info[37] == 2 ? 2 : 1;
    d->nblocks = 0;
    for (int t = 0; t < 80; t++) d->nblocks += spt(t) * d->sides;
    memset(d->blocks, 0, sizeof d->blocks);
    memset(d->bad, 1, sizeof d->bad);
    int block = 0;
    for (int t = 0; t < 80; t++)
        for (int s = 0; s < d->sides; s++) {
            uint8_t got[12] = { 0 };
            uint8_t idx = tmap[t * 2 + s];
            if (idx != 0xFF) {
                const uint8_t *e = trks + idx * 8;
                size_t start = (size_t)le16(e) * 512, bytes = (size_t)le16(e + 2) * 512;
                uint32_t bits = le32(e + 4);
                if (start + bytes <= len && (bits + 7) / 8 <= bytes)
                    decode_track(to_nibbles(img + start, bits), t, s, d->blocks + (size_t)block * 512, got);
            }
            for (int k = 0; k < spt(t); k++) d->bad[block + k] = !got[k];
            block += spt(t);
        }
    d->nbad = 0;
    for (int b = 0; b < d->nblocks; b++) d->nbad += d->bad[b];
    return 0;
}
