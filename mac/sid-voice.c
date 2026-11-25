/* sid-voice.c: what SID voice 1 would play from the tone tracker's frames (S2). See sid-voice.h.
 *
 * Time: sample k is wall time [k, k+1) / 22050 s, the same span the Apple WAV gives it
 * (Apple cycles k x 46.28...). The voice runs one C64 cycle at a time; the C64 clock is the
 * Apple's times clock_den / clock_num (NTSC: 1,022,727 Hz). Frame k's result switches in at the
 * C64 cycle where Apple cycle f[k].end falls. Each sample is the mean of its C64 cycles (the
 * same box the Apple WAV uses), then the same 20 Hz high-pass and the same 0.4 level.
 * The SID parts (from how reSID describes the chip):
 *  - oscillator: a 24-bit accumulator plus Fn every cycle;
 *  - pulse: high while the accumulator's top 12 bits are at or above the pulse width;
 *  - noise: a 23-bit shift register (taps 22 and 17, seeded $7FFFF8), stepped when accumulator
 *    bit 19 rises; the output is bits 22 20 16 13 11 7 4 2;
 *  - envelope: an 8-bit level; at rate 0 one step every 9 cycles; attack climbs straight to 255;
 *    release steps down with the SID's divider (1 above 93, then 2, 4, 8, 16, and 30 below 7).
 * Silence with the envelope at 0 is skipped quickly; nothing else is approximated.
 */
#include <math.h>
#include <stdio.h>
#include "sid-voice.h"
#include "speaker.h"

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static unsigned exp_period(unsigned env) {
    return env > 0x5D ? 1 : env > 0x36 ? 2 : env > 0x1A ? 4 : env > 0x0E ? 8 : env > 0x06 ? 16 : 30;
}

static unsigned noise_out(uint32_t r) {
    return ((r >> 22) & 1) << 7 | ((r >> 20) & 1) << 6 | ((r >> 16) & 1) << 5 | ((r >> 13) & 1) << 4 |
           ((r >> 11) & 1) << 3 | ((r >> 7) & 1) << 2 | ((r >> 4) & 1) << 1 | ((r >> 2) & 1);
}

int sid_write_wav(const tone_frame *f, size_t n, uint32_t clock_num, uint32_t clock_den,
                  uint64_t end_cycle, const char *path) {
    const uint64_t nsamp = spk_wav_samples(end_cycle);
    FILE *out = spk_wav_open(path, nsamp);
    if (!out) return 0;
    const double c64_per_sample = SPK_APPLE_HZ * clock_den / clock_num / SPK_WAV_RATE;   /* about 46.4 */
    const double rc = 1.0 / (2.0 * M_PI * 20.0), dt = 1.0 / SPK_WAV_RATE, a = rc / (rc + dt);
    double x_prev = 0.0, y = 0.0;
    uint32_t acc = 0, lfsr = 0x7FFFF8;
    unsigned env = 0, rate = 0, expc = 0, gate = 0, wave = TONE_PULSE, fn = 0, pw = 2048;
    size_t j = 0;                                  /* the next frame result to switch in */
    uint64_t next = n ? (f[0].end * clock_den + clock_num - 1) / clock_num : UINT64_MAX;
    uint8_t buf[8192];
    size_t nb = 0;
    uint64_t c = 0;
    for (uint64_t k = 0; k < nsamp; k++) {
        const uint64_t c1 = (uint64_t)((double)(k + 1) * c64_per_sample);
        const uint64_t cycles = c1 - c;
        double sum = 0.0;
        if (!gate && env == 0 && next >= c1) {     /* silent through this sample */
            acc = (uint32_t)((acc + (uint64_t)fn * cycles) & 0xFFFFFF);
            c = c1;
        } else for (; c < c1; c++) {
            while (c >= next) {                    /* frame j's result: what the client writes now */
                gate = f[j].gate; wave = f[j].wave; fn = f[j].fn; pw = f[j].pw;
                j++;
                next = j < n ? (f[j].end * clock_den + clock_num - 1) / clock_num : UINT64_MAX;
            }
            const uint32_t before = acc;
            acc = (acc + fn) & 0xFFFFFF;
            if (!(before & 0x080000) && (acc & 0x080000))
                lfsr = ((lfsr << 1) & 0x7FFFFF) | (((lfsr >> 22) ^ (lfsr >> 17)) & 1);
            if (++rate >= 9) {                     /* attack, decay and release 0: every 9 cycles */
                rate = 0;
                if (gate) { if (env < 255) env++; expc = 0; }
                else if (env > 0 && ++expc >= exp_period(env)) { expc = 0; env--; }
            }
            const double w = wave == TONE_NOISE ? (double)noise_out(lfsr) / 127.5 - 1.0
                                                : ((acc >> 12) >= pw ? 1.0 : -1.0);
            sum += w * (double)env / 255.0;
        }
        const double x = cycles ? sum / (double)cycles : 0.0;
        y = a * (y + x - x_prev);
        x_prev = x;
        long v = lrint(y * 0.4 * 32767.0);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        put16(buf + nb, (unsigned)(v & 0xFFFF)); nb += 2;
        if (nb == sizeof buf) { fwrite(buf, 1, nb, out); nb = 0; }
    }
    if (nb) fwrite(buf, 1, nb, out);
    return fclose(out) == 0;
}
