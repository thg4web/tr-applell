/* diskimg.h: disk images as the Apple asks for them (drop-and-play step D1). Shared: the TeensyROM module
 * reads them straight off the SD card, a2term from a file in memory, the tests from either. Portable C99 that
 * also compiles as C++: no malloc, no libc, no 64-bit divide or variable 64-bit shift (the module links
 * -nostdlib). The image itself is never held: every read goes through the caller's read function, one
 * sector or block at a time, at an offset in the file.
 *
 * Formats, told apart by the file name's extension and the file's size (and a .2mg's own header):
 *   .dsk .do   143,360 bytes: a 5.25-inch disk, 35 tracks of 16 sectors, in DOS 3.3 order.
 *   .po        143,360 bytes: the same disk in ProDOS order; any other whole number of 512-byte blocks
 *              (800K: 819,200; up to 65,535 blocks): a block device in ProDOS order (Oregon Trail's .po).
 *   .2mg .2img a 64-byte header ("2IMG"), then the data at the header's offset: format 0 DOS order (143,360
 *              bytes), 1 ProDOS order (whole blocks); 2 (nibbles) is not supported; flag bit 31 = locked.
 * Anything else is refused, and so is a size that does not fit its extension.
 *
 * Orders: a DOS-order file holds each track's sectors as DOS 3.3 numbers them (logical 0-15); a ProDOS-order
 * file holds them as ProDOS does (block b = track b / 8, its two halves). The two differ by one table,
 * kDimgDosProdos (its own inverse): DOS sector s is ProDOS-order sector kDimgDosProdos[s]. The drive itself
 * (D2) works in physical sectors, the order on the track: DOS 3.3 sector s is physical kDimgDosPhysical[s].
 */
#ifndef DISKIMG_H
#define DISKIMG_H
#include <stdint.h>

#define DIMG_525_BYTES 143360u            /* 35 tracks x 16 sectors x 256 */
#define DIMG_525_TRACKS 35
#define DIMG_HEAD_BYTES 64                /* what dimg_open wants to see of the file's start (a .2mg header) */

enum dimg_kind { DIMG_NONE, DIMG_525, DIMG_BLOCKS };
enum dimg_order { DIMG_DOS, DIMG_PRODOS };

/* Read len bytes at offset in the file into buf: returns len, or anything else for an error. */
typedef int (*dimg_read_fn)(void *ctx, uint32_t offset, uint8_t *buf, uint32_t len);
typedef int (*dimg_write_fn)(void *ctx, uint32_t offset, const uint8_t *buf, uint32_t len);

typedef struct dimg {
    uint8_t kind, order, locked;          /* enum dimg_kind, enum dimg_order; locked: a .2mg's flag, writes refused */
    uint32_t offset;                      /* where the disk's data starts in the file (a .2mg header before it) */
    uint32_t blocks;                      /* 512-byte blocks (280 for a 5.25-inch disk) */
    const char *why;                      /* dimg_open's reason, when it refuses */
    dimg_read_fn read; dimg_write_fn write; void *ctx;
} dimg;

extern const uint8_t kDimgDosProdos[16];  /* DOS 3.3 sector -> ProDOS-order sector, and back */
extern const uint8_t kDimgDosPhysical[16];/* DOS 3.3 sector -> physical sector on the track */

#ifdef __cplusplus
extern "C" {
#endif
/* name: the file name (only its extension is looked at); file_bytes: the file's size; head: its first bytes
   (up to DIMG_HEAD_BYTES, fewer if the file is shorter). write may be null (read only). 0 = recognised. */
int dimg_open(dimg *d, const char *name, uint32_t file_bytes, const uint8_t *head, uint32_t head_len,
              dimg_read_fn read, dimg_write_fn write, void *ctx);
int dimg_read_block(const dimg *d, uint32_t block, uint8_t buf[512]);        /* 0 or -1 */
int dimg_write_block(const dimg *d, uint32_t block, const uint8_t buf[512]);
/* 5.25-inch disks only: a sector by track and DOS 3.3 sector number (as DOS 3.3 itself asks), or by track and
   physical sector (as the Disk II drive in D2 meets them on the track). */
int dimg_read_sector(const dimg *d, int track, int sector, uint8_t buf[256]);
int dimg_write_sector(const dimg *d, int track, int sector, const uint8_t buf[256]);
int dimg_read_physical(const dimg *d, int track, int physical, uint8_t buf[256]);
const char *dimg_describe(const dimg *d);  /* "5.25-inch, DOS order" and so on */
#ifdef __cplusplus
}
#endif

#endif
