/* stick.c: the C64's joystick (port 2) on the Apple (J2). See stick.h.
 *
 * How the hunt was found (2026-11-25, on the Mac): in the play-through the hunt takes its keys with
 * LDA $C000 at $EC21 and STA $C010 at $EC26, running from language-card RAM; the name prompt reads at
 * $6205 and $67F1, the Monitor and BASIC elsewhere in ROM. The hunt read the keyboard 414 times in
 * 3 seconds, 23,500 to 61,300 cycles apart, so "recently" is 0.1 s: once the game leaves the hunt
 * (its last read is somewhere else, or none for 0.1 s) the stick types nothing.
 * One press per change: after a single O the hunter aims up and stays so (checked over 6 seconds).
 */
#include "stick.h"

int stick_hunting(const a2 *m) {
    return m->key_read_pc == STICK_HUNT_PC && m->key_read_lc && m->cpu.cycles - m->key_read_cycle < STICK_HUNT_RECENT;
}

/* -1, 0 or 1 on one axis; both ways at once (not possible on a stick) counts as centre. */
static int stick_axis(uint8_t bits, uint8_t neg, uint8_t pos) {
    const int n = (bits & neg) != 0, p = (bits & pos) != 0;
    return n == p ? 0 : (p ? 1 : -1);
}

int stick_apply(a2 *m, uint8_t *last, uint8_t bits, uint8_t keys[2]) {
    static const uint8_t aim[3][3] = {          /* [y + 1][x + 1] */
        { 'I', 'O', 'P' },
        { 'K', 0,   ';' },
        { ',', '.', '/' },
    };
    const int x = stick_axis(bits, STICK_LEFT, STICK_RIGHT), y = stick_axis(bits, STICK_UP, STICK_DOWN);
    const int lx = stick_axis(*last, STICK_LEFT, STICK_RIGHT), ly = stick_axis(*last, STICK_UP, STICK_DOWN);
    a2_set_paddle(m, 0, (uint8_t)(x < 0 ? 0 : x > 0 ? 255 : 127));
    a2_set_paddle(m, 1, (uint8_t)(y < 0 ? 0 : y > 0 ? 255 : 127));
    a2_set_button(m, 0, (bits & STICK_FIRE) != 0);
    int n = 0;
    if (stick_hunting(m)) {
        if ((x != lx || y != ly) && aim[y + 1][x + 1]) keys[n++] = aim[y + 1][x + 1];
        if ((bits & STICK_FIRE) && !(*last & STICK_FIRE)) keys[n++] = ' ';
    }
    *last = (uint8_t)(bits & STICK_BITS);
    return n;
}
