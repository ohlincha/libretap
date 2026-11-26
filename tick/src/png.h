/* png.h - small PNG writer for screenshots (RGB, 8 bit, own deflate). */
#ifndef TICK_PNG_H
#define TICK_PNG_H
#include "rt.h"

typedef struct png_enc png_enc;

/* max_pixels: largest region (w*h) that will be encoded; memory is reserved once. */
png_enc *png_new(u32 max_pixels);
void png_delete(png_enc *e);

/* Encode the w x h region at (x, y) of a top-down 32-bit BGRX image with
 * 'stride' bytes per row.  Returns the PNG file bytes (valid until the next
 * call) and sets *size, or returns NULL if the region is too large. */
const u8 *png_encode_bgrx(png_enc *e, const u8 *img, u32 stride,
                          u32 x, u32 y, u32 w, u32 h, u32 *size);

/* Raw zlib stream of 'n' bytes (used by the tests). */
const u8 *png_zlib(png_enc *e, const u8 *data, u32 n, u32 *size);

u32 png_crc32(u32 crc, const u8 *p, u32 n);

#endif
