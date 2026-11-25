/* stick.h: the C64's joystick (port 2) on the Apple (plan step J2). Shared: the TeensyROM module
 * applies the bits the client sends, and a2term applies its --keys \sHH escape the same way.
 * Portable C99 that also compiles as C++, no malloc, no mutable statics.
 *
 * Bits as the client puts them in the input record's flag byte, bits 3-7.
 * Paddles: a digital stick gives each axis 0, 127 or 255 (left/up 0, centre 127, right/down 255):
 * paddle 0 is x, paddle 1 is y. Fire is button 0.
 * Oregon Trail's hunt: while the game's last keyboard read came from its hunt at $EC21 (language-card
 * RAM), and recently, the stick also types the hunt's aiming keys, one key when the direction changes:
 *      I O P        up-left  up    up-right
 *      K   ;        left           right
 *      , . /        down-left down down-right
 * and fire types Space, once per press. Centre types nothing. The game turns the hunter on one press
 * and holds the aim (on a ][ Plus a held key does not repeat), so the stick never repeats a key.
 */
#ifndef STICK_H
#define STICK_H
#include <stdint.h>
#include "a2.h"

#define STICK_UP    0x08
#define STICK_DOWN  0x10
#define STICK_LEFT  0x20
#define STICK_RIGHT 0x40
#define STICK_FIRE  0x80
#define STICK_BITS  0xF8

#define STICK_HUNT_PC     0xEC21u     /* Oregon Trail's hunt reads the keyboard here, from language-card RAM */
#define STICK_HUNT_RECENT 102300u     /* cycles (0.1 s): the hunt reads every 23,500-61,300 cycles (measured) */

#ifdef __cplusplus
extern "C" {
#endif
int stick_hunting(const a2 *m);       /* 1 while the game's last keyboard read was the hunt's, recently */
/* Apply port 2's bits: paddles 0 and 1, button 0. *last holds the bits applied before (updated here).
   keys gets the Apple keys to type for the hunt (none outside it); returns how many (0-2). */
int stick_apply(a2 *m, uint8_t *last, uint8_t bits, uint8_t keys[2]);
#ifdef __cplusplus
}
#endif

#endif
