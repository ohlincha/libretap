/* x11.c - the few X11 protocol requests Tick needs, spoken directly over the
 * X server's socket: no libX11 needed, so the tick binary can be static and
 * run on any Linux with an X server (Xorg, Xvfb, XWayland for X clients).
 * Requests: GetImage (screen), InternAtom/GetProperty (active window title),
 * GetGeometry/TranslateCoordinates (Tick's own terminal window). */
#define _GNU_SOURCE
#include "x11.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

static int rd(int fd, void *p, u32 n)
{
    u8 *c = p;
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        c += r; n -= (u32)r;
    }
    return 1;
}

static int wr(int fd, const void *p, u32 n)
{
    const u8 *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        c += w; n -= (u32)w;
    }
    return 1;
}

static u16 g16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 g32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* MIT-MAGIC-COOKIE-1 for display number dpy from $XAUTHORITY or ~/.Xauthority */
static int find_cookie(const char *dpy, u8 *cookie)
{
    char path[1024];
    const char *xa = getenv("XAUTHORITY"), *home = getenv("HOME");
    FILE *f;
    int found = 0;
    if (xa && *xa) snprintf(path, sizeof path, "%s", xa);
    else if (home) snprintf(path, sizeof path, "%s/.Xauthority", home);
    else return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    for (;;) {
        u8 h[2];
        char fields[4][256];
        u32 lens[4];
        int i, ok = 1;
        if (fread(h, 1, 2, f) != 2) break;
        for (i = 0; i < 4; i++) {
            u8 l[2];
            if (fread(l, 1, 2, f) != 2) { ok = 0; break; }
            lens[i] = (u32)(l[0] << 8 | l[1]);
            if (lens[i] >= 256) { fseek(f, lens[i], SEEK_CUR); lens[i] = 0; fields[i][0] = 0; continue; }
            if (fread(fields[i], 1, lens[i], f) != lens[i]) { ok = 0; break; }
            fields[i][lens[i]] = 0;
        }
        if (!ok) break;
        /* fields: address, display number, auth name, auth data */
        if (lens[2] == 18 && !memcmp(fields[2], "MIT-MAGIC-COOKIE-1", 18) && lens[3] == 16 &&
            (lens[1] == 0 || !strcmp(fields[1], dpy))) {
            memcpy(cookie, fields[3], 16);
            found = 1;
            if (lens[1]) break;     /* exact display number wins */
        }
    }
    fclose(f);
    return found;
}

static int connect_unix(const char *path, int abstract)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    socklen_t len;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (abstract) {
        a.sun_path[0] = 0;
        snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "%s", path);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path));
    } else {
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        len = sizeof a;
    }
    if (connect(fd, (struct sockaddr *)&a, len) < 0) { close(fd); return -1; }
    return fd;
}

int x11_open(x11 *x, char *err, u32 errcap)
{
    const char *d = getenv("DISPLAY");
    char num[16], path[108];
    u8 cookie[16], req[48], hdr[8], *s = 0;
    int have_cookie, i, scr = 0;
    u32 add, off, nformats, nscreens, vendor_len;

    memset(x, 0, sizeof *x);
    x->fd = -1;
    if (!d || !*d) {
        if (getenv("WAYLAND_DISPLAY"))
            snprintf(err, errcap, "Wayland desktop without X: screen recording is not supported there yet "
                     "(log in to an X11/Xorg session, or record with LibreTap's camera)");
        else snprintf(err, errcap, "No X display (DISPLAY is not set): screen recording is off");
        return 0;
    }
    if (d[0] != ':' && strncmp(d, "unix:", 5)) {
        snprintf(err, errcap, "DISPLAY=%s: only local X displays are supported", d);
        return 0;
    }
    d = strchr(d, ':') + 1;
    for (i = 0; d[i] >= '0' && d[i] <= '9' && i < 15; i++) num[i] = d[i];
    num[i] = 0;
    if (d[i] == '.') scr = atoi(d + i + 1);
    snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", num);
    x->fd = connect_unix(path, 0);
    if (x->fd < 0) x->fd = connect_unix(path, 1);
    if (x->fd < 0) { snprintf(err, errcap, "Cannot connect to the X display :%s", num); return 0; }
    have_cookie = find_cookie(num, cookie);
    memset(req, 0, sizeof req);
    req[0] = 'l';
    p16(req + 2, 11);
    p16(req + 4, 0);
    if (have_cookie) {
        p16(req + 6, 18);
        p16(req + 8, 16);
        memcpy(req + 12, "MIT-MAGIC-COOKIE-1", 18);     /* padded to 20 */
        memcpy(req + 32, cookie, 16);
    }
    if (!wr(x->fd, req, have_cookie ? 48 : 12) || !rd(x->fd, hdr, 8)) goto fail;
    add = g16(hdr + 6) * 4u;
    s = malloc(add + 1);
    if (!s || !rd(x->fd, s, add)) goto fail;
    if (hdr[0] != 1) {
        snprintf(err, errcap, "The X display refused Tick: %.*s", hdr[1] < add ? hdr[1] : (int)add, (char *)s);
        goto fail2;
    }
    x->rid = g32(s + 4);
    x->rid_mask = g32(s + 8);
    x->min_kc = s[26];
    x->max_kc = s[27];
    vendor_len = g16(s + 16);
    nscreens = s[20];
    nformats = s[21];
    x->lsb = s[22] == 0;
    off = 32 + ((vendor_len + 3) & ~3u);
    for (i = 0; i < (int)nformats; i++, off += 8)
        if (x->nfmt < 8) { x->fmt_depth[x->nfmt] = s[off]; x->fmt_bpp[x->nfmt] = s[off + 1]; x->fmt_pad[x->nfmt] = s[off + 2]; x->nfmt++; }
    if (scr >= (int)nscreens) scr = 0;
    for (i = 0; i < (int)nscreens && off + 40 <= add; i++) {
        u32 ndepths = s[off + 39], k, o = off + 40;
        if (i == scr) {
            x->root = g32(s + off);
            x->w = g16(s + off + 20);
            x->h = g16(s + off + 22);
            x->root_visual = g32(s + off + 32);
            x->depth = s[off + 38];
        }
        for (k = 0; k < ndepths; k++) {
            u32 nvis = g16(s + o + 2), v;
            o += 8;
            for (v = 0; v < nvis; v++, o += 24)
                if (i == scr && g32(s + o) == x->root_visual) {
                    x->rmask = g32(s + o + 8); x->gmask = g32(s + o + 12); x->bmask = g32(s + o + 16);
                }
        }
        off = o;
    }
    free(s);
    for (i = 0; i < x->nfmt; i++) if (x->fmt_depth[i] == x->depth) { x->bpp = x->fmt_bpp[i]; x->pad = x->fmt_pad[i]; }
    if (!x->root || (x->bpp != 32 && x->bpp != 16) || !x->rmask) {
        snprintf(err, errcap, "Unsupported X screen (depth %u, %u bits per pixel)", x->depth, x->bpp);
        close(x->fd);
        x->fd = -1;
        return 0;
    }
    return 1;
fail:
    snprintf(err, errcap, "X display :%s: connection failed", num);
fail2:
    free(s);
    close(x->fd);
    x->fd = -1;
    return 0;
}

void x11_close(x11 *x)
{
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
    free(x->buf);
    x->buf = 0;
}

int x11_send(x11 *x, const void *req, u32 n)
{
    x->seq++;
    return wr(x->fd, req, n);
}

static void queue_event(x11 *x, const u8 *e)
{
    if ((x->evt + 1) % X11_EVQ == x->evh) return;  /* full: drop (only a window's events are queued) */
    memcpy(x->evq[x->evt], e, 32);
    x->evt = (x->evt + 1) % X11_EVQ;
}

/* the reply to the last request sent; events met on the way are queued, errors of earlier
 * requests are skipped; its own error -> 0 */
static u8 *reply(x11 *x, u32 *extra)
{
    u8 h[32];
    for (;;) {
        u32 n;
        if (!rd(x->fd, h, 32)) return 0;
        if (h[0] == 0) {                            /* an error */
            if (g16(h + 2) == (x->seq & 0xffff)) return 0;
            continue;
        }
        if (h[0] != 1) {                            /* an event */
            if ((h[0] & 0x7f) == 35) {              /* GenericEvent: has more bytes */
                u32 more = g32(h + 4) * 4;
                u8 junk[256];
                while (more) { u32 k = more > sizeof junk ? (u32)sizeof junk : more; if (!rd(x->fd, junk, k)) return 0; more -= k; }
            } else queue_event(x, h);
            continue;
        }
        n = g32(h + 4) * 4;
        if (n + 32 > x->bufcap) {
            free(x->buf);
            x->bufcap = n + 32;
            x->buf = malloc(x->bufcap);
            if (!x->buf) { x->bufcap = 0; return 0; }
        }
        memcpy(x->buf, h, 32);
        if (n && !rd(x->fd, x->buf + 32, n)) return 0;
        if (g16(h + 2) != (x->seq & 0xffff)) continue;     /* a reply nobody waits for */
        if (extra) *extra = n;
        return x->buf;
    }
}

u8 *x11_reply(x11 *x, u32 *extra) { return reply(x, extra); }

int x11_get_image(x11 *x, u8 *bgrx)
{
    u8 req[20], *r;
    u32 n, y, rowbytes;
    req[0] = 73; req[1] = 2;                        /* GetImage, ZPixmap */
    p16(req + 2, 5);
    p32(req + 4, x->root);
    p16(req + 8, 0); p16(req + 10, 0);
    p16(req + 12, x->w); p16(req + 14, x->h);
    p32(req + 16, 0xffffffffu);
    if (!x11_send(x, req, 20) || !(r = reply(x, &n))) return 0;
    rowbytes = ((x->w * x->bpp + x->pad - 1) / x->pad) * x->pad / 8;
    if (n < rowbytes * x->h) return 0;
    for (y = 0; y < x->h; y++) {
        const u8 *s = r + 32 + y * rowbytes;
        u8 *d = bgrx + y * x->w * 4;
        u32 i;
        if (x->bpp == 32 && x->lsb && x->rmask == 0xff0000 && x->gmask == 0xff00 && x->bmask == 0xff)
            memcpy(d, s, x->w * 4);                 /* the usual case: already B,G,R,X */
        else for (i = 0; i < x->w; i++) {
            u32 v = x->bpp == 32 ? (x->lsb ? g32(s + i * 4) : (u32)(s[i * 4] << 24 | s[i * 4 + 1] << 16 | s[i * 4 + 2] << 8 | s[i * 4 + 3]))
                                 : (x->lsb ? g16(s + i * 2) : (u32)(s[i * 2] << 8 | s[i * 2 + 1]));
            u32 c[3], m[3] = {x->rmask, x->gmask, x->bmask}, k;
            for (k = 0; k < 3; k++) {
                u32 mm = m[k], sh = 0, bits = 0, val;
                while (mm && !(mm & 1)) { mm >>= 1; sh++; }
                while (mm & 1) { mm >>= 1; bits++; }
                val = (v & m[k]) >> sh;
                c[k] = bits >= 8 ? val >> (bits - 8) : (bits ? val * 255 / ((1u << bits) - 1) : 0);
            }
            d[i * 4] = (u8)c[2]; d[i * 4 + 1] = (u8)c[1]; d[i * 4 + 2] = (u8)c[0]; d[i * 4 + 3] = 0;
        }
    }
    return 1;
}

static u32 atom_req(x11 *x, const char *name, int only_if_exists)
{
    u8 req[64], *r;
    u32 n = (u32)strlen(name), len = 2 + (n + 3) / 4;
    if (n > 40) return 0;
    memset(req, 0, sizeof req);
    req[0] = 16; req[1] = (u8)only_if_exists;       /* InternAtom */
    p16(req + 2, len);
    p16(req + 4, n);
    memcpy(req + 8, name, n);
    if (!x11_send(x, req, len * 4) || !(r = reply(x, 0))) return 0;
    return g32(r + 8);
}

static u32 atom(x11 *x, const char *name) { return atom_req(x, name, 1); }
u32 x11_atom(x11 *x, const char *name) { return atom_req(x, name, 0); }

/* property value of a window (first 'max' bytes); returns length or -1 */
static int prop(x11 *x, u32 win, u32 property, char *out, u32 max, u32 *type)
{
    u8 req[24], *r;
    u32 n, len;
    req[0] = 20; req[1] = 0;
    p16(req + 2, 6);
    p32(req + 4, win);
    p32(req + 8, property);
    p32(req + 12, 0);                               /* AnyPropertyType */
    p32(req + 16, 0);
    p32(req + 20, (max + 3) / 4);
    if (!x11_send(x, req, 24) || !(r = reply(x, &n))) return -1;
    if (type) *type = g32(r + 8);
    len = g32(r + 16) * (r[1] / 8);
    if (len > n) len = n;
    if (len > max) len = max;
    memcpy(out, r + 32, len);
    return (int)len;
}

int x11_prop(x11 *x, u32 win, u32 property, void *out, u32 max, u32 *type) { return prop(x, win, property, out, max, type); }

int x11_active_title(x11 *x, char *out, u32 cap)
{
    u32 win, t;
    int n;
    char buf[8];
    if (!x->a_active) {
        x->a_active = atom(x, "_NET_ACTIVE_WINDOW");
        x->a_name = atom(x, "_NET_WM_NAME");
        x->a_wmname = 39;                           /* WM_NAME, predefined */
        if (!x->a_active) return -1;
    }
    n = prop(x, x->root, x->a_active, buf, 4, &t);
    if (n < 4) return -1;
    win = g32((u8 *)buf);
    if (!win) { out[0] = 0; return 0; }
    n = x->a_name ? prop(x, win, x->a_name, out, cap - 1, &t) : -1;
    if (n <= 0) n = prop(x, win, x->a_wmname, out, cap - 1, &t);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

int x11_window_rect(x11 *x, u32 win, int *rx, int *ry, int *rw, int *rh)
{
    u8 req[16], *r;
    req[0] = 14; req[1] = 0;                        /* GetGeometry */
    p16(req + 2, 2);
    p32(req + 4, win);
    if (!x11_send(x, req, 8) || !(r = reply(x, 0))) return 0;
    *rw = g16(r + 16) + 2 * g16(r + 20);
    *rh = g16(r + 18) + 2 * g16(r + 20);
    req[0] = 40; req[1] = 0;                        /* TranslateCoordinates to the root */
    p16(req + 2, 4);
    p32(req + 4, win);
    p32(req + 8, x->root);
    p16(req + 12, 0); p16(req + 14, 0);
    if (!x11_send(x, req, 16) || !(r = reply(x, 0))) return 0;
    *rx = (short)g16(r + 12);
    *ry = (short)g16(r + 14);
    return 1;
}

u32 x11_new_id(x11 *x)
{
    u32 m = x->rid_mask, sh = 0;
    if (!m) m = 0x1fffff;
    while (!(m & 1)) { m >>= 1; sh++; }
    x->id_next++;
    return x->rid | ((x->id_next & m) << sh);
}

/* the next event (32 bytes): queued first, then from the socket, waiting up to timeout_ms
 * (0: do not wait); 0 when there is none */
int x11_next_event(x11 *x, u8 *ev, int timeout_ms)
{
    for (;;) {
        struct pollfd p;
        if (x->evh != x->evt) {
            memcpy(ev, x->evq[x->evh], 32);
            x->evh = (x->evh + 1) % X11_EVQ;
            return 1;
        }
        p.fd = x->fd; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, timeout_ms) <= 0) return 0;
        if (!rd(x->fd, ev, 32)) return -1;
        if (ev[0] == 0 || ev[0] == 1) {             /* an error or a stray reply: skip */
            if (ev[0] == 1) {
                u32 more = g32(ev + 4) * 4;
                u8 junk[256];
                while (more) { u32 k = more > sizeof junk ? (u32)sizeof junk : more; if (!rd(x->fd, junk, k)) return -1; more -= k; }
            }
            timeout_ms = 0;
            continue;
        }
        if ((ev[0] & 0x7f) == 35) {
            u32 more = g32(ev + 4) * 4;
            u8 junk[256];
            while (more) { u32 k = more > sizeof junk ? (u32)sizeof junk : more; if (!rd(x->fd, junk, k)) return -1; more -= k; }
            continue;
        }
        return 1;
    }
}

/* the window under a click: grab the pointer with a crosshair until the button is pressed */
static u32 pick_click(x11 *x, int timeout_ms)
{
    u8 req[32], ev[32], *r;
    u32 font = x11_new_id(x), cur = x11_new_id(x), child = 0;
    u32 t0 = 0;
    struct timespec ts;
    const char *fn = "cursor";
    memset(req, 0, sizeof req);
    req[0] = 45; p16(req + 2, 3 + 2); p32(req + 4, font); p16(req + 8, 6);   /* OpenFont "cursor" */
    memcpy(req + 12, fn, 6);
    x11_send(x, req, 20);
    memset(req, 0, sizeof req);
    req[0] = 94; p16(req + 2, 8); p32(req + 4, cur); p32(req + 8, font); p32(req + 12, font);
    p16(req + 16, 34); p16(req + 18, 35);           /* CreateGlyphCursor: crosshair */
    p16(req + 20, 0); p16(req + 22, 0); p16(req + 24, 0); p16(req + 26, 0xffff); p16(req + 28, 0xffff); p16(req + 30, 0xffff);
    x11_send(x, req, 32);
    memset(req, 0, sizeof req);
    req[0] = 26; req[1] = 0; p16(req + 2, 6); p32(req + 4, x->root);
    p16(req + 8, 1 << 2);                           /* ButtonPress */
    req[10] = 1; req[11] = 1;                       /* asynchronous */
    p32(req + 12, 0); p32(req + 16, cur); p32(req + 20, 0);
    if (!x11_send(x, req, 24) || !(r = reply(x, 0)) || r[1] != 0) return 0;   /* GrabPointer refused */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    for (;;) {
        u32 now;
        int k = x11_next_event(x, ev, 100);
        if (k < 0) break;
        if (k > 0 && (ev[0] & 0x7f) == 4) { child = g32(ev + 16); if (!child) child = g32(ev + 12); break; }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if ((int)(now - t0) > timeout_ms) break;
    }
    memset(req, 0, sizeof req);
    req[0] = 27; p16(req + 2, 2);                   /* UngrabPointer */
    x11_send(x, req, 8);
    memset(req, 0, sizeof req);
    req[0] = 95; p16(req + 2, 2); p32(req + 4, cur);  /* FreeCursor */
    x11_send(x, req, 8);
    memset(req, 0, sizeof req);
    req[0] = 46; p16(req + 2, 2); p32(req + 4, font); /* CloseFont */
    x11_send(x, req, 8);
    return child;
}

/* _NET_WM_PID of win or of a window inside it (window managers put programs in frames) */
static int pid_of(x11 *x, u32 win, u32 a_pid, int depth)
{
    u8 req[8], *r;
    u32 n, i, cnt, type = 0;
    u8 v[4];
    u32 kids[64];
    if (prop(x, win, a_pid, (char *)v, 4, &type) == 4) return (int)g32(v);
    if (depth > 3) return 0;
    req[0] = 15; req[1] = 0; p16(req + 2, 2); p32(req + 4, win);   /* QueryTree */
    if (!x11_send(x, req, 8) || !(r = reply(x, &n))) return 0;
    cnt = g16(r + 16);
    if (cnt > 64) cnt = 64;
    for (i = 0; i < cnt; i++) kids[i] = g32(r + 32 + i * 4);
    for (i = cnt; i-- > 0;) {                       /* top-most first */
        int pid = pid_of(x, kids[i], a_pid, depth + 1);
        if (pid) return pid;
    }
    return 0;
}

int x11_pick_pid(x11 *x, int timeout_ms, char *title, u32 tcap)
{
    u32 a_pid = x11_atom(x, "_NET_WM_PID"), w = pick_click(x, timeout_ms), t;
    int pid;
    if (title && tcap) title[0] = 0;
    if (!w) return 0;
    pid = pid_of(x, w, a_pid, 0);
    if (pid && title && tcap) {
        u32 an = atom(x, "_NET_WM_NAME");
        int n = an ? prop(x, w, an, title, tcap - 1, &t) : -1;
        if (n <= 0) n = prop(x, w, 39, title, tcap - 1, &t);
        title[n > 0 ? n : 0] = 0;
    }
    return pid;
}

/* the programs' windows the window manager lists: pid and title of each (up to max) */
int x11_client_windows(x11 *x, int *pids, char (*titles)[160], int max)
{
    u32 list = atom(x, "_NET_CLIENT_LIST"), a_pid = atom(x, "_NET_WM_PID"), an = atom(x, "_NET_WM_NAME"), t;
    u8 *w;
    int n, i, k = 0;
    if (!list || !a_pid) return 0;
    w = malloc(4096);
    if (!w) return 0;
    n = prop(x, x->root, list, (char *)w, 4096, &t);
    for (i = 0; i + 4 <= n && k < max; i += 4) {
        u32 win = g32(w + i);
        u8 v[4];
        int tl;
        if (prop(x, win, a_pid, (char *)v, 4, &t) != 4) continue;
        pids[k] = (int)g32(v);
        tl = an ? prop(x, win, an, titles[k], 159, &t) : -1;
        if (tl <= 0) tl = prop(x, win, 39, titles[k], 159, &t);
        titles[k][tl > 0 ? tl : 0] = 0;
        k++;
    }
    free(w);
    return k;
}
