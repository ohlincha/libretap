/* gui_x11.c - Tick's window on Linux, like Tick for Windows: clock, Start/Stop, Mark (+ note),
 * Sync, status; "Start a program..." (applications menu, programs running now, Browse, Pick
 * window); Settings.  Spoken directly in the X11 protocol (x11.c) with the server's core fonts,
 * so tick stays one static file with no libraries.  Drawn into a pixmap, copied to the window. */
#define _GNU_SOURCE
#include "tickx.h"
#include "x11.h"
#include "desktop.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define W 460
#define H 360

static x11 X;
static int open_ok, closed, dirty = 1;
static u32 win, pix, gc, font, bold, a_delete, a_protocols;
/* drag and drop from a file manager (XDND, version 5) */
static u32 a_xaware, a_xenter, a_xpos, a_xstatus, a_xleave, a_xdrop, a_xfinished, a_xsel, a_xcopy, a_xtypes, a_urilist, a_tickdrop;
static u32 drop_src, drop_wait;                         /* the window dragging over us; waiting for its data */
static int drop_ok;                                     /* it offers text/uri-list */
static int fw = 6, fa = 10, fh = 13, bw, ba;            /* character width, ascent, line height */
static u32 *keymap;
static int per_kc;
static char last_clock[32];

enum { P_MAIN, P_PROGS, P_SETTINGS, P_BROWSE };
static int page;
static int asking_close;                                /* "Stop recording and close Tick?" */
static int focus;                                       /* 1 note, 2 sessions folder */
static char note[200];

/* program list */
static lprog *pl;
static int pn, psel, ptop;
/* browse */
static char bdir[TICK_PATH];
static char (*bnames)[256];
static int *bisdir, bn, bsel, btop;
static u32 last_click_t;
static int last_click_row = -1;
/* settings being edited */
static tick_cfg ecfg;
static int escreen;

/* buttons of the current page */
typedef struct { int x, y, w, h, id; } btn;
static btn bt[24];
static int nbt;
enum { B_START = 1, B_MARK, B_SYNC, B_PROGS, B_SETTINGS, B_YES, B_NO, B_P_START, B_P_BROWSE, B_P_PICK, B_BACK,
       B_B_OPEN, B_S_SAVE, B_S_SCREEN, B_S_SERIAL, B_S_NET, B_S_USB, B_S_TITLES, B_S_MINUS, B_S_PLUS, B_S_DIR, B_NOTE };

/* ---------------- protocol helpers ---------------- */

static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 g32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static u32 pixel(u32 rgb)
{
    u32 c[3] = {rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255}, m[3] = {X.rmask, X.gmask, X.bmask}, v = 0, k;
    for (k = 0; k < 3; k++) {
        u32 mm = m[k], sh = 0, bits = 0;
        while (mm && !(mm & 1)) { mm >>= 1; sh++; }
        while (mm & 1) { mm >>= 1; bits++; }
        v |= (bits >= 8 ? c[k] << (bits - 8) : c[k] >> (8 - bits)) << sh;
    }
    return v;
}

static void set_fg(u32 rgb, u32 f)
{
    u8 r[20];
    memset(r, 0, sizeof r);
    r[0] = 56; p16(r + 2, f ? 5 : 4); p32(r + 4, gc);   /* ChangeGC */
    p32(r + 8, f ? (1u << 2 | 1u << 14) : 1u << 2);
    p32(r + 12, pixel(rgb));
    if (f) p32(r + 16, f);
    x11_send(&X, r, f ? 20 : 16);
}

static void fill(int x, int y, int w, int h, u32 rgb)
{
    u8 r[20];
    if (w <= 0 || h <= 0) return;
    set_fg(rgb, 0);
    r[0] = 70; r[1] = 0; p16(r + 2, 5); p32(r + 4, pix); p32(r + 8, gc);   /* PolyFillRectangle */
    p16(r + 12, (u32)x); p16(r + 14, (u32)y); p16(r + 16, (u32)w); p16(r + 18, (u32)h);
    x11_send(&X, r, 20);
}

static void frame(int x, int y, int w, int h, u32 rgb)
{
    fill(x, y, w, 1, rgb); fill(x, y + h - 1, w, 1, rgb); fill(x, y, 1, h, rgb); fill(x + w - 1, y, 1, h, rgb);
}

/* UTF-8 -> Latin-1 for the core fonts */
static int latin1(const char *s, char *out, int cap)
{
    const u8 *p = (const u8 *)s;
    int n = 0;
    while (*p && n < cap - 1) {
        u32 c = *p++;
        if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80) { c = (c & 0x1f) << 6 | (*p++ & 0x3f); }
        else if (c >= 0xe0) { while ((*p & 0xc0) == 0x80) p++; c = '?'; }
        else if (c >= 0x80) c = '?';
        out[n++] = (char)(c < 256 ? c : '?');
    }
    out[n] = 0;
    return n;
}

/* text at x, baseline y; cut to max characters (keeping the end when keep_end) */
static void text(int x, int y, const char *s, u32 rgb, int is_bold, int max, int keep_end)
{
    char l[600];
    u8 r[300];
    int n = latin1(s, l, sizeof l), off = 0, len;
    if (max > 0 && n > max) {
        if (keep_end) { off = n - max + 3; l[off - 3] = l[off - 2] = l[off - 1] = '.'; off -= 3; }
        else { l[max - 3] = l[max - 2] = l[max - 1] = '.'; n = max; }
    }
    len = n - off;
    if (len <= 0) return;
    if (len > 250) len = 250;
    set_fg(rgb, is_bold ? bold : font);
    memset(r, 0, sizeof r);
    r[0] = 74; p16(r + 2, (u32)(4 + (2 + len + 3) / 4)); p32(r + 4, pix); p32(r + 8, gc);   /* PolyText8 */
    p16(r + 12, (u32)x); p16(r + 14, (u32)y);
    r[16] = (u8)len; r[17] = 0;
    memcpy(r + 18, l + off, (size_t)len);
    x11_send(&X, r, (u32)(16 + ((2 + len + 3) / 4) * 4));
}

static int cw(int is_bold) { return is_bold ? bw : fw; }

static void button(int x, int y, int w, int h, int id, const char *label, int primary, int enabled)
{
    u32 bg = !enabled ? 0xe4e4e4 : primary ? 0x4a7f7a : 0xf7f7f7, fg = !enabled ? 0xa0a0a0 : primary ? 0xffffff : 0x202020;
    int tw = (int)strlen(label) * cw(primary);
    fill(x, y, w, h, bg);
    frame(x, y, w, h, primary && enabled ? 0x3d6b67 : 0xb8b8b8);
    text(x + (w - tw) / 2, y + (h + fa) / 2 - 1, label, fg, primary, 0, 0);
    if (enabled && nbt < 24) { bt[nbt].x = x; bt[nbt].y = y; bt[nbt].w = w; bt[nbt].h = h; bt[nbt].id = id; nbt++; }
}

static void field(int x, int y, int w, const char *val, int focused, int id)
{
    int max = (w - 10) / fw;
    char shown[TICK_PATH + 2];
    fill(x, y, w, fh + 10, 0xffffff);
    frame(x, y, w, fh + 10, focused ? 0x4a7f7a : 0xb8b8b8);
    snprintf(shown, sizeof shown, "%s%s", val, focused ? "_" : "");
    text(x + 5, y + 5 + fa, shown, 0x202020, 0, max, 1);
    if (nbt < 24) { bt[nbt].x = x; bt[nbt].y = y; bt[nbt].w = w; bt[nbt].h = fh + 10; bt[nbt].id = id; nbt++; }
}

/* ---------------- pages ---------------- */

static const char *glyph[12][5] = {
    {"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"}, {"###", "  #", "###", "#  ", "###"},
    {"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
    {"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
    {"###", "# #", "###", "  #", "###"}, {"   ", " # ", "   ", " # ", "   "}, {"   ", "   ", "   ", "   ", " # "}};

static void clock_band(void)
{
    tick_tm t;
    char d[32], line[120];
    int i, r, k, c = 7, x0, rec = session_active(), fl = app_flash();
    u32 bg = fl ? 0x101010 : rec ? 0xf3d6d6 : 0xe9e9e9, ink = fl ? 0xffffff : rec ? 0x8a1f1f : 0x202020;
    int off = clock_utc_offset_min();
    static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    clock_tm(clock_now_us(), &t);
    snprintf(d, sizeof d, "%02d:%02d:%02d.%d", t.hour, t.min, t.sec, t.ms / 100);
    fill(0, 0, W, 96, bg);
    x0 = (W - (int)strlen(d) * 4 * c + c) / 2;
    for (i = 0; d[i]; i++) {
        int g = d[i] == ':' ? 10 : d[i] == '.' ? 11 : d[i] - '0';
        for (r = 0; r < 5; r++)
            for (k = 0; k < 3; k++)
                if (glyph[g][r][k] == '#') fill(x0 + i * 4 * c + k * c, 12 + r * c, c, c, ink);
    }
    if (fl) snprintf(line, sizeof line, "SYNC %d", fl);
    else snprintf(line, sizeof line, "%s%s %04d-%02d-%02d   UTC%c%02d:%02d", rec ? "REC   " : "", wd[t.wday % 7], t.year, t.mon,
                  t.day, off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
    text((W - (int)strlen(line) * fw) / 2, 64 + fa, line, ink, rec || fl, 0, 0);
}

static void wrap(int x, int y, const char *s, int max_chars, int lines, u32 rgb, int is_bold)
{
    char buf[800];
    int n = 0;
    snprintf(buf, sizeof buf, "%s", s);
    while (*s && n < lines) {
        int l = (int)strlen(s), cut = l;
        char part[300];
        if (l > max_chars) {
            cut = max_chars;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (!cut) cut = max_chars;
        }
        snprintf(part, sizeof part, "%.*s", cut, s);
        text(x, y + n * (fh + 2), part, rgb, is_bold, 0, 0);
        s += cut;
        while (*s == ' ') s++;
        n++;
    }
}

static void page_main(void)
{
    int rec = session_active(), y = 210;
    const char *q = asking_close ? (listener_running() ? "Close Tick? Recording stops; the program keeps running, without Tick listening."
                                                       : "Stop recording and close Tick?") : app_question();
    const char *m = app_message(), *ls = listener_status();
    char l[TICK_PATH + 40];
    clock_band();
    button(12, 106, 200, 44, B_START, rec ? "Stop" : "Start", 1, 1);
    button(220, 106, 110, 44, B_MARK, "Mark", 0, rec);
    button(338, 106, 110, 44, B_SYNC, "Sync", 0, rec);
    text(12, 166 + fa - 10, "Note for the next mark (Enter = Mark):", 0x404040, 0, 0, 0);
    field(12, 172, 436, note, focus == 1, B_NOTE);
    if (q) {
        fill(12, y - 4, 436, 92, 0xfff6dc);
        frame(12, y - 4, 436, 92, 0xd8c48a);
        {
            const char *nl = strchr(q, '\n');
            char first[700];
            snprintf(first, sizeof first, "%.*s", nl ? (int)(nl - q) : (int)strlen(q), q);
            wrap(20, y + fa, first, 420 / fw, 2, 0x202020, 0);
            if (nl) text(20, y + 2 * (fh + 2) + fa, nl + 1, 0x606060, 0, 420 / fw, 0);   /* the command it closes */
        }
        button(20, y + 52, 90, 28, B_YES, asking_close ? "Close" : "Yes", 1, 1);
        button(118, y + 52, 90, 28, B_NO, asking_close ? "Keep" : "No", 0, 1);
    } else {
        if (m[0]) wrap(12, y + fa, m, 436 / fw, 2, 0x1d3f8f, 0);
        else if (rec) {
            u32 sec = app_rec_seconds(), kb = session_bytes_kb();
            snprintf(l, sizeof l, "Recording %02u:%02u:%02u - %ld screenshots, %u %s, %ld marks", sec / 3600, sec / 60 % 60,
                     sec % 60, g_frames_stored, kb < 1024 ? kb : kb / 1024, kb < 1024 ? "kB" : "MB", g_mark_n);
            text(12, y + fa, l, session_write_failed() ? 0xb02020 : 0x202020, 0, 436 / fw, 0);
        } else text(12, y + fa, "Not recording.", 0x202020, 0, 0, 0);
        if (!m[0] || strlen(m) <= (size_t)(436 / fw)) {
            snprintf(l, sizeof l, "%s%s", rec ? "Folder: " : session_dir()[0] ? "Last session: " : "", session_dir());
            text(12, y + fh + 4 + fa, l, 0x505050, 0, 436 / fw, 1);
        }
        if (*ls) {
            const char *nl = strchr(ls, '\n');
            snprintf(l, sizeof l, "%.*s", nl ? (int)(nl - ls) : (int)strlen(ls), ls);
            text(12, y + 2 * (fh + 4) + fa, l, 0x202020, 0, 436 / fw, 0);
            if (nl) text(12, y + 3 * (fh + 4) + fa, nl + 1, 0x202020, 0, 436 / fw, 0);
        }
    }
    button(12, 316, 170, 30, B_PROGS, "Start a program...", 0, !listener_running());
    button(348, 316, 100, 30, B_SETTINGS, "Settings", 0, 1);
}

static void list_box(int top, int rows, int n, int sel, int first, const char *(*label)(int))
{
    int k;
    fill(12, top, 436, rows * (fh + 5) + 4, 0xffffff);
    frame(12, top, 436, rows * (fh + 5) + 4, 0xb8b8b8);
    for (k = first; k < n && k < first + rows; k++) {
        int y = top + 2 + (k - first) * (fh + 5);
        if (k == sel) fill(13, y, 434, fh + 5, 0xcde3e0);
        text(18, y + 2 + fa, label(k), 0x202020, 0, 424 / fw, 0);
    }
    if (n > rows) {                                     /* where in the list */
        int bar = rows * (fh + 5) * rows / n, pos = rows * (fh + 5) * first / n;
        fill(443, top + 2 + pos, 3, bar < 8 ? 8 : bar, 0x9a9a9a);
    }
}

static const char *prog_label(int k) { return pl[k].label; }

static int list_rows(void) { int r = (276 - 36) / (fh + 5); return r > 12 ? 12 : r; }

static void page_programs(void)
{
    int rows = list_rows();
    fill(0, 0, W, H, 0xf0f0f0);
    text(12, 10 + ba, "Which program talks to the instrument?", 0x202020, 1, 0, 0);
    if (psel < ptop) ptop = psel;
    if (psel >= ptop + rows) ptop = psel - rows + 1;
    list_box(32, rows, pn, psel, ptop, prog_label);
    if (!pn) text(20, 52, "No programs found: use Browse or Pick window.", 0x606060, 0, 0, 0);
    if (pn) text(12, 286 + fa - 4, pl[psel].detail, 0x505050, 0, 436 / fw, 0);
    button(12, 316, 80, 30, B_P_START, "Start", 1, pn > 0);
    button(100, 316, 90, 30, B_P_BROWSE, "Browse...", 0, 1);
    button(198, 316, 110, 30, B_P_PICK, "Pick window", 0, 1);
    button(368, 316, 80, 30, B_BACK, "Back", 0, 1);
}

static const char *browse_label(int k)
{
    static char l[300];
    snprintf(l, sizeof l, "%s%s", bnames[k], bisdir[k] ? "/" : "");
    return l;
}

static void page_browse(void)
{
    int rows = list_rows();
    fill(0, 0, W, H, 0xf0f0f0);
    text(12, 10 + ba, bdir, 0x202020, 1, 436 / bw, 1);
    if (bsel < btop) btop = bsel;
    if (bsel >= btop + rows) btop = bsel - rows + 1;
    list_box(32, rows, bn, bsel, btop, browse_label);
    text(12, 286 + fa - 4, "Programs, .desktop files and folders.", 0x505050, 0, 0, 0);
    button(12, 316, 80, 30, B_B_OPEN, "Open", 1, bn > 0);
    button(368, 316, 80, 30, B_BACK, "Back", 0, 1);
}

static void check(int y, int id, const char *label, int on)
{
    fill(12, y, 16, 16, 0xffffff);
    frame(12, y, 16, 16, 0x808080);
    if (on) fill(16, y + 4, 8, 8, 0x4a7f7a);
    text(36, y + 2 + fa, label, 0x202020, 0, 0, 0);
    if (nbt < 24) { bt[nbt].x = 12; bt[nbt].y = y; bt[nbt].w = 300; bt[nbt].h = 16; bt[nbt].id = id; nbt++; }
}

static void page_settings(void)
{
    char l[TICK_PATH + 40];
    fill(0, 0, W, H, 0xf0f0f0);
    text(12, 10 + ba, "Settings", 0x202020, 1, 0, 0);
    check(36, B_S_SCREEN, "Record the screen", escreen);
    text(12, 66 + fa, "Record what the program uses:", 0x404040, 0, 0, 0);
    check(84, B_S_SERIAL, "Serial ports", ecfg.rec_serial);
    check(106, B_S_NET, "Network", ecfg.rec_net);
    check(128, B_S_USB, "USB", ecfg.rec_usb);
    check(156, B_S_TITLES, "Record the active window's title", ecfg.titles);
    snprintf(l, sizeof l, "Screenshot every %u ms", ecfg.interval_ms);
    text(12, 190 + fa, l, 0x202020, 0, 0, 0);
    button(260, 184, 28, 24, B_S_MINUS, "-", 0, ecfg.interval_ms > 100);
    button(294, 184, 28, 24, B_S_PLUS, "+", 0, ecfg.interval_ms < 600000);
    text(12, 222 + fa, "Sessions folder (empty: TickSessions next to tick):", 0x404040, 0, 0, 0);
    field(12, 240, 436, ecfg.dir, focus == 2, B_S_DIR);
    snprintf(l, sizeof l, "Saved in %s", app_ini_path());
    text(12, 280 + fa, l, 0x707070, 0, 436 / fw, 1);
    button(12, 316, 80, 30, B_S_SAVE, "Save", 1, 1);
    button(368, 316, 80, 30, B_BACK, "Cancel", 0, 1);
}

void gui_draw(void)
{
    u8 r[28];
    char now[32];
    tick_tm t;
    if (!open_ok) return;
    clock_tm(clock_now_us(), &t);
    snprintf(now, sizeof now, "%d%d%d", t.sec, t.ms / 100, app_flash());
    if (!dirty && !strcmp(now, last_clock)) return;
    copy_str(last_clock, now, sizeof last_clock);
    dirty = 0;
    nbt = 0;
    if (page == P_MAIN) { fill(0, 0, W, H, 0xf0f0f0); page_main(); }
    else if (page == P_PROGS) page_programs();
    else if (page == P_BROWSE) page_browse();
    else page_settings();
    r[0] = 62; r[1] = 0; p16(r + 2, 7); p32(r + 4, pix); p32(r + 8, win); p32(r + 12, gc);   /* CopyArea */
    p16(r + 16, 0); p16(r + 18, 0); p16(r + 20, 0); p16(r + 22, 0); p16(r + 24, W); p16(r + 26, H);
    x11_send(&X, r, 28);
}

/* ---------------- actions ---------------- */

static void open_programs(void)
{
    char titles[64][160];
    int pids[64], nw = x11_client_windows(&X, pids, titles, 64);
    if (pl) lprogs_free(pl, pn);
    pn = lprogs_list(&pl, pids, titles, nw);
    psel = ptop = 0;
    page = P_PROGS;
}

static int is_runnable(const char *dir, const char *name, int *isdir)
{
    char p[TICK_PATH * 2];
    struct stat st;
    size_t l = strlen(name);
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (stat(p, &st) < 0) return 0;
    *isdir = S_ISDIR(st.st_mode);
    if (*isdir) return 1;
    if (l > 8 && !strcmp(name + l - 8, ".desktop")) return 1;
    return S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

static int by_dir_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void browse_to(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    char (*tmp)[256];
    int *tdir, n = 0, i, k = 0;
    char real[TICK_PATH];
    if (!realpath(dir, real)) copy_str(real, dir, sizeof real);
    dp = opendir(real);
    if (!dp) { app_show("That folder cannot be opened."); return; }
    copy_str(bdir, real, sizeof bdir);
    free(bnames); free(bisdir);
    tmp = calloc(2000, sizeof *tmp);
    tdir = calloc(2000, sizeof *tdir);
    bnames = calloc(2001, sizeof *bnames);
    bisdir = calloc(2001, sizeof *bisdir);
    if (!tmp || !tdir || !bnames || !bisdir) { closedir(dp); free(tmp); free(tdir); bn = 0; return; }
    while ((de = readdir(dp)) && n < 2000) {
        int d = 0;
        if (de->d_name[0] == '.') continue;
        if (!is_runnable(real, de->d_name, &d)) continue;
        snprintf(tmp[n], 256, "%s%s", d ? "\x01" : "\x02", de->d_name);   /* folders first */
        n++;
    }
    closedir(dp);
    qsort(tmp, (size_t)n, sizeof *tmp, by_dir_name);
    if (strcmp(real, "/")) { copy_str(bnames[k], "..", 256); bisdir[k++] = 1; }
    for (i = 0; i < n; i++) { copy_str(bnames[k], tmp[i] + 1, 256); bisdir[k++] = tmp[i][0] == 1; }
    free(tmp); free(tdir);
    bn = k;
    bsel = btop = 0;
    page = P_BROWSE;
}

static void browse_open(void)
{
    char p[TICK_PATH * 2];
    if (!bn) return;
    if (!strcmp(bnames[bsel], "..")) {
        char *s;
        copy_str(p, bdir, sizeof p);
        s = strrchr(p, '/');
        if (s && s != p) *s = 0; else copy_str(p, "/", sizeof p);
        browse_to(p);
        return;
    }
    snprintf(p, sizeof p, "%s%s%s", bdir, strcmp(bdir, "/") ? "/" : "", bnames[bsel]);
    if (bisdir[bsel]) { browse_to(p); return; }
    page = P_MAIN;
    app_open(p);
}

static void choose_selected(void)
{
    lprog c;
    if (!pn) return;
    c = lprog_copy(&pl[psel]);
    page = P_MAIN;
    app_choose(&c);
    lprog_free_strings(c.argv);
    lprog_free_strings(c.envp);
}

static void click(int id)
{
    switch (id) {
    case B_START: if (session_active()) app_stop(); else app_start(); break;
    case B_MARK: app_mark(note); note[0] = 0; break;
    case B_SYNC: app_sync(); break;
    case B_NOTE: focus = 1; break;
    case B_PROGS:
        if (listener_running()) app_show("Tick is already listening to a program.");
        else {
            fill(12, 316, 170, 30, 0xf0f0f0);
            text(12, 336, "Reading the program list...", 0x505050, 0, 0, 0);
            dirty = 0;
            {
                u8 r[28];
                r[0] = 62; r[1] = 0; p16(r + 2, 7); p32(r + 4, pix); p32(r + 8, win); p32(r + 12, gc);
                p16(r + 16, 0); p16(r + 18, 0); p16(r + 20, 0); p16(r + 22, 0); p16(r + 24, W); p16(r + 26, H);
                x11_send(&X, r, 28);
            }
            open_programs();
        }
        break;
    case B_SETTINGS: ecfg = g_cfg; escreen = g_screen_on; focus = 0; page = P_SETTINGS; break;
    case B_YES: if (asking_close) closed = 1; else app_answer(1); break;
    case B_NO: if (asking_close) asking_close = 0; else app_answer(0); break;
    case B_P_START: choose_selected(); break;
    case B_P_BROWSE: {
        const char *h = getenv("HOME");
        browse_to(access("/opt", R_OK) == 0 ? "/opt" : h ? h : "/");
        break;
    }
    case B_P_PICK:
        page = P_MAIN;
        app_show("Click the program's window (20 s).");
        dirty = 1;
        gui_draw();
        app_pick();
        break;
    case B_BACK: page = page == P_BROWSE ? P_PROGS : P_MAIN; focus = page == P_MAIN ? 1 : 0; break;
    case B_B_OPEN: browse_open(); break;
    case B_S_SCREEN: escreen = !escreen; break;
    case B_S_SERIAL: ecfg.rec_serial = !ecfg.rec_serial; break;
    case B_S_NET: ecfg.rec_net = !ecfg.rec_net; break;
    case B_S_USB: ecfg.rec_usb = !ecfg.rec_usb; break;
    case B_S_TITLES: ecfg.titles = !ecfg.titles; break;
    case B_S_MINUS: if (ecfg.interval_ms > 100) ecfg.interval_ms -= ecfg.interval_ms <= 1000 ? 100 : 500; break;
    case B_S_PLUS: if (ecfg.interval_ms < 600000) ecfg.interval_ms += ecfg.interval_ms < 1000 ? 100 : 500; break;
    case B_S_DIR: focus = 2; break;
    case B_S_SAVE:
        g_cfg = ecfg;
        g_screen_on = escreen;
        app_show(cfg_save() ? "Settings saved. Screenshot settings apply from the next recording." : "Settings could not be saved.");
        page = P_MAIN;
        focus = 1;
        break;
    }
    dirty = 1;
}

static void list_click(int y, u32 t)
{
    int row = (y - 34) / (fh + 5);
    int *sel = page == P_PROGS ? &psel : &bsel, top = page == P_PROGS ? ptop : btop, n = page == P_PROGS ? pn : bn;
    if (row < 0 || row >= list_rows() || top + row >= n) return;
    *sel = top + row;
    if (last_click_row == *sel && t - last_click_t < 450) {          /* double click */
        if (page == P_PROGS) choose_selected(); else browse_open();
        last_click_row = -1;
    } else { last_click_row = *sel; last_click_t = t; }
    dirty = 1;
}

static void scroll(int d)
{
    int *sel = page == P_PROGS ? &psel : &bsel, n = page == P_PROGS ? pn : bn;
    *sel += d;
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    dirty = 1;
}

static void key(u32 sym)
{
    char *buf = focus == 1 ? note : focus == 2 ? ecfg.dir : 0;
    size_t cap = focus == 1 ? sizeof note : sizeof ecfg.dir;
    dirty = 1;
    if (page == P_PROGS || page == P_BROWSE) {
        if (sym == 0xff52) scroll(-1);                  /* Up */
        else if (sym == 0xff54) scroll(1);              /* Down */
        else if (sym == 0xff55) scroll(-12);            /* Page Up */
        else if (sym == 0xff56) scroll(12);
        else if (sym == 0xff0d || sym == 0xff8d) { if (page == P_PROGS) choose_selected(); else browse_open(); }
        else if (sym == 0xff1b) { page = page == P_BROWSE ? P_PROGS : P_MAIN; focus = page == P_MAIN; }
        return;
    }
    if (page == P_MAIN && asking_close) {
        if (sym == 0xff0d) closed = 1;
        if (sym == 0xff1b) asking_close = 0;
        return;
    }
    if (page == P_MAIN && app_question()) {
        if (sym == 'y' || sym == 'Y' || sym == 0xff0d) { app_answer(1); return; }
        if (sym == 'n' || sym == 'N' || sym == 0xff1b) { app_answer(0); return; }
    }
    if (page == P_SETTINGS && sym == 0xff1b) { page = P_MAIN; focus = 1; return; }
    if (!buf) return;
    if (sym == 0xff0d || sym == 0xff8d) {               /* Enter */
        if (focus == 1 && session_active()) { app_mark(note); note[0] = 0; }
        else if (focus == 2) focus = 0;
    } else if (sym == 0xff08) {                          /* BackSpace */
        size_t n = strlen(buf);
        if (n) {
            n--;
            while (n && ((u8)buf[n] & 0xc0) == 0x80) n--;
            buf[n] = 0;
        }
    } else if (sym >= 0x20 && sym <= 0xff) {             /* Latin-1 -> UTF-8 */
        size_t n = strlen(buf);
        if (sym < 0x80 && n + 1 < cap) { buf[n] = (char)sym; buf[n + 1] = 0; }
        else if (sym >= 0xa0 && n + 2 < cap) { buf[n] = (char)(0xc0 | sym >> 6); buf[n + 1] = (char)(0x80 | (sym & 0x3f)); buf[n + 2] = 0; }
    }
}

static void xdnd_send(u32 to, u32 type, u32 l1, u32 l2, u32 l3, u32 l4)
{
    u8 r[44];
    memset(r, 0, sizeof r);
    r[0] = 25; p16(r + 2, 11); p32(r + 4, to);         /* SendEvent, no event mask */
    r[12] = 33; r[13] = 32;                              /* ClientMessage, format 32 */
    p32(r + 16, to); p32(r + 20, type);
    p32(r + 24, win); p32(r + 28, l1); p32(r + 32, l2); p32(r + 36, l3); p32(r + 40, l4);
    x11_send(&X, r, 44);
}

/* a dropped file: a menu entry (.desktop) or a program file, else a plain refusal */
static void drop_open(const char *path)
{
    struct stat st;
    size_t l = strlen(path);
    const char *b = strrchr(path, '/');
    char m[400];
    page = P_MAIN;
    dirty = 1;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
        ((l > 8 && !strcmp(path + l - 8, ".desktop")) || access(path, X_OK) == 0)) {
        app_open(path);
        return;
    }
    snprintf(m, sizeof m, "Not a program or shortcut: %s", b ? b + 1 : path);
    app_show(m);
}

static void xdnd_message(const u8 *ev)
{
    u32 type = g32(ev + 8), l0 = g32(ev + 12), l1 = g32(ev + 16), l2 = g32(ev + 20);
    if (!a_xenter) return;
    if (type == a_xenter) {
        u32 t[64], n = 0, i, ptype = 0;
        drop_src = l0;
        drop_ok = 0;
        if (l1 & 1) {                                    /* more than three types: in XdndTypeList */
            int k = x11_prop(&X, drop_src, a_xtypes, t, sizeof t, &ptype);
            n = k > 0 ? (u32)k / 4 : 0;
        } else {
            t[0] = l2; t[1] = g32(ev + 24); t[2] = g32(ev + 28);
            n = 3;
        }
        for (i = 0; i < n; i++) if (t[i] == a_urilist) drop_ok = 1;
    } else if (type == a_xpos) {
        if (l0 != drop_src) return;
        xdnd_send(drop_src, a_xstatus, drop_ok ? 1 : 0, 0, 0, drop_ok ? a_xcopy : 0);
    } else if (type == a_xleave) {
        if (l0 == drop_src) drop_src = 0;
    } else if (type == a_xdrop) {
        u8 r[24];
        if (l0 != drop_src) return;
        if (!drop_ok) { xdnd_send(drop_src, a_xfinished, 0, 0, 0, 0); drop_src = 0; return; }
        memset(r, 0, sizeof r);
        r[0] = 24; p16(r + 2, 6); p32(r + 4, win);      /* ConvertSelection: ask for the dropped files */
        p32(r + 8, a_xsel); p32(r + 12, a_urilist); p32(r + 16, a_tickdrop); p32(r + 20, l2);
        x11_send(&X, r, 24);
        drop_wait = drop_src;
    }
}

static void xdnd_data(const u8 *ev)
{
    char *buf;
    char path[TICK_PATH];
    u32 type = 0, src = drop_wait;
    int n, ok = 0;
    drop_wait = drop_src = 0;
    if (!src) return;
    buf = malloc(65536);
    if (buf && g32(ev + 20) == a_tickdrop) {             /* property set: the data arrived */
        n = x11_prop(&X, win, a_tickdrop, buf, 65535, &type);
        if (n > 0) {
            buf[n] = 0;
            ok = uri_list_first_path(buf, path, sizeof path);
        }
    }
    free(buf);
    xdnd_send(src, a_xfinished, ok ? 1 : 0, ok ? a_xcopy : 0, 0, 0);
    if (ok) drop_open(path);
    else app_show("Drop a program or its shortcut from a file manager.");
}

void gui_events(void)
{
    u8 ev[32];
    int k;
    if (!open_ok) return;
    while ((k = x11_next_event(&X, ev, 0)) > 0) {
        int type = ev[0] & 0x7f;
        if (type == 12) dirty = 1;                       /* Expose */
        else if (type == 4) {                            /* ButtonPress */
            int b = ev[1], x = (short)(ev[24] | ev[25] << 8), y = (short)(ev[26] | ev[27] << 8), i, hit = 0;
            if (b == 4 || b == 5) { if (page == P_PROGS || page == P_BROWSE) scroll(b == 4 ? -3 : 3); continue; }
            if (b != 1) continue;
            for (i = nbt - 1; i >= 0; i--)
                if (x >= bt[i].x && x < bt[i].x + bt[i].w && y >= bt[i].y && y < bt[i].y + bt[i].h) {
                    click(bt[i].id);
                    hit = 1;
                    break;
                }
            if (!hit && (page == P_PROGS || page == P_BROWSE) && x >= 12 && x < 448 && y >= 32 && y < 32 + list_rows() * (fh + 5) + 4)
                list_click(y, g32(ev + 4));
        } else if (type == 2 && keymap) {                /* KeyPress */
            int kc = ev[1], st = ev[28] | ev[29] << 8;
            u32 sym = 0;
            if (kc >= X.min_kc && kc <= X.max_kc) {
                u32 *m = keymap + (kc - X.min_kc) * per_kc;
                sym = (st & 1) && per_kc > 1 && m[1] ? m[1] : m[0];
                if ((st & 2) && sym >= 'a' && sym <= 'z') sym -= 32;
            }
            if (sym) key(sym);
        } else if (type == 34) {                         /* MappingNotify: the keyboard map changed */
            u8 r[8], *q;
            u32 n;
            memset(r, 0, sizeof r);
            r[0] = 101; p16(r + 2, 2); r[4] = X.min_kc; r[5] = (u8)(X.max_kc - X.min_kc + 1);
            x11_send(&X, r, 8);
            if ((q = x11_reply(&X, &n)) && n) {
                u32 *m = malloc(n);
                if (m) { memcpy(m, q + 32, n); free(keymap); keymap = m; per_kc = q[1]; }
            }
        } else if (type == 31) {                         /* SelectionNotify: dropped files */
            xdnd_data(ev);
        } else if (type == 33 && g32(ev + 8) != a_protocols) {
            xdnd_message(ev);                            /* drag and drop */
            dirty = 1;
        } else if (type == 33) {                         /* ClientMessage: the window's close button */
            if (g32(ev + 8) == a_protocols && g32(ev + 12) == a_delete) {
                if (session_active() || listener_running()) { asking_close = 1; page = P_MAIN; dirty = 1; }
                else closed = 1;
            }
        }
    }
    if (k < 0) closed = 1;                               /* the X server went away */
}

/* ---------------- setup ---------------- */

static u32 open_font(const char **names, int *w, int *asc, int *h)
{
    int i;
    for (i = 0; names[i]; i++) {
        u8 r[64], *q;
        u32 f = x11_new_id(&X), n = (u32)strlen(names[i]);
        memset(r, 0, sizeof r);
        r[0] = 45; p16(r + 2, 3 + (n + 3) / 4); p32(r + 4, f); p16(r + 8, n);     /* OpenFont */
        memcpy(r + 12, names[i], n);
        x11_send(&X, r, 12 + ((n + 3) / 4) * 4);
        memset(r, 0, 8);
        r[0] = 47; p16(r + 2, 2); p32(r + 4, f);                                    /* QueryFont */
        x11_send(&X, r, 8);
        if ((q = x11_reply(&X, 0))) {
            *w = q[28] | q[29] << 8;                     /* max_bounds.character_width */
            *asc = q[52] | q[53] << 8;
            *h = *asc + (q[54] | q[55] << 8);
            if (*w > 0 && *w < 30) return f;
        }
    }
    return 0;
}

static void set_prop(u32 property, u32 type, int format, const void *data, u32 n)
{
    u8 r[600];
    u32 bytes = n * (u32)format / 8;
    if (bytes > 560) return;
    memset(r, 0, sizeof r);
    r[0] = 18; r[1] = 0; p16(r + 2, 6 + (bytes + 3) / 4);                            /* ChangeProperty, replace */
    p32(r + 4, win); p32(r + 8, property); p32(r + 12, type); r[16] = (u8)format; p32(r + 20, n);
    memcpy(r + 24, data, bytes);
    x11_send(&X, r, 24 + ((bytes + 3) / 4) * 4);
}

int gui_open(char *err, u32 errcap)
{
    static const char *reg[] = {"-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1", "9x15", "8x13", "7x13", "fixed", 0};
    static const char *bld[] = {"-misc-fixed-bold-r-normal--15-*-*-*-*-*-iso8859-1", "9x15bold", "8x13bold", "7x13bold", "fixed", 0};
    u8 r[64], *q;
    u32 n;
    int dummy;
    if (!x11_open(&X, err, errcap)) return 0;
    font = open_font(reg, &fw, &fa, &fh);
    bold = open_font(bld, &bw, &ba, &dummy);
    if (!font) { snprintf(err, errcap, "The X display has no usable font"); x11_close(&X); return 0; }
    if (!bold) { bold = font; bw = fw; ba = fa; }
    win = x11_new_id(&X);
    memset(r, 0, sizeof r);
    r[0] = 1; r[1] = (u8)X.depth; p16(r + 2, 8 + 2); p32(r + 4, win); p32(r + 8, X.root);   /* CreateWindow */
    p16(r + 12, 0); p16(r + 14, 0); p16(r + 16, W); p16(r + 18, H); p16(r + 20, 0);
    p16(r + 22, 1); p32(r + 24, X.root_visual);
    p32(r + 28, 1u << 1 | 1u << 11);                     /* background pixel, event mask */
    p32(r + 32, pixel(0xf0f0f0));
    p32(r + 36, 1u << 15 | 1u << 2 | 1u << 0 | 1u << 17); /* Exposure, ButtonPress, KeyPress, StructureNotify */
    x11_send(&X, r, 40);
    pix = x11_new_id(&X);
    memset(r, 0, sizeof r);
    r[0] = 53; r[1] = (u8)X.depth; p16(r + 2, 4); p32(r + 4, pix); p32(r + 8, win); p16(r + 12, W); p16(r + 14, H);
    x11_send(&X, r, 16);
    gc = x11_new_id(&X);
    memset(r, 0, sizeof r);
    r[0] = 55; p16(r + 2, 4 + 2); p32(r + 4, gc); p32(r + 8, pix); p32(r + 12, 1u << 14 | 1u << 16);
    p32(r + 16, font); p32(r + 20, 0);                   /* font, no graphics exposures */
    x11_send(&X, r, 24);
    {
        const char *title = TICK_NAME " - LibreTap recorder";
        u32 utf8 = x11_atom(&X, "UTF8_STRING");
        u32 hints[18];
        set_prop(39, 31, 8, title, (u32)strlen(title));                              /* WM_NAME */
        set_prop(x11_atom(&X, "_NET_WM_NAME"), utf8, 8, title, (u32)strlen(title));
        set_prop(67, 31, 8, "tick\0Tick\0", 10);                                      /* WM_CLASS */
        a_protocols = x11_atom(&X, "WM_PROTOCOLS");
        a_delete = x11_atom(&X, "WM_DELETE_WINDOW");
        set_prop(a_protocols, 4, 32, &a_delete, 1);                                   /* ATOM */
        {   /* files dropped from a file manager (XDND version 5) */
            u32 version = 5;
            a_xaware = x11_atom(&X, "XdndAware");
            a_xenter = x11_atom(&X, "XdndEnter");
            a_xpos = x11_atom(&X, "XdndPosition");
            a_xstatus = x11_atom(&X, "XdndStatus");
            a_xleave = x11_atom(&X, "XdndLeave");
            a_xdrop = x11_atom(&X, "XdndDrop");
            a_xfinished = x11_atom(&X, "XdndFinished");
            a_xsel = x11_atom(&X, "XdndSelection");
            a_xcopy = x11_atom(&X, "XdndActionCopy");
            a_xtypes = x11_atom(&X, "XdndTypeList");
            a_urilist = x11_atom(&X, "text/uri-list");
            a_tickdrop = x11_atom(&X, "TICK_DROP");
            set_prop(a_xaware, 4, 32, &version, 1);
        }
        memset(hints, 0, sizeof hints);
        hints[0] = 16 | 32;                              /* PMinSize | PMaxSize: a fixed size */
        hints[5] = W; hints[6] = H; hints[7] = W; hints[8] = H;
        set_prop(40, 41, 32, hints, 18);                 /* WM_NORMAL_HINTS, WM_SIZE_HINTS */
    }
    memset(r, 0, sizeof r);
    r[0] = 101; p16(r + 2, 2); r[4] = X.min_kc; r[5] = (u8)(X.max_kc - X.min_kc + 1);   /* GetKeyboardMapping */
    x11_send(&X, r, 8);
    if ((q = x11_reply(&X, &n)) && n) {
        per_kc = q[1];
        keymap = malloc(n);
        if (keymap) memcpy(keymap, q + 32, n);
    }
    memset(r, 0, sizeof r);
    r[0] = 8; p16(r + 2, 2); p32(r + 4, win);            /* MapWindow */
    x11_send(&X, r, 8);
    open_ok = 1;
    focus = 1;
    dirty = 1;
    return 1;
}

int gui_fd(void) { return X.fd; }
u32 gui_window(void) { return win; }
int gui_closed(void) { return closed; }

void gui_close(void)
{
    if (!open_ok) return;
    x11_close(&X);
    open_ok = 0;
    free(keymap);
    keymap = 0;
}
