/* sid-voice.h: what SID voice 1 would play from the tone tracker's frames (sound plan step S2). Mac only.
 *
 * A simple model of one SID voice, NOT a SID emulator: a 24-bit oscillator, the pulse wave with its
 * 12-bit width, the 23-bit noise generator, and the envelope at attack 0, decay 0, sustain 15,
 * release 0 (about 2 ms up, about 6.6 ms down, with the SID's stepped release). No filter, no 6581
 * or 8580 analogue colour, no ADSR delay bug. It is for comparing with the Apple's own WAV by ear;
 * the real listen is on a C64.
 * The WAV matches a2term --wav: 22050 Hz, mono, 16-bit, the same number of samples, the same 20 Hz
 * high-pass and the same level, so the two line up sample for sample in an editor.
 */
#ifndef SID_VOICE_H
#define SID_VOICE_H
#include <stddef.h>
#include <stdint.h>
#include "../emu/tone.h"

/* f[0..n): every frame from the tracker, in order (frame k's result plays from f[k].end).
   clock: the tracker's Apple-over-C64 fraction (TONE_CLOCK_*). Returns 1 written, 0 not. */
int sid_write_wav(const tone_frame *f, size_t n, uint32_t clock_num, uint32_t clock_den,
                  uint64_t end_cycle, const char *path);

#endif
