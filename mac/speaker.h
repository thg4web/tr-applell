/* speaker.h: the Apple's speaker on the Mac (sound plan step S1). Mac only.
 *
 * The emulator calls spk_record for every click ($C030), with the CPU cycle count. From that log:
 *  - spk_write_wav: what the speaker would play, as a 16-bit mono WAV, to listen to;
 *  - spk_report: what kind of sound it is, 1/60 second at a time: steady tones (a SID pulse voice
 *    can play them), irregular clicks (noise, fast sweeps: it cannot), or a few lone ticks.
 */
#ifndef SPEAKER_H
#define SPEAKER_H
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* The real Apple ][ Plus clock: 14.31818 MHz x 65 / 912 (each 65-cycle line has one long cycle). */
#define SPK_APPLE_HZ 1020484.4
#define SPK_WAV_RATE 22050

typedef struct spk_log { uint64_t *t; size_t n, cap; } spk_log;

void spk_record(void *ctx, uint64_t cycle);           /* ctx is the spk_log; the a2 speaker_hook */
int  spk_write_wav(const spk_log *log, uint64_t end_cycle, const char *path);   /* 1 written, 0 not */
/* For a second WAV that lines up sample for sample (mac/sid-voice.c): */
uint64_t spk_wav_samples(uint64_t end_cycle);              /* samples in a WAV of Apple cycles [0, end) */
FILE *spk_wav_open(const char *path, uint64_t nsamp);      /* writes the 44-byte header; NULL on failure */
void spk_report(const spk_log *log, FILE *out, int list_events);

#endif
