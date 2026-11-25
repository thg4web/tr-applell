/* badlist.h: the unreadable-block list that sits beside a block image (TR ][).
 *
 * A copy-protected disk has blocks the real drive cannot read (Oregon Trail: block 8). The block image
 * (.po) holds zeros there, and a text file beside it lists those blocks, so the slot-5 drive answers them
 * with an I/O error as the real drive does. Copy protection is emulated, never patched.
 *
 * The list (written by tools/woz-to-po.py --bad-list): '#' starts a comment that runs to the
 * end of the line; otherwise decimal block numbers separated by spaces, commas or new lines.
 * Its name is the image's with the extension changed to "bad" (OREGON.PO -> OREGON.BAD, x.po -> x.bad).
 *
 * Header only, plain C (the Mac front end and the TeensyROM module both include it).
 */
#ifndef A2_BADLIST_H
#define A2_BADLIST_H
#include <stddef.h>
#include <stdint.h>

/* Mark bad[b] = 1 for each block b listed in text[0..n). Returns how many were marked, or -1 if the text
   holds anything other than numbers, separators and comments, or a number at or past nblocks. */
static int a2_parse_badlist(const char *text, size_t n, uint8_t *bad, int nblocks) {
    int count = 0;
    size_t i = 0;
    while (i < n) {
        char c = text[i];
        if (c == '#') { while (i < n && text[i] != '\n') i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') { i++; continue; }
        if (c < '0' || c > '9') return -1;
        long v = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + (text[i] - '0');
            if (v >= nblocks) return -1;
            i++;
        }
        if (!bad[v]) count++;
        bad[v] = 1;
    }
    return count;
}

/* The list's path for an image path: the extension after the last '.' of the file name becomes "bad"
   ("BAD" when the extension's first letter is upper case). Returns 0 if out is too small or there is no
   extension. */
static int a2_badlist_path(const char *image, char *out, size_t outsize) {
    size_t len = 0, dot = 0, slash = 0;
    while (image[len]) { if (image[len] == '.') dot = len + 1; if (image[len] == '/') slash = len + 1; len++; }
    if (dot == 0 || dot <= slash || dot + 4 > outsize) return 0;
    for (size_t i = 0; i < dot; i++) out[i] = image[i];
    int upper = image[dot] >= 'A' && image[dot] <= 'Z';
    out[dot] = upper ? 'B' : 'b'; out[dot + 1] = upper ? 'A' : 'a'; out[dot + 2] = upper ? 'D' : 'd'; out[dot + 3] = 0;
    return 1;
}

#endif
