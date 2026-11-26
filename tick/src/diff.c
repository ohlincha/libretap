/* diff.c - see diff.h */
#include "diff.h"

u32 diff_rects(const u8 *cur, const u8 *prev, u32 stride, u32 w, u32 h, u32 tile,
               u8 *tilemap, trect *out, u32 maxr, u32 *changed_px)
{
    u32 tx = (w + tile - 1) / tile, ty = (h + tile - 1) / tile;
    u32 r, c, y, n = 0, i, area = 0;
    u32 bx0 = tx, by0 = ty, bx1 = 0, by1 = 0;
    int overflow = 0;

    memset(tilemap, 0, tx * ty);
    for (r = 0; r < ty; r++) {
        u8 *row = tilemap + r * tx;
        u32 y0 = r * tile, y1 = y0 + tile < h ? y0 + tile : h;
        u32 left = tx;                      /* tiles in this row not yet known to differ */
        for (y = y0; y < y1 && left; y++) {
            const u8 *a = cur + y * stride, *b = prev + y * stride;
            for (c = 0; c < tx; c++) {
                u32 x0, x1;
                if (row[c]) continue;
                x0 = c * tile;
                x1 = x0 + tile < w ? x0 + tile : w;
                if (memcmp(a + x0 * 4, b + x0 * 4, (x1 - x0) * 4)) { row[c] = 1; left--; }
            }
        }
    }

    /* merge: horizontal runs per tile row, extended downwards when a run of
     * the next row has exactly the same span (rects kept in tile units) */
    for (r = 0; r < ty && !overflow; r++) {
        u8 *row = tilemap + r * tx;
        c = 0;
        while (c < tx) {
            u32 a;
            if (!row[c]) { c++; continue; }
            a = c;
            while (c < tx && row[c]) c++;
            if (a < bx0) bx0 = a;
            if (c > bx1) bx1 = c;
            if (r < by0) by0 = r;
            by1 = r + 1;
            for (i = 0; i < n; i++)
                if (out[i].x == a && out[i].w == c - a && out[i].y + out[i].h == r) break;
            if (i < n) out[i].h++;
            else if (n < maxr) { out[n].x = a; out[n].y = r; out[n].w = c - a; out[n].h = 1; n++; }
            else overflow = 1;
        }
    }
    if (overflow) {                         /* finish the bounding box */
        for (; r < ty; r++)
            for (c = 0; c < tx; c++)
                if (tilemap[r * tx + c]) {
                    if (c < bx0) bx0 = c;
                    if (c + 1 > bx1) bx1 = c + 1;
                    by1 = r + 1;
                }
        n = maxr ? 1 : 0;
        if (n) { out[0].x = bx0; out[0].y = by0; out[0].w = bx1 - bx0; out[0].h = by1 - by0; }
    }
    for (i = 0; i < n; i++) {               /* tiles -> pixels, clipped */
        u32 x0 = out[i].x * tile, y0 = out[i].y * tile;
        u32 x1 = (out[i].x + out[i].w) * tile, y1 = (out[i].y + out[i].h) * tile;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        out[i].x = x0; out[i].y = y0; out[i].w = x1 - x0; out[i].h = y1 - y0;
        area += out[i].w * out[i].h;
    }
    if (changed_px) *changed_px = area;
    return n;
}
