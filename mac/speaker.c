/* speaker.c: the Apple's speaker on the Mac (sound plan step S1). See speaker.h.
 *
 * The speaker is one bit: every access to $C030 flips the cone. The WAV is that square wave,
 * averaged over each sample (so a click between samples still counts) and passed through a
 * 20 Hz high-pass (the speaker is AC-coupled: a cone held to one side goes quiet).
 *
 * The report looks at the clicks 1/60 second at a time (one C64 frame, the rate a sound
 * packet could change the SID). A frame with 4 or more clicks is a steady tone when nine in ten
 * of its full periods (two flips, so an uneven duty cycle still counts) sit within 4% of their
 * median. A frame that is two steady parts, with one or two mixed periods between them, is split:
 * a note change or a short rest inside the frame (the period that spans it mixes both sides).
 * Anything else is irregular (noise, fast sweeps). A frame with 1-3 clicks is sparse: ticks, or
 * the edge of a sound. Clicks more than 0.2 s apart start a new sound event.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "speaker.h"

void spk_record(void *ctx, uint64_t cycle) {
    spk_log *L = (spk_log *)ctx;
    if (L->n == L->cap) {
        size_t cap = L->cap ? L->cap * 2 : 65536;
        uint64_t *t = realloc(L->t, cap * sizeof *t);
        if (!t) return;                            /* out of memory: the log stops growing */
        L->t = t; L->cap = cap;
    }
    L->t[L->n++] = cycle;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

uint64_t spk_wav_samples(uint64_t end) { return (uint64_t)((double)end / (SPK_APPLE_HZ / SPK_WAV_RATE)); }

FILE *spk_wav_open(const char *path, uint64_t nsamp) {
    if (nsamp > 0x3FFFFFFFu) return NULL;              /* over about 13 hours: too big for a WAV */
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); put32le(h + 4, (uint32_t)(36 + nsamp * 2)); memcpy(h + 8, "WAVEfmt ", 8);
    put32le(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1);               /* PCM, mono */
    put32le(h + 24, SPK_WAV_RATE); put32le(h + 28, SPK_WAV_RATE * 2);       /* rate, bytes a second */
    put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32le(h + 40, (uint32_t)(nsamp * 2));
    fwrite(h, 1, 44, f);
    return f;
}

int spk_write_wav(const spk_log *L, uint64_t end, const char *path) {
    const double cps = SPK_APPLE_HZ / SPK_WAV_RATE;    /* Apple cycles per sample, about 46.3 */
    const uint64_t nsamp = spk_wav_samples(end);
    FILE *f = spk_wav_open(path, nsamp);
    if (!f) return 0;
    const double rc = 1.0 / (2.0 * M_PI * 20.0), dt = 1.0 / SPK_WAV_RATE, a = rc / (rc + dt);
    size_t i = 0;
    int level = 0;                                      /* the cone: 0 or 1, flipped by each click */
    double x_prev = -1.0, y = 0.0;
    uint8_t buf[8192];
    size_t nb = 0;
    for (uint64_t k = 0; k < nsamp; k++) {
        const double s0 = (double)k * cps, s1 = s0 + cps;
        double t = s0, hi = 0.0;
        while (i < L->n && (double)L->t[i] < s1) {
            double c = (double)L->t[i];
            if (c < t) c = t;
            if (level) hi += c - t;
            t = c; level ^= 1; i++;
        }
        if (level) hi += s1 - t;
        const double x = 2.0 * hi / cps - 1.0;          /* -1 .. 1 over this sample */
        y = a * (y + x - x_prev);
        x_prev = x;
        long v = lrint(y * 0.4 * 32767.0);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        put16(buf + nb, (unsigned)(v & 0xFFFF)); nb += 2;
        if (nb == sizeof buf) { fwrite(buf, 1, nb, f); nb = 0; }
    }
    if (nb) fwrite(buf, 1, nb, f);
    return fclose(f) == 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* p[0..n): steady when nine in ten sit within 4% of their median; *med gets the median. */
static int steady(const uint32_t *p, size_t n, double *med) {
    static uint32_t s[4096];
    if (n == 0) return 0;
    memcpy(s, p, n * sizeof *p);
    qsort(s, n, sizeof *s, cmp_u32);
    const double m = s[n / 2];
    size_t near = 0;
    for (size_t k = 0; k < n; k++) if (fabs((double)p[k] - m) <= 0.04 * m) near++;
    *med = m;
    return near * 10 >= n * 9;
}

enum { K_TONE, K_SPLIT, K_IRREG, K_SPARSE };
typedef struct tally { size_t clicks; long frames; } tally;

/* One frame's clicks: t[j0..j1), with t[first] the event's first click. Returns K_* and the
   full period in cycles for a tone. */
static int classify(const uint64_t *t, size_t first, size_t j0, size_t j1, double *period) {
    static uint32_t d[4096], p[4096];
    size_t nd = 0;
    for (size_t j = j0; j < j1 && nd < 4096; j++)
        if (j > first) d[nd++] = (uint32_t)(t[j] - t[j - 1]);
    if (j1 - j0 < 4 || nd < 3) return K_SPARSE;
    size_t np = 0;
    for (size_t k = 0; k + 1 < nd; k++) p[np++] = d[k] + d[k + 1];
    double med, m1;
    if (steady(p, np, &med)) { *period = med; return K_TONE; }
    for (size_t s = 1; s + 1 < np; s++)            /* two steady parts; the later one is what plays on */
        for (size_t skip = 1; skip <= 2 && s + skip < np; skip++)
            if (steady(p, s, &m1) && steady(p + s + skip, np - s - skip, &med)) { *period = med; return K_SPLIT; }
    return K_IRREG;
}

void spk_report(const spk_log *L, FILE *out, int list_events) {
    const double frame = SPK_APPLE_HZ / 60.0, gap = 0.2 * SPK_APPLE_HZ;
    tally kind[4] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    long events = 0, notes_all = 0;
    double fmin_all = 0, fmax_all = 0, sound_s = 0;
    if (list_events && L->n) fprintf(out, "sound events (start, length, clicks; frames tone/split/irregular/sparse; notes, range):\n");
    for (size_t a = 0; a < L->n; ) {
        size_t b = a + 1;
        while (b < L->n && (double)(L->t[b] - L->t[b - 1]) <= gap) b++;
        events++;
        long fr[4] = { 0, 0, 0, 0 }, notes = 0;
        double fmin = 0, fmax = 0, note_f = 0;
        uint64_t last_tone_frame = UINT64_MAX;
        for (size_t j0 = a; j0 < b; ) {
            const uint64_t fi = (uint64_t)((double)L->t[j0] / frame);
            size_t j1 = j0 + 1;
            while (j1 < b && (uint64_t)((double)L->t[j1] / frame) == fi) j1++;
            double period = 0;
            const int k = classify(L->t, a, j0, j1, &period);
            fr[k]++; kind[k].frames++; kind[k].clicks += j1 - j0;
            if (k == K_TONE || k == K_SPLIT) {
                const double f = SPK_APPLE_HZ / period;
                if (!fmin || f < fmin) fmin = f;
                if (f > fmax) fmax = f;
                if (last_tone_frame == UINT64_MAX || fi != last_tone_frame + 1 || fabs(f - note_f) > 0.03 * note_f) { notes++; note_f = f; }
                last_tone_frame = fi;
            }
            j0 = j1;
        }
        notes_all += notes;
        if (fmin && (!fmin_all || fmin < fmin_all)) fmin_all = fmin;
        if (fmax > fmax_all) fmax_all = fmax;
        const double start = (double)L->t[a] / SPK_APPLE_HZ, len = (double)(L->t[b - 1] - L->t[a]) / SPK_APPLE_HZ;
        sound_s += len;
        if (list_events) {
            fprintf(out, "  %8.2f s %6.2f s %7zu clicks  %4ld/%ld/%ld/%ld", start, len, b - a, fr[K_TONE], fr[K_SPLIT], fr[K_IRREG], fr[K_SPARSE]);
            if (notes) fprintf(out, "  %ld note%s %.1f-%.1f Hz", notes, notes == 1 ? "" : "s", fmin, fmax);
            fprintf(out, "\n");
        }
        a = b;
    }
    const double all = L->n ? (double)L->n : 1.0;
    fprintf(out, "sound: %zu clicks in %ld events, %.1f Apple seconds of sound\n", L->n, events, sound_s);
    fprintf(out, "  steady tones: %zu clicks (%.1f%%), %ld frames, %ld notes", kind[K_TONE].clicks,
            100.0 * (double)kind[K_TONE].clicks / all, kind[K_TONE].frames, notes_all);
    if (notes_all) fprintf(out, ", %.1f-%.1f Hz", fmin_all, fmax_all);
    fprintf(out, "\n  split (a note change or a rest inside the frame): %zu clicks (%.1f%%), %ld frames", kind[K_SPLIT].clicks,
            100.0 * (double)kind[K_SPLIT].clicks / all, kind[K_SPLIT].frames);
    fprintf(out, "\n  irregular: %zu clicks (%.1f%%), %ld frames\n", kind[K_IRREG].clicks,
            100.0 * (double)kind[K_IRREG].clicks / all, kind[K_IRREG].frames);
    fprintf(out, "  sparse: %zu clicks (%.1f%%), %ld frames\n", kind[K_SPARSE].clicks,
            100.0 * (double)kind[K_SPARSE].clicks / all, kind[K_SPARSE].frames);
}
