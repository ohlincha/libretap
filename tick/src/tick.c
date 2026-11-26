/* tick.c - Tick's window: big clock, Start/Stop, Mark (+ note), Sync, status.
 *
 * Tick runs on the instrument's own (old) PC next to the vendor program and
 * records the screen (and, with "Start a program...", the program's serial
 * port calls) into a session folder for LibreTap.  No installation, no
 * registry, nothing written outside the session folder (and Tick.ini when
 * the operator saves options). */
#include "tick.h"
#include "resource.h"
#include <commdlg.h>
#include <tlhelp32.h>

#define IDC_START  101
#define IDC_MARK   102
#define IDC_SYNC   103
#define IDC_NOTE   104
#define IDC_STAT1  105
#define IDC_STAT2  106
#define IDC_STAT3  107
#define IDC_PROG   108
#define IDC_ADV    109

tick_cfg g_cfg;
HWND g_hwnd;
char g_exe_dir[MAX_PATH];
int g_win9x;
DWORD g_winver;

static HINSTANCE hinst;
static HWND h_start, h_mark, h_sync, h_note, h_stat1, h_stat2, h_stat3;
static HFONT f_clock, f_small, f_button, f_text;
static int S100 = 100;                  /* DPI scale in percent */
static char clock_text[32], last_msg[160];
static DWORD rec_start_tick, flash_until, msg_until;
static int flash_n;
static WNDPROC note_proc;
static DWORD pick_until;                /* "click the program's window" pending until */
static char ini_path[MAX_PATH];

#define SC(v) ((v) * S100 / 100)

/* ---------------- settings ---------------- */

void cfg_load(void)
{
    copy_str(ini_path, g_exe_dir, MAX_PATH);
    lstrcatA(ini_path, "Tick.ini");
    g_cfg.interval_ms = GetPrivateProfileIntA("tick", "interval_ms", 500, ini_path);
    g_cfg.full_every = GetPrivateProfileIntA("tick", "full_every", 60, ini_path);
    g_cfg.tile = GetPrivateProfileIntA("tick", "tile", 32, ini_path);
    g_cfg.cursor = GetPrivateProfileIntA("tick", "cursor", 1, ini_path) != 0;
    g_cfg.titles = GetPrivateProfileIntA("tick", "window_titles", 1, ini_path) != 0;
    g_cfg.mask_self = GetPrivateProfileIntA("tick", "hide_tick_window", 1, ini_path) != 0;
    g_cfg.topmost = GetPrivateProfileIntA("tick", "on_top", 1, ini_path) != 0;
    g_cfg.copy_settings = GetPrivateProfileIntA("tick", "copy_settings", 1, ini_path) != 0;
    g_cfg.program_only = GetPrivateProfileIntA("tick", "program_windows_only", 0, ini_path) != 0;
    g_cfg.rec_serial = GetPrivateProfileIntA("tick", "record_serial", 1, ini_path) != 0;
    g_cfg.rec_net = GetPrivateProfileIntA("tick", "record_network", 1, ini_path) != 0;
    g_cfg.rec_usb = GetPrivateProfileIntA("tick", "record_usb", 1, ini_path) != 0;
    GetPrivateProfileStringA("tick", "sessions_folder", "", g_cfg.dir, MAX_PATH, ini_path);
    if (g_cfg.interval_ms < 100) g_cfg.interval_ms = 100;
    if (g_cfg.interval_ms > 600000) g_cfg.interval_ms = 600000;
    if (g_cfg.full_every < 1) g_cfg.full_every = 1;
    if (g_cfg.full_every > 100000) g_cfg.full_every = 100000;
    if (g_cfg.tile < 8) g_cfg.tile = 8;
    if (g_cfg.tile > 256) g_cfg.tile = 256;
}

static BOOL put_int(const char *key, u32 v)
{
    sbuf b;
    BOOL ok;
    sb_init(&b, 16);
    sb_u32(&b, v);
    ok = WritePrivateProfileStringA("tick", key, b.p, ini_path);
    sb_free(&b);
    return ok;
}

int cfg_save(void)
{
    BOOL ok = put_int("interval_ms", g_cfg.interval_ms);
    ok = ok && put_int("full_every", g_cfg.full_every);
    ok = ok && put_int("cursor", (u32)g_cfg.cursor);
    ok = ok && put_int("window_titles", (u32)g_cfg.titles);
    ok = ok && put_int("hide_tick_window", (u32)g_cfg.mask_self);
    ok = ok && put_int("on_top", (u32)g_cfg.topmost);
    ok = ok && put_int("copy_settings", (u32)g_cfg.copy_settings);
    ok = ok && put_int("program_windows_only", (u32)g_cfg.program_only);
    ok = ok && put_int("record_serial", (u32)g_cfg.rec_serial);
    ok = ok && put_int("record_network", (u32)g_cfg.rec_net);
    ok = ok && put_int("record_usb", (u32)g_cfg.rec_usb);
    ok = ok && WritePrivateProfileStringA("tick", "sessions_folder", g_cfg.dir, ini_path);
    return ok != 0;
}

/* ---------------- window ---------------- */

static void show_msg(const char *m)
{
    copy_str(last_msg, m, sizeof last_msg);
    msg_until = GetTickCount() + 8000;
}

static void update_status(void)
{
    sbuf b;
    char ln[300];
    int rec = session_active();
    sb_init(&b, 300);
    if (msg_until && GetTickCount() < msg_until) sb_str(&b, last_msg);
    else if (rec && session_write_failed())
        sb_str(&b, "Cannot write to the session folder (USB stick removed? disk full?)");
    else if (rec) {
        DWORD s = (GetTickCount() - rec_start_tick) / 1000;
        u32 kb = session_bytes_kb();
        sb_str(&b, "Recording ");
        sb_u32w(&b, s / 3600, 2); sb_putc(&b, ':');
        sb_u32w(&b, s / 60 % 60, 2); sb_putc(&b, ':');
        sb_u32w(&b, s % 60, 2);
        sb_str(&b, " - ");
        sb_u32(&b, (u32)g_frames_stored);
        sb_str(&b, " screenshots, ");
        if (kb < 1024) { sb_u32(&b, kb); sb_str(&b, " kB"); }
        else { sb_u32(&b, kb / 1024); sb_putc(&b, '.'); sb_u32(&b, kb % 1024 * 10 / 1024); sb_str(&b, " MB"); }
        if (g_mark_n) { sb_str(&b, ", "); sb_u32(&b, (u32)g_mark_n); sb_str(&b, g_mark_n == 1 ? " mark" : " marks"); }
    } else sb_str(&b, "Not recording. Press Start to record the screen.");
    GetWindowTextA(h_stat1, ln, sizeof ln);
    if (lstrcmpA(ln, b.p)) SetWindowTextA(h_stat1, b.p);

    sb_reset(&b);
    if (rec) { sb_str(&b, "Folder: "); sb_str(&b, session_dir()); }
    else if (session_dir()[0]) { sb_str(&b, "Last session: "); sb_str(&b, session_dir()); }
    GetWindowTextA(h_stat2, ln, sizeof ln);
    if (lstrcmpA(ln, b.p)) SetWindowTextA(h_stat2, b.p);

    GetWindowTextA(h_stat3, ln, sizeof ln);
    if (lstrcmpA(ln, listener_status())) SetWindowTextA(h_stat3, listener_status());
    sb_free(&b);
}

static void update_buttons(void)
{
    int rec = session_active();
    SetWindowTextA(h_start, rec ? "Stop" : "Start");
    EnableWindow(h_mark, rec);
    EnableWindow(h_sync, rec);
    InvalidateRect(g_hwnd, 0, FALSE);
    update_status();
}

static int start_recording(void)
{
    char err[200];
    if (session_active()) return 1;
    if (!session_start(err, sizeof err)) {
        MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
        return 0;
    }
    rec_start_tick = GetTickCount();
    capture_start();
    listener_session_changed();
    msg_until = 0;
    update_buttons();
    return 1;
}

static void stop_recording(void)
{
    if (!session_active()) return;
    capture_stop();
    listener_session_changed();     /* drains what the listener still has, then pauses it */
    session_stop();
    update_buttons();
}

static void get_note(sbuf *u)
{
    char t[256];
    t[0] = 0;
    GetWindowTextA(h_note, t, sizeof t);
    ansi_to_utf8(u, t);
}

static void do_mark(void)
{
    sbuf b, u;
    LONG n;
    if (!session_active()) return;
    n = InterlockedIncrement((LONG *)&g_mark_n);
    sb_init(&b, 256);
    sb_init(&u, 256);
    get_note(&u);
    ev_begin(&b, clock_now_us(), "mark");
    sb_str(&b, ",\"n\":");
    sb_u32(&b, (u32)n);
    sb_str(&b, ",\"note\":");
    sb_json(&b, u.p ? u.p : "", u.len);
    ev_end(&b);
    sb_reset(&b);
    sb_str(&b, "Mark ");
    sb_u32(&b, (u32)n);
    sb_str(&b, " saved at ");
    sb_str(&b, clock_text);
    show_msg(b.p);
    SetWindowTextA(h_note, "");
    sb_free(&b);
    sb_free(&u);
    update_status();
}

static void do_sync(void)
{
    sbuf b;
    LONG n;
    if (!session_active()) return;
    n = InterlockedIncrement((LONG *)&g_sync_n);
    sb_init(&b, 64);
    ev_begin(&b, clock_now_us(), "sync");
    sb_str(&b, ",\"n\":");
    sb_u32(&b, (u32)n);
    ev_end(&b);
    sb_free(&b);
    flash_n = (int)n;
    flash_until = GetTickCount() + 1000;
    InvalidateRect(g_hwnd, 0, FALSE);
}

static LRESULT CALLBACK note_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_CHAR || m == WM_KEYDOWN) && w == VK_RETURN) {
        if (m == WM_KEYDOWN) do_mark();
        return 0;
    }
    return CallWindowProcA(note_proc, h, m, w, l);
}

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExA(lstrcmpA(cls, "EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text,
                             WS_CHILD | WS_VISIBLE | style, SC(x), SC(y), SC(w), SC(h),
                             g_hwnd, (HMENU)(UINT_PTR)id, hinst, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, 0);
    return c;
}

static HFONT font(int px, int bold, const char *face)
{
    return CreateFontA(-SC(px), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

static void create_controls(void)
{
    f_clock = font(54, 1, "Arial");
    f_small = font(13, 0, "Arial");
    f_button = font(20, 1, "Arial");
    f_text = font(13, 0, "Arial");
    h_start = mk("BUTTON", "Start", BS_PUSHBUTTON | WS_TABSTOP, 10, 92, 190, 48, IDC_START, f_button);
    h_mark = mk("BUTTON", "Mark", BS_PUSHBUTTON | WS_TABSTOP, 208, 92, 88, 48, IDC_MARK, f_button);
    h_sync = mk("BUTTON", "Sync", BS_PUSHBUTTON | WS_TABSTOP, 302, 92, 88, 48, IDC_SYNC, f_button);
    mk("STATIC", "Note for the next mark (Enter = Mark):", SS_LEFT, 10, 148, 380, 16, -1, f_text);
    h_note = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, 10, 165, 380, 22, IDC_NOTE, f_text);
    h_stat1 = mk("STATIC", "", SS_LEFT | SS_NOPREFIX, 10, 194, 380, 16, IDC_STAT1, f_text);
    h_stat2 = mk("STATIC", "", SS_LEFT | SS_NOPREFIX, 10, 211, 380, 16, IDC_STAT2, f_text);
    h_stat3 = mk("STATIC", "", SS_LEFT | SS_NOPREFIX, 10, 228, 380, 32, IDC_STAT3, f_text);
#ifndef TICK_SCREEN_ONLY
    mk("BUTTON", "Start a program...", BS_PUSHBUTTON | WS_TABSTOP, 10, 264, 150, 26, IDC_PROG, f_text);
#endif
    mk("BUTTON", "Advanced...", BS_PUSHBUTTON | WS_TABSTOP, 290, 264, 100, 26, IDC_ADV, f_text);
    SendMessageA(h_note, EM_LIMITTEXT, 250, 0);
    note_proc = (WNDPROC)SetWindowLongA(h_note, GWL_WNDPROC, (LONG)(UINT_PTR)note_wndproc);
}

static void paint_clock(HDC dc)
{
    RECT r, rc;
    HDC m;
    HBITMAP bm, ob;
    HFONT of;
    int rec = session_active(), flash = flash_until && GetTickCount() < flash_until;
    COLORREF bg = flash ? RGB(0, 0, 0) : rec ? RGB(255, 214, 214) : GetSysColor(COLOR_BTNFACE);
    char line[80];
    SYSTEMTIME st;
    int off;
    sbuf b;

    GetClientRect(g_hwnd, &rc);
    r.left = 0; r.top = 0; r.right = rc.right; r.bottom = SC(86);
    m = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, r.right, r.bottom);
    ob = SelectObject(m, bm);
    {
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(m, &r, br);
        DeleteObject(br);
    }
    SetBkMode(m, TRANSPARENT);
    SetTextColor(m, flash ? RGB(255, 255, 255) : RGB(0, 0, 0));
    of = SelectObject(m, f_clock);
    {
        RECT t = r;
        t.top = SC(2); t.bottom = SC(64);
        DrawTextA(m, clock_text, -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(m, f_small);
    sb_init(&b, 80);
    if (flash) {
        sb_str(&b, "SYNC ");
        sb_u32(&b, (u32)flash_n);
    } else {
        static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        clock_local(clock_now_us(), &st);
        if (rec) sb_str(&b, "\x95 REC   ");
        sb_str(&b, wd[st.wDayOfWeek % 7]);
        sb_putc(&b, ' ');
        sb_u32w(&b, st.wYear, 4); sb_putc(&b, '-');
        sb_u32w(&b, st.wMonth, 2); sb_putc(&b, '-');
        sb_u32w(&b, st.wDay, 2);
        sb_str(&b, "   UTC");
        off = clock_utc_offset_min();
        sb_putc(&b, off < 0 ? '-' : '+');
        if (off < 0) off = -off;
        sb_u32w(&b, (u32)off / 60, 2); sb_putc(&b, ':'); sb_u32w(&b, (u32)off % 60, 2);
    }
    copy_str(line, b.p, sizeof line);
    sb_free(&b);
    if (rec && !flash) SetTextColor(m, RGB(180, 0, 0));
    {
        RECT t = r;
        t.top = SC(64); t.bottom = SC(84);
        DrawTextA(m, line, -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(m, of);
    BitBlt(dc, 0, 0, r.right, r.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bm);
    DeleteDC(m);
}

static void tick_clock(void)
{
    SYSTEMTIME st;
    sbuf b;
    clock_local(clock_now_us(), &st);
    sb_init(&b, 16);
    sb_u32w(&b, st.wHour, 2); sb_putc(&b, ':');
    sb_u32w(&b, st.wMinute, 2); sb_putc(&b, ':');
    sb_u32w(&b, st.wSecond, 2); sb_putc(&b, '.');
    sb_u32(&b, st.wMilliseconds / 100);
    if (lstrcmpA(b.p, clock_text)) {
        RECT r;
        copy_str(clock_text, b.p, sizeof clock_text);
        GetClientRect(g_hwnd, &r);
        r.bottom = SC(86);
        InvalidateRect(g_hwnd, &r, FALSE);
    }
    if (flash_until && GetTickCount() >= flash_until + 50) { flash_until = 0; InvalidateRect(g_hwnd, 0, FALSE); }
    sb_free(&b);
}

/* ---------------- dialogs ---------------- */

#ifndef TICK_SCREEN_ONLY
static void pick_program(void)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH], err[300];
    file[0] = 0;
    if (listener_running()) {
        MessageBoxA(g_hwnd, "Tick is already listening to a program. Close that program first.", "Tick", MB_OK | MB_ICONINFORMATION);
        return;
    }
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = 76;           /* OPENFILENAME_SIZE_VERSION_400: works on Windows 95..11 */
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Start the vendor program (Tick records what it does on its ports)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return;
    if (!start_recording()) return;
    if (!launch_program(file, "", err, sizeof err)) MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
    update_status();
}

static INT_PTR CALLBACK attach_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    typedef HANDLE (WINAPI *SNAP)(DWORD, DWORD);
    typedef BOOL (WINAPI *P32)(HANDLE, LPPROCESSENTRY32);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    SNAP snap = (SNAP)(void *)GetProcAddress(k, "CreateToolhelp32Snapshot");
    P32 first = (P32)(void *)GetProcAddress(k, "Process32First");
    P32 next = (P32)(void *)GetProcAddress(k, "Process32Next");
    (void)l;
    if (m == WM_INITDIALOG) {
        HWND lb = GetDlgItem(d, IDC_PROCLIST);
        HANDLE s = snap && first && next ? snap(TH32CS_SNAPPROCESS, 0) : INVALID_HANDLE_VALUE;
        PROCESSENTRY32 pe;
        if (s != INVALID_HANDLE_VALUE && s) {
            memset(&pe, 0, sizeof pe);
            pe.dwSize = sizeof pe;
            if (first(s, &pe)) do {
                if (pe.th32ProcessID != GetCurrentProcessId() && pe.th32ProcessID > 8) {
                    sbuf b;
                    LRESULT i;
                    sb_init(&b, 300);
                    sb_str(&b, pe.szExeFile);
                    sb_str(&b, "   (");
                    sb_u32(&b, pe.th32ProcessID);
                    sb_putc(&b, ')');
                    i = SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)b.p);
                    SendMessageA(lb, LB_SETITEMDATA, (WPARAM)i, (LPARAM)pe.th32ProcessID);
                    sb_free(&b);
                }
            } while (next(s, &pe));
            CloseHandle(s);
        }
        return TRUE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK || (LOWORD(w) == IDC_PROCLIST && HIWORD(w) == LBN_DBLCLK)) {
            HWND lb = GetDlgItem(d, IDC_PROCLIST);
            LRESULT i = SendMessageA(lb, LB_GETCURSEL, 0, 0);
            char name[300], err[300];
            DWORD pid;
            if (i == LB_ERR) return TRUE;
            pid = (DWORD)SendMessageA(lb, LB_GETITEMDATA, (WPARAM)i, 0);
            SendMessageA(lb, LB_GETTEXT, (WPARAM)i, (LPARAM)name);
            { char *p = name; while (*p && !(p[0] == ' ' && p[1] == ' ')) p++; *p = 0; }
            EndDialog(d, IDOK);
            if (!start_recording()) return TRUE;
            if (!attach_process(pid, name, err, sizeof err)) MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    }
    return FALSE;
}

#endif

/* "Pick the program's window": the next other window the operator activates */
static void check_pick(void)
{
    HWND fg;
    DWORD pid = 0;
    char title[200];
    sbuf b;
    if (!pick_until) return;
    if (GetTickCount() > pick_until) {
        pick_until = 0;
        show_msg("No window picked.");
        return;
    }
    fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || GetParent(fg) == g_hwnd) return;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    pick_until = 0;
    g_pick_pid = pid;
    title[0] = 0;
    GetWindowTextA(fg, title, sizeof title);
    sb_init(&b, 300);
    sb_str(&b, "Picked: ");
    sb_str(&b, title[0] ? title : "(untitled window)");
    show_msg(b.p);
    sb_free(&b);
    SetForegroundWindow(g_hwnd);
}

static INT_PTR CALLBACK adv_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_INITDIALOG) {
        sbuf b;
        SetDlgItemInt(d, IDC_INTERVAL, g_cfg.interval_ms, FALSE);
        SetDlgItemInt(d, IDC_FULLEVERY, g_cfg.full_every, FALSE);
        CheckDlgButton(d, IDC_CURSOR, g_cfg.cursor ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_TITLES, g_cfg.titles ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_MASK, g_cfg.mask_self ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_TOPMOST, g_cfg.topmost ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextA(d, IDC_DIR, g_cfg.dir);
        CheckDlgButton(d, IDC_PROGONLY, g_cfg.program_only ? BST_CHECKED : BST_UNCHECKED);
#ifndef TICK_SCREEN_ONLY
        CheckDlgButton(d, IDC_COPYSET, g_cfg.copy_settings ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_RECSER, g_cfg.rec_serial ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_RECNET, g_cfg.rec_net ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_RECUSB, g_cfg.rec_usb ? BST_CHECKED : BST_UNCHECKED);
        if (g_win9x) EnableWindow(GetDlgItem(d, IDC_ATTACH), FALSE);
#else
        ShowWindow(GetDlgItem(d, IDC_RECSER), SW_HIDE);
        ShowWindow(GetDlgItem(d, IDC_RECNET), SW_HIDE);
        ShowWindow(GetDlgItem(d, IDC_RECUSB), SW_HIDE);
        ShowWindow(GetDlgItem(d, IDC_COPYSET), SW_HIDE);
        ShowWindow(GetDlgItem(d, IDC_ATTACH), SW_HIDE);
#endif
        sb_init(&b, 300);
        sb_str(&b, TICK_NAME " - part of LibreTap. Options are saved in ");
        sb_str(&b, ini_path);
        sb_str(&b, " (if that folder is writable). Changes to the screenshot options apply to the next recording.");
        SetDlgItemTextA(d, IDC_INFO, b.p);
        sb_free(&b);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        switch (LOWORD(w)) {
        case IDOK: {
            BOOL ok;
            UINT v = GetDlgItemInt(d, IDC_INTERVAL, &ok, FALSE);
            if (ok) g_cfg.interval_ms = v < 100 ? 100 : v;
            v = GetDlgItemInt(d, IDC_FULLEVERY, &ok, FALSE);
            if (ok) g_cfg.full_every = v < 1 ? 1 : v;
            g_cfg.cursor = IsDlgButtonChecked(d, IDC_CURSOR) == BST_CHECKED;
            g_cfg.titles = IsDlgButtonChecked(d, IDC_TITLES) == BST_CHECKED;
            g_cfg.mask_self = IsDlgButtonChecked(d, IDC_MASK) == BST_CHECKED;
            g_cfg.topmost = IsDlgButtonChecked(d, IDC_TOPMOST) == BST_CHECKED;
            GetDlgItemTextA(d, IDC_DIR, g_cfg.dir, MAX_PATH);
#ifndef TICK_SCREEN_ONLY
            g_cfg.copy_settings = IsDlgButtonChecked(d, IDC_COPYSET) == BST_CHECKED;
            g_cfg.rec_serial = IsDlgButtonChecked(d, IDC_RECSER) == BST_CHECKED;
            g_cfg.rec_net = IsDlgButtonChecked(d, IDC_RECNET) == BST_CHECKED;
            g_cfg.rec_usb = IsDlgButtonChecked(d, IDC_RECUSB) == BST_CHECKED;
#endif
            g_cfg.program_only = IsDlgButtonChecked(d, IDC_PROGONLY) == BST_CHECKED;
            SetWindowPos(g_hwnd, g_cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            if (!cfg_save())
                MessageBoxA(d, "Tick.ini could not be written (read-only folder?). The options apply until Tick is closed.",
                            "Tick", MB_OK | MB_ICONINFORMATION);
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        case IDC_OPENDIR: {
            sbuf b;
            const char *r = session_root();
            sb_init(&b, 300);
            sb_str(&b, "explorer.exe \"");
            if (r[0]) sb_str(&b, r);
            else { sb_str(&b, g_cfg.dir[0] ? g_cfg.dir : g_exe_dir); }
            sb_putc(&b, '"');
            WinExec(b.p, SW_SHOWNORMAL);
            sb_free(&b);
            return TRUE;
        }
        case IDC_PICKWIN:
            g_cfg.program_only = 1;
            EndDialog(d, IDCANCEL);
            pick_until = GetTickCount() + 15000;
            show_msg("Now click on the vendor program's window (15 s)...");
            update_status();
            return TRUE;
#ifndef TICK_SCREEN_ONLY
        case IDC_ATTACH:
            if (listener_running()) {
                MessageBoxA(d, "Tick is already listening to a program.", "Tick", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(d, IDCANCEL);
            DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_ATTACH), g_hwnd, attach_dlg, 0);
            return TRUE;
#endif
        }
    }
    return FALSE;
}

/* ---------------- main window ---------------- */

static int confirm_close(void)
{
    if (listener_running()) {
        int r = MessageBoxA(g_hwnd, "Tick is listening to a program. If you close Tick, that program keeps "
                            "running normally and Tick stops recording it.\n\nClose Tick?",
                            "Tick", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        return r == IDYES;
    }
    if (session_active())
        return MessageBoxA(g_hwnd, "Stop recording and close Tick?", "Tick", MB_YESNO | MB_ICONQUESTION) == IDYES;
    return 1;
}

static void shutdown_all(void)
{
    listener_detach_all();
    stop_recording();
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        create_controls();
        SetTimer(h, 1, 50, 0);
        SetTimer(h, 2, 1000, 0);
        tick_clock();
        update_buttons();
        return 0;
    case WM_TIMER:
        if (w == 1) { tick_clock(); check_pick(); }
        else { update_status(); session_flush_if_due(); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_clock(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
        if ((HWND)l == h_stat1 && msg_until && GetTickCount() < msg_until) SetTextColor((HDC)w, RGB(0, 0, 160));
    if ((HWND)l == h_stat1 && session_active() && session_write_failed()) SetTextColor((HDC)w, RGB(200, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_START:
            if (session_active()) stop_recording();
            else start_recording();
            SetFocus(h_note);
            return 0;
        case IDC_MARK: do_mark(); SetFocus(h_note); return 0;
        case IDC_SYNC: do_sync(); SetFocus(h_note); return 0;
#ifndef TICK_SCREEN_ONLY
        case IDC_PROG: pick_program(); return 0;
#endif
        case IDC_ADV: DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_ADV), h, adv_dlg, 0); update_status(); return 0;
        }
        break;
    case WM_TICK_LISTENER:
        update_status();
        return 0;
    case WM_TICK_ERROR:
        show_msg((const char *)l);
        update_status();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) shutdown_all();
        return 0;
    case WM_CLOSE:
        if (!confirm_close()) return 0;
        shutdown_all();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---------------- command line ---------------- */

static const char *skip_arg(const char *p, char *out, u32 cap)
{
    u32 n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (out && n + 1 < cap) out[n++] = *p; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (out && n + 1 < cap) out[n++] = *p; p++; }
    }
    if (out && cap) out[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void handle_cmdline(void)
{
    const char *p = skip_arg(GetCommandLineA(), 0, 0);
    char a[MAX_PATH], err[300];
    (void)err;
    while (*p) {
        p = skip_arg(p, a, sizeof a);
        if (!lstrcmpiA(a, "/start") || !lstrcmpiA(a, "-start")) start_recording();
#ifndef TICK_SCREEN_ONLY
        else if (!lstrcmpiA(a, "/run") || !lstrcmpiA(a, "-run")) {
            p = skip_arg(p, a, sizeof a);
            if (a[0] && start_recording() && !launch_program(a, p, err, sizeof err))
                MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
            break;                  /* the rest are the program's arguments */
        }
#endif
    }
}

/* ---------------- entry point (no C runtime) ---------------- */

void __stdcall TickMain(void)
{
    WNDCLASSA wc;
    MSG msg;
    RECT wa, r;
    HDC dc;
    int i;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    hinst = GetModuleHandleA(0);
    g_winver = GetVersion();
    g_win9x = (g_winver & 0x80000000u) != 0;
    /* no "insert a disk" boxes if the USB stick is pulled */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    {   /* real pixels on scaled Windows 8-11 screens */
        typedef BOOL (WINAPI *SPDA)(void);
        SPDA f = (SPDA)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        if (f) f();
    }
    GetModuleFileNameA(0, g_exe_dir, MAX_PATH);
    for (i = lstrlenA(g_exe_dir); i > 0 && g_exe_dir[i - 1] != '\\'; i--) ;
    g_exe_dir[i] = 0;
    clock_init();
    cfg_load();
    dc = GetDC(0);
    S100 = GetDeviceCaps(dc, LOGPIXELSY) * 100 / 96;
    ReleaseDC(0, dc);
    if (S100 < 100) S100 = 100;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(IDI_TICK));
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "TickRecorder";
    RegisterClassA(&wc);

    r.left = 0; r.top = 0; r.right = SC(400); r.bottom = SC(300);
    AdjustWindowRectEx(&r, style, FALSE, 0);
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    CreateWindowExA(g_cfg.topmost ? WS_EX_TOPMOST : 0, "TickRecorder",
#ifdef TICK_SCREEN_ONLY
                    "TickScreen - LibreTap screen recorder",
#else
                    "Tick - LibreTap recorder",
#endif
                    style,
                    wa.right - (r.right - r.left) - 8, wa.bottom - (r.bottom - r.top) - 8,
                    r.right - r.left, r.bottom - r.top, 0, 0, hinst, 0);
    if (!g_hwnd) ExitProcess(1);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetFocus(h_note);
    handle_cmdline();

    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
