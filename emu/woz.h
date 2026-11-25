/* woz.h: decode a 3.5-inch (400K/800K) WOZ 2 disk image into 512-byte blocks.
 *
 * Portable C99, works on a buffer (no files), so the Mac front end and the TeensyROM
 * firmware can both use it. Same method as tools/woz-to-po.py, which
 * decoded 1,599 of 1,600 Oregon Trail sectors with good checksums: Apple 3.5-inch GCR,
 * address field D5 AA 96, data field D5 AA AD with 524 bytes (12 tag + 512 data) under
 * the three-byte running checksum. A sector that is missing or fails its checksum is
 * marked bad: a block-level drive cannot read it, so reads of that block report an I/O
 * error. (Oregon Trail's block 8 has a non-standard data mark, D5 AA 97.)
 */
#ifndef WOZ_H
#define WOZ_H
#include <stddef.h>
#include <stdint.h>

#define WOZ35_MAX_BLOCKS 1600

typedef struct woz_disk {
    int sides;
    int nblocks;
    int nbad;
    uint8_t bad[WOZ35_MAX_BLOCKS];
    uint8_t blocks[WOZ35_MAX_BLOCKS * 512];
} woz_disk;

/* Returns 0 and fills *out, or -1 with a reason in *why. */
int woz35_decode(const uint8_t *img, size_t len, woz_disk *out, const char **why);

#endif
