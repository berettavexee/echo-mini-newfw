/* Small helpers shared by the file system and the tag reader: little-endian fields, UTF-8 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>
#include "bytes.h"

/* Code point c in UTF-8 at d[*o], when it fits in max - 1 bytes (room for the final 0): 1, and
 * *o moves past it; else 0 */
static inline int utf8_put(char *d, int max, int *o, uint32_t c)
{
    int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (*o + len > max - 1)
        return 0;
    if (len == 1)
        d[(*o)++] = (char)c;
    else {
        static const uint8_t lead[5] = { 0, 0, 0xc0, 0xe0, 0xf0 };
        d[(*o)++] = (char)(lead[len] | c >> (6 * (len - 1)));
        for (int k = len - 2; k >= 0; k--)
            d[(*o)++] = (char)(0x80 | (c >> (6 * k) & 0x3f));
    }
    return 1;
}

#endif
