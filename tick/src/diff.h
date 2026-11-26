/* diff.h - find the changed parts of a screenshot */
#ifndef TICK_DIFF_H
#define TICK_DIFF_H
#include "rt.h"

typedef struct { u32 x, y, w, h; } trect;

/* Compare two top-down 32-bit images (same size and stride) in tile x tile
 * squares and merge the changed tiles into rectangles (in pixels, clipped to
 * the image).  tilemap needs ceil(w/tile)*ceil(h/tile) bytes.  At most maxr
 * rectangles are returned; if more would be needed, the bounding box of all
 * changes is returned instead.  *changed_px gets the area of the result. */
u32 diff_rects(const u8 *cur, const u8 *prev, u32 stride, u32 w, u32 h, u32 tile,
               u8 *tilemap, trect *out, u32 maxr, u32 *changed_px);

#endif
