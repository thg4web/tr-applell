/* tone.h: the tone tracker (sound plan step S2). Shared: the Mac (a2term) uses it now, the
 * TeensyROM module (S3) will include it unchanged. Portable C99 that also compiles as C++:
 * no malloc, no statics, fixed buffers inside the tracker.
 *
 * The Apple's speaker is one bit flipped by each access to $C030. The C64's SID cannot play
 * clicks, but its voice 1 can play the tone they make. The tracker takes the clicks one at a
 * time (Apple cycle counts, in order) and, at the end of each frame, says what voice 1 should
 * play during the NEXT frame: gate on or off, pulse or noise, the frequency value Fn and the
 * pulse width. One frame of latency, as on the C64: a frame's result is known only at its end.
 * S3 sends a packet only when that changes (tone_frame.changed). Details in tone.c.
 */
#ifndef TONE_H
#define TONE_H
#include <stdint.h>

/* Frame length in Apple cycles, as a fraction num/den.
 * S3's frame (the default): 17,050 cycles, the module's 60 Hz screen scan (16,667 us) at the pace
 * it runs the Apple (1,023 cycles a millisecond: teensy/apple2-module.cpp pump()), so a frame's
 * result comes due once per scan. 17,050 is 17,050.341 rounded: 0.002% off.
 * S1's frame: 1/60 Apple second at the real clock (1,020,484.4 Hz), 17,008.07 cycles; the
 * cross-check against S1's report uses it. */
#define TONE_FRAME_NUM     17050u
#define TONE_FRAME_DEN     1u
#define TONE_FRAME_S1_NUM  2551211u
#define TONE_FRAME_S1_DEN  150u

/* The Apple's clock over the C64's, as a fraction. NTSC: both come from the same
 * 14.31818 MHz crystal, the C64 dividing by 14 and the Apple's 65-cycle line taking 912 crystal
 * cycles (one long cycle), so the ratio is exactly 910/912. PAL (later): Apple 1,020,484.4 Hz
 * over C64 985,248.6 Hz, both x10. */
#define TONE_CLOCK_NTSC_NUM 910u
#define TONE_CLOCK_NTSC_DEN 912u
#define TONE_CLOCK_PAL_NUM  10204844u
#define TONE_CLOCK_PAL_DEN  9852486u

#define TONE_MAX_INTERVALS 256u       /* intervals kept per frame: 256 is a 7.7 kHz tone; clicks past it still count */
#define TONE_EVENT_GAP     204096u    /* clicks more than 0.2 Apple second apart start a new sound event (as S1) */
#define TONE_SPLIT_MIN     3u         /* a split frame's later part needs this many full periods to play on */
#define TONE_HOLD_FN_DIV   200u       /* while the gate stays on, an Fn within 1/200 (0.5%, 8.6 cents) is held */
#define TONE_HOLD_PW       64u        /* ... and a pulse width within 64 (1.6% duty) */
#define TONE_HOLD_NOISE_DIV 4u        /* noise: an Fn within 1/4 is held (its rate wanders frame to frame) */

enum tone_kind {
    TONE_SILENT,       /* no clicks */
    TONE_STEADY,       /* 4+ clicks, nine in ten full periods within 4% of their median (S1's "steady tone") */
    TONE_SPLIT,        /* two steady parts, one or two mixed periods between: the later part plays on */
    TONE_SLOW,         /* 0-3 clicks, but a tone under about 120 Hz followed across frames */
    TONE_IRREGULAR,    /* 4+ clicks, neither: played as noise */
    TONE_SPARSE        /* 1-3 clicks and no slow tone: ticks or the edge of a sound; lost (gate off) */
};
enum tone_wave { TONE_PULSE, TONE_NOISE };

typedef struct tone_frame {
    uint32_t index;        /* frame number: frame k is Apple cycles [ceil(k*num/den), ceil((k+1)*num/den)) */
    uint64_t end;          /* the frame's end (the next frame's first cycle): this result plays from here */
    uint8_t  kind;         /* enum tone_kind: what the frame held (independent of the gate) */
    uint8_t  gate;         /* 1 = voice 1 sounds during the next frame */
    uint8_t  wave;         /* enum tone_wave */
    uint8_t  changed;      /* gate, wave, Fn or pulse width differ from the frame before: S3 sends a packet */
    uint8_t  octaves;      /* octaves dropped to fit the SID (0-2) */
    uint8_t  too_high;     /* a tone over two octaves above the SID's top: gate off */
    uint8_t  carried;      /* a split frame whose new note was under TONE_SPLIT_MIN periods: the old one plays on */
    uint16_t fn;           /* SID frequency value (registers $D400/$D401) */
    uint16_t pw;           /* SID pulse width, 12-bit ($D402/$D403) */
    uint16_t clicks;       /* clicks in the frame (stops at 65535) */
    uint32_t period;       /* pulse kinds: the full period S1's rules measure (a split frame's later part), Apple cycles */
} tone_frame;

typedef void (*tone_emit_fn)(void *ctx, const tone_frame *f);

typedef struct tone_tracker {
    uint32_t frame_num, frame_den, clock_num, clock_den;
    tone_emit_fn emit; void *ctx;
    uint64_t frame;                        /* the open frame */
    uint64_t frame_end;                    /* its end: the first cycle of the next frame */
    uint64_t last;                         /* the latest click */
    int      have_last;
    uint32_t hist[4]; uint32_t nhist;      /* the event's latest intervals, oldest first (slow tones) */
    uint32_t d[TONE_MAX_INTERVALS]; uint32_t nd;   /* intervals ending in the open frame */
    uint32_t clicks;                       /* clicks in the open frame */
    uint8_t  gate, wave; uint16_t fn, pw;  /* what voice 1 plays now */
    /* scratch for one frame's sums */
    uint32_t p[TONE_MAX_INTERVALS], sorted[TONE_MAX_INTERVALS], suf_med[TONE_MAX_INTERVALS];
    uint32_t pre_med[TONE_MAX_INTERVALS + 1];
    uint8_t  pre_ok[TONE_MAX_INTERVALS + 1], suf_ok[TONE_MAX_INTERVALS];
} tone_tracker;

#ifdef __cplusplus
extern "C" {
#endif
/* frame: num/den Apple cycles (TONE_FRAME_*); clock: Apple over C64 (TONE_CLOCK_*). emit gets
   every frame, in order, as it closes (silent ones too). */
void tone_init(tone_tracker *t, uint32_t frame_num, uint32_t frame_den, uint32_t clock_num, uint32_t clock_den,
               tone_emit_fn emit, void *ctx);
void tone_click(tone_tracker *t, uint64_t cycle);    /* each click, cycles never going back */
void tone_advance(tone_tracker *t, uint64_t now);    /* close every frame that ends at or before now */
#ifdef __cplusplus
}
#endif

#endif
