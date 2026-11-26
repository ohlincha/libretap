/* capture_x11.c - Linux screenshots: like Tick for Windows (capture.c), every
 * interval on a low-priority thread, 32x32 tile diff, changed rectangles as
 * PNG, full picture at the start / every Nth / on large changes. */
#define _GNU_SOURCE
#include "tickx.h"
#include "x11.h"
#include "../png.h"
#include "../diff.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAXRECTS 48

unsigned long g_mask_window;
static pthread_t th;
static volatile int running, stopping;
static char err[300];
static x11 X;
static u8 *bits[2], *tilemap;
static int cur, need_full, paused;
static u32 since_full, file_n;
static png_enc *png;
static trect rects[MAXRECTS];
static char title[512];
static sbuf ev;

const char *capture_error(void) { return err; }

static int store(u64 t, const u8 *img, int full, u32 x, u32 y, u32 w, u32 h)
{
    u32 size;
    const u8 *p = png_encode_bgrx(png, img, X.w * 4, x, y, w, h, &size);
    char rel[32];
    if (!p) return 0;
    file_n++;
    snprintf(rel, sizeof rel, "frames/%06u.png", file_n);
    if (!session_write_file(rel, p, size)) return 0;
    ev_begin(&ev, t, "frame");
    sb_str(&ev, ",\"src\":\"screen\",\"file\":\"");
    sb_str(&ev, rel);
    sb_str(&ev, full ? "\",\"full\":true" : "\",\"full\":false");
    sb_str(&ev, ",\"x\":"); sb_u32(&ev, x);
    sb_str(&ev, ",\"y\":"); sb_u32(&ev, y);
    sb_str(&ev, ",\"w\":"); sb_u32(&ev, w);
    sb_str(&ev, ",\"h\":"); sb_u32(&ev, h);
    ev_end(&ev);
    return 1;
}

static void mask_self(u8 *img)
{
    int x, y, w, h, yy;
    if (!g_cfg.mask_self || !g_mask_window) return;
    if (!x11_window_rect(&X, (u32)g_mask_window, &x, &y, &w, &h)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)X.w) w = (int)X.w - x;
    if (y + h > (int)X.h) h = (int)X.h - y;
    if (w <= 0 || h <= 0) return;
    for (yy = y; yy < y + h; yy++) memset(img + ((u32)yy * X.w + (u32)x) * 4, 0xa0, (u32)w * 4);
}

static void shot(void)
{
    u64 t = clock_now_us();
    u8 *c = bits[cur], *p = bits[cur ^ 1];
    u32 n = 0, area = 0, i;
    int full;
    if (paused) return;
    if (!x11_get_image(&X, c)) {
        ev_status("error", "The X display stopped answering: screen recording stopped");
        paused = 1;
        return;
    }
    mask_self(c);
    full = need_full || since_full + 1 >= g_cfg.full_every;
    if (!full) {
        n = diff_rects(c, p, X.w * 4, X.w, X.h, g_cfg.tile, tilemap, rects, MAXRECTS, &area);
        if (!n) return;
        if ((u64)area * 2 > (u64)X.w * X.h) full = 1;
    }
    if (full) {
        if (!store(t, c, 1, 0, 0, X.w, X.h)) goto fail;
        since_full = 0;
        need_full = 0;
    } else {
        for (i = 0; i < n; i++)
            if (!store(t, c, 0, rects[i].x, rects[i].y, rects[i].w, rects[i].h)) goto fail;
        since_full++;
    }
    __sync_fetch_and_add(&g_frames_stored, 1);
    cur ^= 1;
    return;
fail:
    cur ^= 1;
    need_full = 1;
    ev_status("error", "Could not write a screenshot (disk full or removed?)");
}

static void window_title(void)
{
    char t[512];
    if (x11_active_title(&X, t, sizeof t) < 0) return;
    if (!strcmp(t, title)) return;
    copy_str(title, t, sizeof title);
    ev_begin(&ev, clock_now_us(), "input");
    sb_str(&ev, ",\"src\":\"input\",\"what\":\"window\",\"title\":");
    sb_jsonz(&ev, t);
    ev_end(&ev);
}

static void *loop(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, 0, 10);       /* this thread only (Linux): be nice to the vendor program */
    while (!stopping) {
        u32 t0 = p_ms(), el;
        shot();
        if (g_cfg.titles) window_title();
        el = p_ms() - t0;
        {
            u32 wait = g_cfg.interval_ms > el + 10 ? g_cfg.interval_ms - el : 10;
            while (wait && !stopping) {
                u32 s = wait > 50 ? 50 : wait;
                struct timespec ts = {0, (long)s * 1000000};
                nanosleep(&ts, 0);
                wait -= s;
            }
        }
    }
    return 0;
}

int capture_start(void)
{
    char json[200];
    if (running) return 1;
    err[0] = 0;
    if (!x11_open(&X, err, sizeof err)) {
        ev_status("status", err);
        return 0;
    }
    bits[0] = calloc(X.w * X.h, 4);
    bits[1] = calloc(X.w * X.h, 4);
    tilemap = calloc(((X.w + g_cfg.tile - 1) / g_cfg.tile) * ((X.h + g_cfg.tile - 1) / g_cfg.tile), 1);
    png = png_new(X.w * X.h);
    if (!bits[0] || !bits[1] || !tilemap || !png) {
        snprintf(err, sizeof err, "Not enough memory for screenshots");
        x11_close(&X);
        return 0;
    }
    snprintf(json, sizeof json, "{\"id\": \"screen\", \"kind\": \"screen\", \"label\": \"Screen %ux%u (X11 %s)\", "
             "\"width\": %u, \"height\": %u}", X.w, X.h, getenv("DISPLAY"), X.w, X.h);
    session_add_source("screen", json);
    if (g_cfg.titles)
        session_add_source("input", "{\"id\": \"input\", \"kind\": \"input\", \"label\": \"Active window title\"}");
    session_mkdir("frames");
    sb_init(&ev, 256);
    cur = 0; need_full = 1; paused = 0; since_full = 0; file_n = 0; title[0] = 0;
    stopping = 0;
    if (pthread_create(&th, 0, loop, 0)) { x11_close(&X); return 0; }
    running = 1;
    return 1;
}

void capture_stop(void)
{
    if (!running) return;
    stopping = 1;
    pthread_join(th, 0);
    running = 0;
    x11_close(&X);
    free(bits[0]); free(bits[1]); free(tilemap);
    bits[0] = bits[1] = tilemap = 0;
    png_delete(png);
    png = 0;
    sb_free(&ev);
}
