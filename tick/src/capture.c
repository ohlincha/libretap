/* capture.c - screenshots every interval on a low-priority thread.  Only the
 * parts of the screen that changed are stored (PNG, 32x32 tiles merged into
 * rectangles); a full picture at the start, every Nth stored screenshot and
 * when most of the screen changed.  Nothing is stored while the screen is
 * still. */
#include "tick.h"
#include "png.h"
#include "diff.h"

#define MAXRECTS 48
#define DISK_LOW  (30u << 20)
#define DISK_OK   (50u << 20)

static HANDLE th, stop_ev;

static struct {
    int vx, vy, w, h;
    HDC mdc;
    HBITMAP bm[2], oldbm;
    u8 *bits[2];
    int cur;
    u8 *tilemap;
    trect rects[MAXRECTS];
    png_enc *png;
    u32 since_full, file_n;
    int need_full, registered, blt_failed, paused;
    DWORD last_disk;
    HCURSOR hcur;
    int hot_x, hot_y;
    char title[512];
    sbuf ev;
} C;

typedef BOOL (WINAPI *GETCURSORINFO)(PCURSORINFO);
static GETCURSORINFO pGetCursorInfo;

static void free_buffers(void)
{
    int i;
    if (C.mdc) {
        if (C.oldbm) SelectObject(C.mdc, C.oldbm);
        DeleteDC(C.mdc);
    }
    for (i = 0; i < 2; i++) if (C.bm[i]) DeleteObject(C.bm[i]);
    rt_free(C.tilemap);
    png_delete(C.png);
    C.mdc = 0; C.oldbm = 0; C.bm[0] = C.bm[1] = 0; C.bits[0] = C.bits[1] = 0;
    C.tilemap = 0; C.png = 0;
}

static void screen_rect(int *x, int *y, int *w, int *h)
{
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);   /* 0 on Windows 95 and NT 4 */
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w > 0 && *h > 0) {
        *x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        *y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    } else {
        *x = *y = 0;
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

static int alloc_buffers(void)
{
    BITMAPINFO bi;
    HDC sdc;
    int i;
    u32 tile = g_cfg.tile;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = C.w;
    bi.bmiHeader.biHeight = -C.h;               /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(0);
    C.mdc = CreateCompatibleDC(sdc);
    for (i = 0; i < 2; i++) {
        void *bits = 0;
        C.bm[i] = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
        C.bits[i] = bits;
    }
    ReleaseDC(0, sdc);
    C.tilemap = rt_alloc(((C.w + tile - 1) / tile) * ((C.h + tile - 1) / tile));
    C.png = png_new((u32)C.w * (u32)C.h);
    if (!C.mdc || !C.bm[0] || !C.bm[1] || !C.bits[0] || !C.bits[1] || !C.tilemap || !C.png) {
        free_buffers();
        return 0;
    }
    C.oldbm = SelectObject(C.mdc, C.bm[0]);
    memset(C.bits[0], 0, (u32)C.w * C.h * 4);
    memset(C.bits[1], 0, (u32)C.w * C.h * 4);
    return 1;
}

static void draw_cursor(void)
{
    POINT pt;
    HCURSOR hc = 0;
    if (pGetCursorInfo) {
        CURSORINFO ci;
        memset(&ci, 0, sizeof ci);
        ci.cbSize = sizeof ci;
        if (!pGetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        hc = ci.hCursor;
        pt = ci.ptScreenPos;
    } else {
        if (!GetCursorPos(&pt)) return;
        hc = LoadCursor(0, IDC_ARROW);      /* Windows 95: shape unknown, draw an arrow */
    }
    if (hc != C.hcur) {
        ICONINFO ii;
        C.hcur = hc;
        C.hot_x = C.hot_y = 0;
        if (GetIconInfo(hc, &ii)) {
            C.hot_x = (int)ii.xHotspot;
            C.hot_y = (int)ii.yHotspot;
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    DrawIconEx(C.mdc, pt.x - C.hot_x - C.vx, pt.y - C.hot_y - C.vy, hc, 0, 0, 0, 0, DI_NORMAL);
}

/* ---- "vendor program only": keep the program's own windows, black out the rest ---- */

volatile DWORD g_pick_pid;              /* window picked by the operator (when not listening) */
static DWORD cur_pid = 0xffffffffu;
static RECT wr[64];
static int nwr;

static BOOL CALLBACK enum_cb(HWND w, LPARAM pid)
{
    DWORD p = 0;
    RECT r;
    GetWindowThreadProcessId(w, &p);
    if (p == (DWORD)pid && IsWindowVisible(w) && !IsIconic(w) && GetWindowRect(w, &r) && nwr < 64) wr[nwr++] = r;
    return TRUE;
}

static void mask_program(u8 *bits)
{
    DWORD pid = listener_pid();
    int y, i;
    if (!pid) pid = g_pick_pid;
    if (pid != cur_pid) {
        sbuf b;
        char name[MAX_PATH];
        cur_pid = pid;
        sb_init(&b, 200);
        if (pid) {
            process_name(pid, name);
            sb_str(&b, "Screenshots show only the windows of ");
            ansi_to_utf8(&b, name);
            sb_str(&b, " (process ");
            sb_u32(&b, pid);
            sb_str(&b, "); the rest is black");
            session_set_capture(name);
        } else {
            sb_str(&b, "Screenshots: vendor-program-only mode, but no program chosen: pictures are black");
            session_set_capture("");
        }
        ev_status("status", b.p);
        sb_free(&b);
    }
    nwr = 0;
    if (pid) EnumWindows(enum_cb, (LPARAM)pid);
    for (i = 0; i < nwr; i++) {             /* to bitmap coordinates, clipped */
        wr[i].left -= C.vx; wr[i].right -= C.vx; wr[i].top -= C.vy; wr[i].bottom -= C.vy;
        if (wr[i].left < 0) wr[i].left = 0;
        if (wr[i].top < 0) wr[i].top = 0;
        if (wr[i].right > C.w) wr[i].right = C.w;
        if (wr[i].bottom > C.h) wr[i].bottom = C.h;
    }
    for (y = 0; y < C.h; y++) {
        u8 *row = bits + (u32)y * C.w * 4;
        int x = 0;
        for (;;) {                          /* black from x to the next covered span */
            int next = C.w, end = x;
            for (i = 0; i < nwr; i++)
                if (y >= wr[i].top && y < wr[i].bottom && wr[i].right > x) {
                    if (wr[i].left <= x) { if (wr[i].right > end) end = wr[i].right; }
                    else if (wr[i].left < next) next = wr[i].left;
                }
            if (end > x) { x = end; continue; }     /* x is covered: skip the span */
            memset(row + x * 4, 0, (u32)(next - x) * 4);
            x = next;
            if (x >= C.w) break;
        }
    }
}

/* paint Tick's own window flat grey: its clock changes all the time */
static void mask_self(u8 *bits)
{
    RECT r;
    int x0, y0, x1, y1, y;
    if (!g_cfg.mask_self || !g_hwnd || !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) return;
    if (!GetWindowRect(g_hwnd, &r)) return;
    x0 = r.left - C.vx; y0 = r.top - C.vy; x1 = r.right - C.vx; y1 = r.bottom - C.vy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > C.w) x1 = C.w;
    if (y1 > C.h) y1 = C.h;
    if (x0 >= x1 || y0 >= y1) return;
    for (y = y0; y < y1; y++) memset(bits + ((u32)y * C.w + x0) * 4, 0xa0, (u32)(x1 - x0) * 4);
}

static int store(u64 t, const u8 *bits, int full, u32 x, u32 y, u32 w, u32 h)
{
    u32 size;
    const u8 *p = png_encode_bgrx(C.png, bits, (u32)C.w * 4, x, y, w, h, &size);
    char rel[32];
    sbuf *b = &C.ev;
    if (!p) return 0;
    C.file_n++;
    sb_reset(b);
    sb_str(b, "frames\\");
    sb_u32w(b, C.file_n, 6);
    sb_str(b, ".png");
    copy_str(rel, b->p, sizeof rel);
    if (!session_write_file(rel, p, size)) return 0;
    ev_begin(b, t, "frame");
    sb_str(b, ",\"src\":\"screen\",\"file\":\"frames/");
    sb_u32w(b, C.file_n, 6);
    sb_str(b, full ? ".png\",\"full\":true" : ".png\",\"full\":false");
    sb_str(b, ",\"x\":"); sb_u32(b, x);
    sb_str(b, ",\"y\":"); sb_u32(b, y);
    sb_str(b, ",\"w\":"); sb_u32(b, w);
    sb_str(b, ",\"h\":"); sb_u32(b, h);
    ev_end(b);
    return 1;
}

static void check_disk(void)
{
    const char *d = session_dir();
    char rootp[4];
    DWORD spc, bps, fc, tc;
    u64 freeb;
    if (GetTickCount() - C.last_disk < 10000 && C.last_disk) return;
    C.last_disk = GetTickCount();
    if (!(d[0] && d[1] == ':' && d[2] == '\\')) { if (C.paused == 2) C.paused = 0; return; }
    rootp[0] = d[0]; rootp[1] = ':'; rootp[2] = '\\'; rootp[3] = 0;
    if (!GetDiskFreeSpaceA(rootp, &spc, &bps, &fc, &tc)) return;
    freeb = (u64)spc * bps * fc;
    if (!C.paused && freeb < DISK_LOW) {
        C.paused = 1;
        ev_status("error", "Disk almost full: screenshots paused (events are still written)");
        PostMessageA(g_hwnd, WM_TICK_ERROR, 0, (LPARAM)"Disk almost full: screenshots paused");
    } else if (C.paused == 1 && freeb > DISK_OK) {
        C.paused = 0;
        C.need_full = 1;
        ev_status("status", "Disk space available again: screenshots resumed");
    } else if (C.paused == 2) {             /* after a write error: try again */
        C.paused = 0;
        C.need_full = 1;
    }
}

static void shot(void)
{
    int x, y, w, h, full;
    u64 t;
    HDC sdc;
    u8 *cur, *prev;
    u32 n = 0, area = 0, i;
    BOOL ok;

    screen_rect(&x, &y, &w, &h);
    if (w != C.w || h != C.h || x != C.vx || y != C.vy || !C.mdc) {
        free_buffers();
        C.vx = x; C.vy = y; C.w = w; C.h = h;
        if (w <= 0 || h <= 0 || !alloc_buffers()) {
            C.w = 0;
            ev_status("error", "Not enough memory for screenshots");
            return;
        }
        C.need_full = 1;
        sb_reset(&C.ev);
        sb_str(&C.ev, "Screen ");
        sb_u32(&C.ev, (u32)w); sb_putc(&C.ev, 'x'); sb_u32(&C.ev, (u32)h);
        if (!C.registered) {
            char json[160];
            sbuf j;
            sb_init(&j, 160);
            sb_str(&j, "{\"id\": \"screen\", \"kind\": \"screen\", \"label\": \"");
            sb_str(&j, C.ev.p);
            sb_str(&j, "\", \"width\": "); sb_u32(&j, (u32)w);
            sb_str(&j, ", \"height\": "); sb_u32(&j, (u32)h);
            sb_putc(&j, '}');
            copy_str(json, j.p, sizeof json);
            sb_free(&j);
            session_add_source("screen", json);
            C.registered = 1;
        } else {
            char text[64];
            copy_str(text, C.ev.p, sizeof text);
            ev_status("status", text);
        }
    }
    check_disk();
    if (C.paused) return;

    t = clock_now_us();
    SelectObject(C.mdc, C.bm[C.cur]);
    sdc = GetDC(0);
    /* CAPTUREBLT (layered windows) only where it is known: Windows 2000 and later */
    ok = BitBlt(C.mdc, 0, 0, C.w, C.h, sdc, C.vx, C.vy,
                SRCCOPY | (!g_win9x && LOBYTE(LOWORD(g_winver)) >= 5 ? CAPTUREBLT : 0));
    ReleaseDC(0, sdc);
    if (!ok) {                              /* locked workstation, screen saver desktop... */
        if (!C.blt_failed) ev_status("status", "Screen cannot be read (locked or screen saver?)");
        C.blt_failed = 1;
        return;
    }
    if (C.blt_failed) { C.blt_failed = 0; C.need_full = 1; ev_status("status", "Screen readable again"); }
    if (g_cfg.cursor) draw_cursor();
    GdiFlush();
    cur = C.bits[C.cur];
    prev = C.bits[C.cur ^ 1];
    if (g_cfg.program_only) mask_program(cur);
    mask_self(cur);

    full = C.need_full || C.since_full + 1 >= g_cfg.full_every;
    if (!full) {
        n = diff_rects(cur, prev, (u32)C.w * 4, (u32)C.w, (u32)C.h, g_cfg.tile, C.tilemap,
                       C.rects, MAXRECTS, &area);
        if (n == 0) return;                 /* nothing changed: keep prev, store nothing */
        if ((u64)area * 2 > (u64)C.w * C.h) full = 1;
    }
    if (full) {
        if (!store(t, cur, 1, 0, 0, (u32)C.w, (u32)C.h)) goto fail;
        C.since_full = 0;
        C.need_full = 0;
    } else {
        for (i = 0; i < n; i++)
            if (!store(t, cur, 0, C.rects[i].x, C.rects[i].y, C.rects[i].w, C.rects[i].h)) goto fail;
        C.since_full++;
    }
    InterlockedIncrement((LONG *)&g_frames_stored);
    C.cur ^= 1;                             /* this picture is the new reference */
    return;
fail:
    C.cur ^= 1;
    C.need_full = 1;
    C.paused = 2;
    C.last_disk = GetTickCount();
    ev_status("error", "Could not write a screenshot (disk full or removed?); retrying in 10 s");
    PostMessageA(g_hwnd, WM_TICK_ERROR, 0, (LPARAM)"Could not write a screenshot");
}

static void window_title(void)
{
    char t[512];
    HWND fg = GetForegroundWindow();
    t[0] = 0;
    if (fg) GetWindowTextA(fg, t, sizeof t);
    if (lstrcmpA(t, C.title) == 0) return;
    copy_str(C.title, t, sizeof C.title);
    ev_begin(&C.ev, clock_now_us(), "input");
    sb_str(&C.ev, ",\"src\":\"input\",\"what\":\"window\",\"title\":");
    {
        sbuf u;
        sb_init(&u, 128);
        ansi_to_utf8(&u, t);
        sb_json(&C.ev, u.p ? u.p : "", u.len);
        sb_free(&u);
    }
    ev_end(&C.ev);
}

static DWORD WINAPI cap_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        DWORD t0 = GetTickCount(), el, wait;
        shot();
        if (g_cfg.titles) window_title();
        el = GetTickCount() - t0;
        wait = g_cfg.interval_ms > el + 10 ? g_cfg.interval_ms - el : 10;
        if (WaitForSingleObject(stop_ev, wait) == WAIT_OBJECT_0) break;
    }
    free_buffers();
    return 0;
}

int capture_start(void)
{
    DWORD id;
    HMODULE u = GetModuleHandleA("user32.dll");
    pGetCursorInfo = u ? (GETCURSORINFO)(void *)GetProcAddress(u, "GetCursorInfo") : 0;
    memset(&C, 0, sizeof C);
    cur_pid = 0xffffffffu;
    sb_init(&C.ev, 256);
    C.need_full = 1;
    if (g_cfg.titles)
        session_add_source("input", "{\"id\": \"input\", \"kind\": \"input\", \"label\": \"Active window title\"}");
    stop_ev = CreateEventA(0, TRUE, FALSE, 0);
    th = CreateThread(0, 0, cap_thread, 0, 0, &id);
    return th != 0;
}

void capture_stop(void)
{
    if (!th) return;
    SetEvent(stop_ev);
    WaitForSingleObject(th, 30000);
    CloseHandle(th);
    CloseHandle(stop_ev);
    th = 0;
    stop_ev = 0;
    sb_free(&C.ev);
}
