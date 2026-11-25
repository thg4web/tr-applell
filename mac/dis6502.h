#ifndef DIS6502_H
#define DIS6502_H
#include <stddef.h>
#include "../emu/a2.h"
int dis6502(const a2 *m, uint16_t pc, char *out, size_t n);   /* returns the instruction length */
#endif
