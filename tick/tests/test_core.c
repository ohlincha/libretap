/* test_core.c - native (Linux) tests of the portable parts of Tick:
 * deflate/PNG writer, screen diff, event ring, text helpers.
 * Usage: test_core OUTDIR   (then tests/verify_png.py OUTDIR checks the files) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../src/rt.h"
#include "../src/png.h"
#include "../src/diff.h"
#include "../src/ring.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void save(const char *dir, const char *name, const void *p, size_t n)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(p, 1, n, f);
    fclose(f);
}

/* a fake "screen": window frames, flat areas, text-like noise, a gradient */
static void paint_screen(u8 *img, u32 w, u32 h, u32 seed)
{
    u32 x, y, i;
    rng = seed;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            u8 *p = img + (y * w + x) * 4;
            p[0] = 0x80; p[1] = 0x80; p[2] = 0x00; p[3] = 0;   /* teal desktop */
        }
    for (i = 0; i < 6; i++) {
        u32 x0 = rnd() % w, y0 = rnd() % h, ww = 40 + rnd() % (w / 2), hh = 30 + rnd() % (h / 2);
        u8 c = (u8)(0xc0 + i * 5);
        for (y = y0; y < y0 + hh && y < h; y++)
            for (x = x0; x < x0 + ww && x < w; x++) {
                u8 *p = img + (y * w + x) * 4;
                int title = y < y0 + 18;
                p[0] = title ? 0x80 : c; p[1] = title ? 0 : c; p[2] = title ? 0 : c;
                if (!title && (y - y0) % 14 < 9 && (x - x0) % 7 < 5 && rnd() % 3 == 0) { p[0] = p[1] = p[2] = 0; }
            }
    }
    for (y = h - 40; y < h; y++)            /* gradient strip */
        for (x = 0; x < w; x++) {
            u8 *p = img + (y * w + x) * 4;
            p[0] = (u8)x; p[1] = (u8)(x + y); p[2] = (u8)(y * 3);
        }
}

static void write_rgb(const char *dir, const char *name, const u8 *img, u32 stride, u32 x0, u32 y0, u32 w, u32 h)
{
    u8 *rgb = malloc((size_t)w * h * 3);
    u32 x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const u8 *s = img + (y0 + y) * stride + (x0 + x) * 4;
            u8 *d = rgb + (y * w + x) * 3;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
        }
    save(dir, name, rgb, (size_t)w * h * 3);
    free(rgb);
}

static void test_zlib(const char *dir)
{
    png_enc *e = png_new(1 << 20);
    u8 *d = malloc(3 << 20);
    u32 sizes[] = {0, 1, 2, 3, 4, 257, 258, 259, 1000, 40000, 100000, 3000000};
    u32 i, k, n;
    char name[64];
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        n = sizes[k];
        for (i = 0; i < n; i++) {           /* mixture: runs, repeats far back, noise */
            u32 m = (i / 5000) % 4;
            d[i] = m == 0 ? (u8)rnd() : m == 1 ? 'a' : m == 2 ? (u8)(i % 251) : (i > 33000 ? d[i - 32768] : (u8)rnd());
        }
        {
            u32 zs;
            const u8 *z = png_zlib(e, d, n, &zs);
            CHECK(z != 0);
            snprintf(name, sizeof name, "z%02u.bin", k);
            save(dir, name, d, n);
            snprintf(name, sizeof name, "z%02u.zlib", k);
            if (z) save(dir, name, z, zs);
        }
    }
    free(d);
    png_delete(e);
}

static void test_png(const char *dir)
{
    u32 w = 1024, h = 768, size, i;
    u8 *img = malloc(w * h * 4);
    png_enc *e = png_new(w * h);
    const u8 *p;
    char name[64];
    struct { u32 x, y, w, h; } regions[] = {{0, 0, 1024, 768}, {10, 20, 300, 40}, {1023, 767, 1, 1}, {0, 700, 1024, 68}, {500, 0, 1, 768}};
    paint_screen(img, w, h, 7);
    for (i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        p = png_encode_bgrx(e, img, w * 4, regions[i].x, regions[i].y, regions[i].w, regions[i].h, &size);
        CHECK(p != 0);
        snprintf(name, sizeof name, "img%u.png", i);
        if (p) save(dir, name, p, size);
        snprintf(name, sizeof name, "img%u.rgb", i);
        write_rgb(dir, name, img, w * 4, regions[i].x, regions[i].y, regions[i].w, regions[i].h);
        if (i == 0) printf("full 1024x768 screen-like frame: %u bytes PNG\n", size);
    }
    CHECK(png_encode_bgrx(e, img, w * 4, 0, 0, 1025, 768, &size) == 0);
    free(img);
    png_delete(e);
}

static void test_diff(void)
{
    u32 w = 1000, h = 700, tile = 32, tx = (w + 31) / 32, ty = (h + 31) / 32;
    u8 *a = malloc(w * h * 4), *b = malloc(w * h * 4), *c = malloc(w * h * 4), *map = malloc(tx * ty);
    trect r[64];
    u32 n, px, i, y, round;
    paint_screen(a, w, h, 1);
    memcpy(b, a, w * h * 4);
    n = diff_rects(b, a, w * 4, w, h, tile, map, r, 64, &px);
    CHECK(n == 0 && px == 0);
    for (round = 0; round < 50; round++) {
        u32 nchanges = 1 + rnd() % (round < 25 ? 5 : 400);
        memcpy(b, a, w * h * 4);
        for (i = 0; i < nchanges; i++) {
            u32 x = rnd() % w, yy = rnd() % h, k, len = 1 + rnd() % 60;
            for (k = 0; k < len && x + k < w; k++) b[(yy * w + x + k) * 4 + (k % 3)] ^= 0x55;
        }
        n = diff_rects(b, a, w * 4, w, h, tile, map, r, 64, &px);
        CHECK(n > 0);
        /* painting the regions of b onto a must give b */
        memcpy(c, a, w * h * 4);
        for (i = 0; i < n; i++) {
            CHECK(r[i].x + r[i].w <= w && r[i].y + r[i].h <= h && r[i].w && r[i].h);
            for (y = r[i].y; y < r[i].y + r[i].h; y++)
                memcpy(c + (y * w + r[i].x) * 4, b + (y * w + r[i].x) * 4, r[i].w * 4);
        }
        CHECK(memcmp(c, b, w * h * 4) == 0);
        if (round < 25) CHECK(px <= nchanges * 3 * 32 * 32 * 2);
    }
    /* edge tile at the bottom right corner */
    memcpy(b, a, w * h * 4);
    b[((h - 1) * w + w - 1) * 4] ^= 1;
    n = diff_rects(b, a, w * 4, w, h, tile, map, r, 64, &px);
    CHECK(n == 1 && r[0].x == 992 && r[0].y == 672 && r[0].w == 8 && r[0].h == 28);
    free(a); free(b); free(c); free(map);
}

/* ring: 4 writer threads, 1 reader, payload checked */
static ring_hdr *R;
static volatile int writers_done;
#define PER_THREAD 200000

static void *writer(void *arg)
{
    u32 id = (u32)(size_t)arg, i;
    u8 pay[300];
    for (i = 0; i < PER_THREAD; i++) {
        ring_rec rec;
        u32 n = (i * 7 + id) % 300, k;
        memset(&rec, 0, sizeof rec);
        rec.type = REC_WRITE;
        rec.tid = id;
        rec.a = i;
        for (k = 0; k < n; k++) pay[k] = (u8)(i + k + id);
        ring_put(R, &rec, pay, n / 2, pay + n / 2, n - n / 2);
    }
    __sync_fetch_and_add(&writers_done, 1);
    return 0;
}

static void test_ring(void)
{
    pthread_t th[4];
    u32 got = 0, i, last[4] = {0, 0, 0, 0}, bad = 0;
    int t;
    static u8 buf[RING_MAXREC + 64];
    R = calloc(1, sizeof(ring_hdr) + (1 << 16));
    ring_init(R, 1 << 16);
    for (t = 0; t < 4; t++) pthread_create(&th[t], 0, writer, (void *)(size_t)t);
    for (;;) {
        u32 n = ring_get(R, buf, sizeof buf);
        if (!n) {
            if (writers_done == 4 && R->tail == R->reserve) break;
            continue;
        }
        {
            ring_rec *rec = (ring_rec *)buf;
            u32 k, len = rec->dlen;
            const u8 *p = buf + sizeof(ring_rec);
            if (rec->tid >= 4 || len != (rec->a * 7 + rec->tid) % 300) bad++;
            else {
                for (k = 0; k < len; k++) if (p[k] != (u8)(rec->a + k + rec->tid)) { bad++; break; }
                if (last[rec->tid] && rec->a <= last[rec->tid] - 1) bad++;   /* order per thread */
                last[rec->tid] = rec->a + 1;
            }
            got++;
        }
    }
    for (t = 0; t < 4; t++) pthread_join(th[t], 0);
    printf("ring: %u records read, %u dropped, %u bad\n", got, R->drops, bad);
    CHECK(bad == 0);
    CHECK(got + R->drops == 4 * PER_THREAD);
    for (i = 0; i < (1u << 16) / 4; i++)
        if (((u32 *)(R + 1))[i]) { bad++; break; }
    CHECK(bad == 0);
    free(R);
}

static void test_text(void)
{
    sbuf b;
    u64 q, r;
    sb_init(&b, 4);
    sb_time(&b, 1796031300123456ull);
    CHECK(strcmp(b.p, "1796031300.123456") == 0);
    sb_reset(&b);
    sb_time(&b, 5);
    CHECK(strcmp(b.p, "0.000005") == 0);
    sb_reset(&b);
    sb_jsonz(&b, "a\"b\\c\n\x01 \xc3\xa9 \xff \xe2\x82\xac");
    CHECK(strcmp(b.p, "\"a\\\"b\\\\c\\n\\u0001 \xc3\xa9 \xef\xbf\xbd \xe2\x82\xac\"") == 0);
    sb_reset(&b);
    {
        u16 w[] = {'C', 0xe9, 0x20ac, 0xd83d, 0xde00, 0};
        sb_utf16(&b, w, 0xffffffffu);
        CHECK(strcmp(b.p, "C\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0);
    }
    sb_reset(&b);
    {
        u8 x[] = {0x00, 0x0f, 0xab, 0xff};
        sb_hexbytes(&b, x, 4);
        CHECK(strcmp(b.p, "000fabff") == 0);
    }
    sb_reset(&b);
    sb_i32(&b, -2147483647 - 1);
    CHECK(strcmp(b.p, "-2147483648") == 0);
    sb_free(&b);
    for (r = 0; r < 100000; r++) {
        u64 n = ((u64)rnd() << 40) ^ ((u64)rnd() << 16) ^ rnd(), d = r % 3 ? (rnd() | 1) : (((u64)rnd() << 24) | 1), rem;
        q = rt_udiv64(n, d, &rem);
        if (q != n / d || rem != n % d) { CHECK(0); break; }
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_text();
    test_diff();
    test_ring();
    test_zlib(dir);
    test_png(dir);
    printf(fails ? "%d FAILED\n" : "all native tests passed\n", fails);
    return fails != 0;
}
