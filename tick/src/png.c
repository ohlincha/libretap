/* png.c - PNG writer with its own deflate (LZ77 with hash chains, fixed
 * Huffman codes).  Written for Tick; screenshots of old vendor programs are
 * mostly flat colours and text, which this compresses well and fast enough
 * for a Pentium II.  No dynamic Huffman, no lazy matching: speed first. */
#include "png.h"

#define WSIZE 32768u
#define WMASK (WSIZE - 1)
#define HBITS 15
#define HSIZE (1u << HBITS)
#define MAXCHAIN 12
#define NICE 96
#define MAXLEN 258

struct png_enc {
    u32 max_pixels;
    u8 *raw;            /* filtered scanlines */
    u32 raw_cap;
    u8 *out;            /* PNG file */
    u32 out_cap;
    u8 *row, *uprow;    /* RGB of the current and the previous scanline */
    u32 row_cap;
    u32 *head;          /* hash -> global position */
    u32 *prev;          /* global position & WMASK -> previous position */
    u32 base;           /* global position of data[0] in this call */
    /* bit writer */
    u8 *o;
    u32 bb, bc;
};

static u32 crc_tab[256];
static u16 lit_code[288];
static u8 lit_len[288];
static u16 len_sym[MAXLEN + 1];     /* length -> symbol 257..285 */
static u8 len_xbits[MAXLEN + 1];
static u16 len_xval[MAXLEN + 1];
static u8 dist_sym[512];
static const u16 dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                  513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_xbits[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
                                  8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const u16 len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
                                 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_xb[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
                              3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static u8 dist_code_rev[30];
static int tables_ready;

static u32 bitrev(u32 v, int n)
{
    u32 r = 0;
    while (n--) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void init_tables(void)
{
    u32 i, c;
    int k, s;
    if (tables_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    for (i = 0; i < 288; i++) {
        u32 code, len;
        if (i < 144) { code = 0x30 + i; len = 8; }
        else if (i < 256) { code = 0x190 + (i - 144); len = 9; }
        else if (i < 280) { code = i - 256; len = 7; }
        else { code = 0xc0 + (i - 280); len = 8; }
        lit_code[i] = (u16)bitrev(code, (int)len);
        lit_len[i] = (u8)len;
    }
    for (s = 0; s < 29; s++) {
        u32 n = s == 28 ? 1 : 1u << len_xb[s];
        for (i = 0; i < n && len_base[s] + i <= MAXLEN; i++) {
            u32 l = len_base[s] + i;
            if (s == 27 && l == 258) break;     /* 258 has its own code */
            len_sym[l] = (u16)(257 + s);
            len_xbits[l] = len_xb[s];
            len_xval[l] = (u16)i;
        }
    }
    len_sym[258] = 285; len_xbits[258] = 0; len_xval[258] = 0;
    for (s = 0; s < 30; s++) {
        dist_code_rev[s] = (u8)bitrev((u32)s, 5);
        for (i = dist_base[s]; i < (u32)dist_base[s] + (1u << dist_xbits[s]) && i <= 32768; i++) {
            if (i <= 256) dist_sym[i - 1] = (u8)s;
            else dist_sym[256 + ((i - 1) >> 7)] = (u8)s;
        }
    }
    tables_ready = 1;
}

u32 png_crc32(u32 crc, const u8 *p, u32 n)
{
    init_tables();
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static u32 adler32(const u8 *p, u32 n)
{
    u32 a = 1, b = 0;
    while (n) {
        u32 k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

png_enc *png_new(u32 max_pixels)
{
    png_enc *e;
    init_tables();
    e = rt_alloc(sizeof *e);
    if (!e) return 0;
    e->max_pixels = max_pixels;
    e->raw_cap = max_pixels * 3 + 4 * 4096 + 64;
    e->raw = rt_alloc(e->raw_cap);
    e->out_cap = e->raw_cap + e->raw_cap / 8 + 1024;
    e->out = rt_alloc(e->out_cap);
    e->head = rt_alloc(HSIZE * 4);
    e->prev = rt_alloc(WSIZE * 4);
    e->base = 1;
    if (!e->raw || !e->out || !e->head || !e->prev) { png_delete(e); return 0; }
    return e;
}

void png_delete(png_enc *e)
{
    if (!e) return;
    rt_free(e->raw); rt_free(e->out); rt_free(e->head); rt_free(e->prev);
    rt_free(e->row); rt_free(e->uprow);
    rt_free(e);
}

#define PUTBITS(e, v, n) do { \
    (e)->bb |= (u32)(v) << (e)->bc; (e)->bc += (n); \
    while ((e)->bc >= 8) { *(e)->o++ = (u8)(e)->bb; (e)->bb >>= 8; (e)->bc -= 8; } } while (0)

static inline u32 hash3(const u8 *p)
{
    return (((u32)p[0] << 16 | (u32)p[1] << 8 | p[2]) * 2654435761u) >> (32 - HBITS);
}

/* zlib stream of data[0..n) at e->o; returns the end pointer */
static u8 *deflate_fixed(png_enc *e, const u8 *data, u32 n, u8 *o)
{
    u32 i = 0, base;
    u32 *head = e->head, *prev = e->prev;
    if (e->base > 0x70000000u) {            /* positions would overflow: start over */
        memset(head, 0, HSIZE * 4);
        e->base = 1;
    }
    base = e->base;
    e->o = o;
    e->bb = 0; e->bc = 0;
    *e->o++ = 0x78; *e->o++ = 0x01;          /* zlib: deflate, 32K window, fastest */
    PUTBITS(e, 1, 1);                        /* final block */
    PUTBITS(e, 1, 2);                        /* fixed Huffman */
    while (i < n) {
        u32 best = 0, bestd = 0;
        if (i + 2 < n) {
            u32 gp = base + i, h = hash3(data + i), cand = head[h];
            u32 maxl = n - i < MAXLEN ? n - i : MAXLEN;
            int chain = MAXCHAIN;
            head[h] = gp;
            prev[gp & WMASK] = cand;
            while (cand >= base && gp - cand < WSIZE && chain--) {
                const u8 *c = data + (cand - base), *d = data + i;
                if (c[best < maxl ? best : 0] == d[best < maxl ? best : 0] && c[0] == d[0] && c[1] == d[1]) {
                    u32 l = 2;
                    while (l < maxl && c[l] == d[l]) l++;
                    if (l > best) {
                        best = l; bestd = gp - cand;
                        if (l >= NICE || l == maxl) break;
                    }
                }
                cand = prev[cand & WMASK];
            }
        }
        if (best >= 3) {
            u32 ls = len_sym[best], ds;
            PUTBITS(e, lit_code[ls], lit_len[ls]);
            if (len_xbits[best]) PUTBITS(e, len_xval[best], len_xbits[best]);
            ds = bestd <= 256 ? dist_sym[bestd - 1] : dist_sym[256 + ((bestd - 1) >> 7)];
            PUTBITS(e, dist_code_rev[ds], 5);
            if (dist_xbits[ds]) PUTBITS(e, bestd - dist_base[ds], dist_xbits[ds]);
            if (best <= 32) {
                u32 j, end = i + best;
                for (j = i + 1; j < end && j + 2 < n; j++) {
                    u32 gp = base + j, h = hash3(data + j);
                    prev[gp & WMASK] = head[h];
                    head[h] = gp;
                }
            }
            i += best;
        } else {
            PUTBITS(e, lit_code[data[i]], lit_len[data[i]]);
            i++;
        }
    }
    PUTBITS(e, lit_code[256], lit_len[256]);
    if (e->bc) { *e->o++ = (u8)e->bb; e->bb = 0; e->bc = 0; }
    {
        u32 a = adler32(data, n);
        *e->o++ = (u8)(a >> 24); *e->o++ = (u8)(a >> 16); *e->o++ = (u8)(a >> 8); *e->o++ = (u8)a;
    }
    e->base = base + n + WSIZE;
    return e->o;
}

const u8 *png_zlib(png_enc *e, const u8 *data, u32 n, u32 *size)
{
    u8 *end;
    if (n > e->raw_cap) return 0;
    end = deflate_fixed(e, data, n, e->out);
    *size = (u32)(end - e->out);
    return e->out;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static inline u32 sabs(u8 v) { return v < 128 ? v : 256u - v; }

/* PNG filters per scanline: None, Sub or Up, whichever has the smallest sum
 * of absolute values (the usual heuristic). */
static u32 filter_rows(png_enc *e, const u8 *img, u32 stride, u32 x, u32 y, u32 w, u32 h)
{
    u32 rb = w * 3, r, i;
    u8 *o = e->raw, *t;
    if (rb > e->row_cap) {
        rt_free(e->row); rt_free(e->uprow);
        e->row = rt_alloc(rb); e->uprow = rt_alloc(rb);
        e->row_cap = (e->row && e->uprow) ? rb : 0;
        if (!e->row_cap) return 0;
    }
    memset(e->uprow, 0, rb);
    for (r = 0; r < h; r++) {
        const u8 *s = img + (y + r) * stride + x * 4;
        u8 *c = e->row, *u = e->uprow;
        u32 sn = 0, ss = 0, su = 0;
        for (i = 0; i < w; i++) {       /* BGRX -> RGB */
            c[i * 3] = s[i * 4 + 2];
            c[i * 3 + 1] = s[i * 4 + 1];
            c[i * 3 + 2] = s[i * 4];
        }
        for (i = 0; i < rb; i++) {
            u8 v = c[i];
            sn += sabs(v);
            ss += sabs((u8)(v - (i >= 3 ? c[i - 3] : 0)));
            su += sabs((u8)(v - u[i]));
        }
        if (sn <= ss && sn <= su) {
            *o++ = 0;
            memcpy(o, c, rb);
            o += rb;
        } else if (su <= ss) {
            *o++ = 2;
            for (i = 0; i < rb; i++) *o++ = (u8)(c[i] - u[i]);
        } else {
            *o++ = 1;
            for (i = 0; i < 3 && i < rb; i++) *o++ = c[i];
            for (; i < rb; i++) *o++ = (u8)(c[i] - c[i - 3]);
        }
        t = e->uprow; e->uprow = e->row; e->row = t;
    }
    return (u32)(o - e->raw);
}

const u8 *png_encode_bgrx(png_enc *e, const u8 *img, u32 stride,
                          u32 x, u32 y, u32 w, u32 h, u32 *size)
{
    static const u8 sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    u8 *p = e->out, *z, *end;
    u32 rawlen;
    if (!w || !h || (u64)w * h > e->max_pixels || (u64)h * (w * 3 + 1) > e->raw_cap) return 0;
    rawlen = filter_rows(e, img, stride, x, y, w, h);
    if (!rawlen) return 0;
    memcpy(p, sig, 8);
    put32(p + 8, 13);
    memcpy(p + 12, "IHDR", 4);
    put32(p + 16, w);
    put32(p + 20, h);
    p[24] = 8; p[25] = 2; p[26] = 0; p[27] = 0; p[28] = 0;    /* 8 bit RGB */
    put32(p + 29, png_crc32(0, p + 12, 17));
    z = p + 41;
    end = deflate_fixed(e, e->raw, rawlen, z);
    put32(p + 33, (u32)(end - z));
    memcpy(p + 37, "IDAT", 4);
    put32(end, png_crc32(0, p + 37, (u32)(end - z) + 4));
    end += 4;
    put32(end, 0);
    memcpy(end + 4, "IEND", 4);
    put32(end + 8, 0xae426082u);
    end += 12;
    *size = (u32)(end - p);
    return p;
}
