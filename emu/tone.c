/* tone.c: the tone tracker (sound plan step S2). See tone.h.
 *
 * How a frame is read (the rules are S1's, mac/speaker.c, so the two can check each other):
 *  - Intervals: the time from each click to the one before it, when that one is in the same sound
 *    event (clicks more than 0.2 Apple second apart start a new event). So the interval that crosses
 *    into a frame counts, and notes under about 120 Hz still track (below).
 *  - Full periods: each pair of neighbouring intervals (two flips), so an uneven duty cycle still
 *    reads as one pitch. A frame with 4+ clicks and 3+ intervals is STEADY when nine in ten of its
 *    full periods sit within 4% of their median (the upper median, as S1). It is SPLIT when it is
 *    two steady parts with one or two mixed periods between them (a note change or a short rest):
 *    the later part is what plays on. Otherwise IRREGULAR (noise, fast sweeps).
 *  - SLOW: a frame with fewer than 4 clicks, when the event's last 4 intervals make 3 full periods
 *    within 4% of their median, the period is over half a frame (only then can a steady tone leave
 *    a frame with under 4 clicks: under about 120 Hz), and the tone is still going (below). It takes
 *    5 clicks (2.5 periods) to see one: a 50 Hz tone starts about 50 ms late.
 *  - SPARSE: 1-3 clicks and no slow tone. Lone ticks (a keyboard click, the edge of a sound) are lost:
 *    the gate stays off. The SID cannot click.
 * What voice 1 plays during the next frame:
 *  - Pulse (STEADY, SPLIT, SLOW): Fn = 2^24 x clock_num / (clock_den x P), P the full period in Apple
 *    cycles, rounded. On NTSC that is 2^24 x 910 / (912 x P): the doc's 8,388,608 / half period,
 *    corrected by 910/912 (0.22%, about 4 cents) because the Apple's average clock is that much
 *    slower than the C64's. The pitch then matches a real Apple, whatever pace the Teensy keeps.
 *  - Above the SID's top (Fn 65535, about 4 kHz on NTSC) the tone drops an octave, or two (up to
 *    about 16 kHz), so a melody keeps its shape; over that the gate goes off (too_high). A choice
 *    made by ear, not measured: Oregon Trail never goes above 540 Hz.
 *  - Pulse width = the smaller half over the whole period x 4096 (a square wave and its inverse
 *    sound alike), from the part that plays on: 2048 is an even square.
 *  - A split frame's later part plays on only when it holds 3 or more full periods (TONE_SPLIT_MIN).
 *    A shorter one is the first period or two of a new note, and those mix in the code between the
 *    notes (Oregon Trail: a 677-cycle flip between 1,451 and 1,382 made a 480 Hz blip between a
 *    335 Hz note and a 360 Hz one). Then the earlier part plays one more frame (carried); the next
 *    frame, all new note, brings it in. With 2 periods Oregon Trail had 1 blip, with 1 period 9.
 *  - Noise (IRREGULAR): Fn from the click rate. The SID's noise generator steps when bit 19 of the
 *    oscillator's 24-bit accumulator rises, every 2^20 / Fn C64 cycles, so Fn = clicks per C64 cycle
 *    x 2^20 makes it step about as often as the Apple clicked (up to 65535). Confidence: medium that
 *    the formula is right (it follows from how reSID documents the noise clock), low that it sounds
 *    like the Apple's noise; Oregon Trail makes none, so the ear test waits for a game that does.
 *    A fast sweep is irregular too, so it plays as noise: S3 can follow its last period instead if
 *    that sounds wrong.
 *  - The gate is on only if the sound is still going at the frame's end: the last click no further
 *    back than one full period and an eighth (pulse) or two mean intervals (noise). A note that stops
 *    half way through a frame is not held for the whole next one. The kind still says what the frame
 *    held, so frame counts compare with S1's report.
 *  - Gate off keeps the last Fn and pulse width (the SID's release plays out at that pitch). The
 *    frame is "changed" only when the gate changes, or the gate is on and the wave, Fn or pulse
 *    width changes.
 *  - Holding through jitter: while the gate stays on with the same wave, an Fn within 0.5% (8.6
 *    cents) of what plays and a pulse width within 64 (1.6% duty) are held; noise holds an Fn within
 *    a quarter. Oregon Trail's tone loops vary by a cycle or two from period to period, so the median
 *    flips between neighbours (Fn 6667/6662, width 1991/1992): without the hold the play-through
 *    made 1,407 packets, 1,280 of them jitter; with it, 115 (at most 5 in any second). A tune's
 *    notes are at least a semitone (5.9%) apart, so no note change is held.
 * The work for a frame is bounded: at most 256 intervals; the split test is O(n^2) on those
 * (insertion into a sorted list), a few thousand steps for an Oregon Trail frame.
 */
#include "tone.h"

/* n / d for 64-bit values (d > 0, below 2^63), by shift and subtract: the same quotient as "/".
   The TeensyROM module links without a runtime library (-nostdlib), so a plain 64-bit "/" calls
   __aeabi_uldivmod, which it does not have (APPLE2G's first link failed on it, 2026-11-25). Only
   shifts by a constant, so no other helper is needed. A handful a frame: cheap on the Teensy. */
static uint64_t tone_div64(uint64_t n, uint64_t d) {
    uint64_t q = 0, r = 0;
    for (int i = 0; i < 64; i++) {
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}

static uint64_t tone_frame_end_of(const tone_tracker *t, uint64_t k) {      /* ceil((k+1) * num / den) */
    const uint64_t x = (k + 1) * t->frame_num;
    return tone_div64(x + t->frame_den - 1, t->frame_den);
}

void tone_init(tone_tracker *t, uint32_t frame_num, uint32_t frame_den, uint32_t clock_num, uint32_t clock_den,
               tone_emit_fn emit, void *ctx) {
    unsigned char *b = (unsigned char *)t;
    for (unsigned long i = 0; i < sizeof *t; i++) b[i] = 0;
    t->frame_num = frame_num ? frame_num : TONE_FRAME_NUM;
    t->frame_den = frame_den ? frame_den : 1;
    t->clock_num = clock_num ? clock_num : TONE_CLOCK_NTSC_NUM;
    t->clock_den = clock_den ? clock_den : TONE_CLOCK_NTSC_DEN;
    t->emit = emit; t->ctx = ctx;
    t->frame = 0;
    t->frame_end = tone_frame_end_of(t, 0);
    t->pw = 2048;
}

/* s[0..n) sorted: insert v. */
static void tone_insert(uint32_t *s, uint32_t n, uint32_t v) {
    uint32_t i = n;
    while (i > 0 && s[i - 1] > v) { s[i] = s[i - 1]; i--; }
    s[i] = v;
}

/* s[0..n) sorted: the first index with s[i] >= v (by the 25x scale: 25 s[i] >= lim). */
static uint32_t tone_first_at_least(const uint32_t *s, uint32_t n, uint64_t lim25) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t mid = (lo + hi) / 2; if ((uint64_t)s[mid] * 25 >= lim25) hi = mid; else lo = mid + 1; }
    return lo;
}

/* Steady: nine in ten within 4% of the (upper) median s[n/2]. */
static int tone_steady(const uint32_t *s, uint32_t n, uint32_t *med) {
    if (n == 0) return 0;
    const uint32_t m = s[n / 2];
    const uint32_t a = tone_first_at_least(s, n, (uint64_t)m * 24);
    const uint32_t b = tone_first_at_least(s, n, (uint64_t)m * 26 + 1);
    *med = m;
    return (uint64_t)(b - a) * 10 >= (uint64_t)n * 9;
}

/* Fn for a full period of P Apple cycles; octaves dropped; 0 = too high even two octaves down. */
static uint16_t tone_pulse_fn(const tone_tracker *t, uint32_t period, uint8_t *octaves) {
    for (uint8_t o = 0; o <= 2; o++) {
        const uint64_t num = ((uint64_t)1 << 25) * t->clock_num;            /* x2, for rounding */
        const uint64_t den = (uint64_t)t->clock_den * ((uint64_t)period << o);
        const uint64_t fn = (tone_div64(num, den) + 1) / 2;
        if (fn <= 65535) { *octaves = o; return (uint16_t)(fn ? fn : 1); }
    }
    *octaves = 0;
    return 0;
}

/* Pulse width from intervals d[0..n): pairs (d[k], d[k+1]) for k = 0, 2, 4 ... */
static uint16_t tone_pulse_width(const uint32_t *d, uint32_t n) {
    uint64_t a = 0, b = 0;
    for (uint32_t k = 0; k + 1 < n; k += 2) { a += d[k]; b += d[k + 1]; }
    if (a + b == 0) return 2048;
    const uint64_t lo = a < b ? a : b;
    return (uint16_t)((tone_div64(lo * 4096 * 2, a + b) + 1) / 2);
}

static void tone_close(tone_tracker *t) {
    tone_frame f;
    unsigned char *b = (unsigned char *)&f;
    for (unsigned long i = 0; i < sizeof f; i++) b[i] = 0;
    f.index = (uint32_t)t->frame;
    f.end = t->frame_end;
    f.clicks = (uint16_t)(t->clicks > 65535 ? 65535 : t->clicks);
    const uint64_t since = t->have_last ? t->frame_end - t->last : (uint64_t)-1;
    uint8_t gate = 0, wave = t->wave;
    uint16_t fn = t->fn, pw = t->pw;
    uint32_t period = 0, play = 0;             /* the period measured (S1's), the one that plays */
    uint32_t play0 = 0, play1 = t->nd;         /* the playing part: intervals d[play0..play1) */
    const uint32_t nd = t->nd;

    if (t->clicks >= 4 && nd >= 3) {
        const uint32_t np = nd - 1;
        for (uint32_t k = 0; k < np; k++) t->p[k] = t->d[k] + t->d[k + 1];
        /* prefix p[0..s) steady, for every s; then suffix p[i..np) steady, for every i */
        t->pre_ok[0] = 0;
        for (uint32_t s = 1; s <= np; s++) {
            tone_insert(t->sorted, s - 1, t->p[s - 1]);
            t->pre_ok[s] = (uint8_t)tone_steady(t->sorted, s, &t->pre_med[s]);
        }
        period = play = t->pre_med[np];
        if (t->pre_ok[np]) f.kind = TONE_STEADY;
        else {
            for (uint32_t i = np; i-- > 0; ) {
                tone_insert(t->sorted, np - 1 - i, t->p[i]);
                t->suf_ok[i] = (uint8_t)tone_steady(t->sorted, np - i, &t->suf_med[i]);
            }
            f.kind = TONE_IRREGULAR;
            for (uint32_t s = 1; s + 1 < np && f.kind == TONE_IRREGULAR; s++)      /* S1's order */
                for (uint32_t skip = 1; skip <= 2 && s + skip < np; skip++)
                    if (t->pre_ok[s] && t->suf_ok[s + skip]) {
                        f.kind = TONE_SPLIT; period = play = t->suf_med[s + skip]; play0 = s + skip; play1 = nd;
                        if (np - play0 < TONE_SPLIT_MIN) {      /* the new note is too short to trust: carry the old */
                            f.carried = 1; play = t->pre_med[s]; play0 = 0; play1 = s + 1;
                        }
                        break;
                    }
        }
        if (f.kind != TONE_IRREGULAR) {
            f.period = period;
            pw = tone_pulse_width(t->d + play0, play1 - play0);
            wave = TONE_PULSE;
            gate = since <= (uint64_t)period + period / 8;
        } else {
            uint64_t sum = 0;
            for (uint32_t k = 0; k < nd; k++) sum += t->d[k];
            const uint64_t num = ((uint64_t)nd << 20) * t->clock_num, den = sum * t->clock_den;
            const uint64_t nfn = den ? (tone_div64(num * 2, den) + 1) / 2 : 65535;
            fn = (uint16_t)(nfn > 65535 ? 65535 : nfn ? nfn : 1);
            wave = TONE_NOISE;
            gate = since <= tone_div64(2 * sum, nd);
        }
    } else if (t->clicks || t->have_last) {
        /* a slow tone: the event's last 4 intervals, over half a frame each period, still going */
        if (t->nhist == 4) {
            uint32_t s3[3];
            for (uint32_t k = 0; k < 3; k++) { uint32_t v = t->hist[k] + t->hist[k + 1]; tone_insert(s3, k, v); }
            uint32_t med = 0;
            if (tone_steady(s3, 3, &med) && (uint64_t)med * 2 * t->frame_den > t->frame_num
                && since <= (uint64_t)med + med / 8) {
                f.kind = TONE_SLOW; f.period = period = play = med;
                pw = tone_pulse_width(t->hist, 4);
                wave = TONE_PULSE; gate = 1;
            }
        }
        if (f.kind != TONE_SLOW) f.kind = t->clicks ? TONE_SPARSE : TONE_SILENT;
    } else f.kind = TONE_SILENT;

    if (gate && wave == TONE_PULSE) {
        fn = tone_pulse_fn(t, play, &f.octaves);
        if (!fn) { f.too_high = 1; gate = 0; fn = t->fn; }
    }
    if (gate && t->gate && wave == t->wave) {  /* hold through jitter: under a packet's worth of difference */
        const uint32_t dfn = fn > t->fn ? fn - t->fn : t->fn - fn, dpw = pw > t->pw ? pw - t->pw : t->pw - pw;
        if ((uint64_t)dfn * (wave == TONE_NOISE ? TONE_HOLD_NOISE_DIV : TONE_HOLD_FN_DIV) <= t->fn) fn = t->fn;
        if (wave == TONE_NOISE || dpw <= TONE_HOLD_PW) pw = t->pw;
    }
    f.gate = gate; f.wave = wave; f.fn = fn; f.pw = pw;
    f.changed = (uint8_t)(gate != t->gate || (gate && (wave != t->wave || fn != t->fn || pw != t->pw)));
    if (gate) { t->wave = wave; t->fn = fn; t->pw = pw; }
    t->gate = gate;
    if (t->emit) t->emit(t->ctx, &f);
    t->frame++;
    t->frame_end = tone_frame_end_of(t, t->frame);
    t->nd = 0;
    t->clicks = 0;
}

void tone_advance(tone_tracker *t, uint64_t now) {
    while (now >= t->frame_end) tone_close(t);
}

void tone_click(tone_tracker *t, uint64_t cycle) {
    tone_advance(t, cycle);
    t->clicks++;
    if (t->have_last && cycle - t->last <= TONE_EVENT_GAP) {
        const uint32_t iv = (uint32_t)(cycle - t->last);
        if (t->nd < TONE_MAX_INTERVALS) t->d[t->nd++] = iv;
        if (t->nhist < 4) t->hist[t->nhist++] = iv;
        else { t->hist[0] = t->hist[1]; t->hist[1] = t->hist[2]; t->hist[2] = t->hist[3]; t->hist[3] = iv; }
    } else t->nhist = 0;                       /* a new sound event */
    t->last = cycle;
    t->have_last = 1;
}
