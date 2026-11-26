/* session.c - the session folder: session.json, events.jsonl, frames/, files/
 * (format: docs/FORMAT.md).  events.jsonl is appended line by line and
 * flushed at least once a second, so a crash or a pulled USB stick leaves a
 * readable session. */
#include "tick.h"
#include <tlhelp32.h>

volatile LONG g_frames_stored, g_mark_n, g_sync_n;

#define MAXSRC 32
static struct { char id[16]; char *json; } srcs[MAXSRC];
static int nsrc;

static CRITICAL_SECTION cs;
static int cs_ready;
static volatile int active;
static HANDLE hev = INVALID_HANDLE_VALUE;
static char root[MAX_PATH], dir[MAX_PATH];
static u64 started_us, ended_us, bytes;
static DWORD last_flush;
static int write_failed, dirty;
static char note[256];
static char capture_proc[MAX_PATH];

static void lock(void)
{
    if (!cs_ready) { InitializeCriticalSection(&cs); cs_ready = 1; }
    EnterCriticalSection(&cs);
}
static void unlock(void) { LeaveCriticalSection(&cs); }

void copy_str(char *d, const char *s, u32 cap)
{
    u32 i = 0;
    if (!cap) return;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void cat_str(char *d, const char *s, u32 cap)
{
    u32 n = (u32)strlen(d);
    if (n < cap) copy_str(d + n, s, cap - n);
}

void ansi_to_utf8(sbuf *b, const char *s)
{
    int n;
    u16 *w;
    if (!s || !*s) return;
    n = MultiByteToWideChar(CP_ACP, 0, s, -1, 0, 0);
    w = n > 0 ? rt_alloc((u32)n * 2 + 2) : 0;
    if (w && MultiByteToWideChar(CP_ACP, 0, s, -1, (LPWSTR)w, n) > 0) sb_utf16(b, w, 0xffffffffu);
    else sb_str(b, s);
    rt_free(w);
}

int session_active(void) { return active; }
const char *session_dir(void) { return dir; }
const char *session_root(void) { return root; }
u32 session_bytes_kb(void) { return (u32)(bytes >> 10); }

static int write_all(HANDLE h, const void *p, u32 n)
{
    DWORD w = 0;
    return WriteFile(h, p, n, &w, 0) && w == n;
}

static void write_session_json(void)
{
    sbuf b;
    OSVERSIONINFOA ov;
    char host[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD hn = sizeof host;
    char path[MAX_PATH];
    HANDLE h;
    int i;

    sb_init(&b, 2048);
    sb_str(&b, "{\n  \"format\": \"libretap-session\",\n  \"version\": 1,\n  \"recorder\": \"" TICK_NAME "\",\n  \"host\": ");
    host[0] = 0;
    if (!GetComputerNameA(host, &hn)) host[0] = 0;
    {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, host);
        sb_json(&b, u.p, u.len);
        sb_free(&u);
    }
    sb_str(&b, ",\n  \"os\": \"Windows ");
    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    if (GetVersionExA(&ov)) {
        sb_u32(&b, ov.dwMajorVersion); sb_putc(&b, '.'); sb_u32(&b, ov.dwMinorVersion);
        sb_str(&b, " build ");
        sb_u32(&b, ov.dwPlatformId == VER_PLATFORM_WIN32_NT ? ov.dwBuildNumber : (ov.dwBuildNumber & 0xffff));
        if (ov.szCSDVersion[0] && ov.szCSDVersion[0] != ' ') {
            sbuf u;
            sb_init(&u, 64);
            ansi_to_utf8(&u, ov.szCSDVersion);
            sb_putc(&b, ' ');
            for (i = 0; i < (int)u.len; i++) if (u.p[i] != '"' && u.p[i] != '\\') sb_putc(&b, u.p[i]);
            sb_free(&u);
        }
    }
    sb_str(&b, "\",\n  \"started\": \"");
    clock_iso(&b, started_us);
    sb_str(&b, "\",\n");
    if (ended_us) {
        sb_str(&b, "  \"ended\": \"");
        clock_iso(&b, ended_us);
        sb_str(&b, "\",\n");
    }
    sb_str(&b, "  \"clock\": \"Recorder's own wall clock; event t = seconds since 1970-01-01 UTC by that clock\",\n"
               "  \"utc_offset_min\": ");
    sb_i32(&b, clock_utc_offset_min());
    sb_str(&b, ",\n  \"sources\": [");
    for (i = 0; i < nsrc; i++) {
        sb_str(&b, i ? ",\n    " : "\n    ");
        sb_str(&b, srcs[i].json);
    }
    sb_str(&b, "\n  ],\n  \"tick\": {\"interval_ms\": ");
    sb_u32(&b, g_cfg.interval_ms);
    sb_str(&b, ", \"full_every\": ");
    sb_u32(&b, g_cfg.full_every);
    sb_str(&b, ", \"tile\": ");
    sb_u32(&b, g_cfg.tile);
    sb_str(&b, g_cfg.cursor ? ", \"cursor\": true" : ", \"cursor\": false");
    sb_str(&b, g_cfg.mask_self ? ", \"tick_window_masked\": true" : ", \"tick_window_masked\": false");
    sb_str(&b, g_cfg.program_only ? ", \"capture\": \"program-windows\"" : ", \"capture\": \"screen\"");
    if (g_cfg.program_only) {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, capture_proc);
        sb_str(&b, ", \"capture_process\": ");
        sb_json(&b, u.p ? u.p : "", u.len);
        sb_free(&u);
    }
    sb_putc(&b, '}');
    sb_str(&b, ",\n  \"note\": ");
    {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, note);
        sb_json(&b, u.p, u.len);
        sb_free(&u);
    }
    sb_str(&b, "\n}\n");

    copy_str(path, dir, MAX_PATH);
    cat_str(path, "session.json", MAX_PATH);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        if (!write_all(h, b.p, b.len)) write_failed = 1;
        FlushFileBuffers(h);
        CloseHandle(h);
    } else write_failed = 1;
    sb_free(&b);
}

static int dir_ok(const char *d)
{
    DWORD a;
    CreateDirectoryA(d, 0);
    a = GetFileAttributesA(d);
    return a != 0xffffffffu && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* try to create <base>\<name> with an events.jsonl in it */
static int try_root(const char *base)
{
    SYSTEMTIME st;
    char host[MAX_COMPUTERNAME_LENGTH + 2], name[80], path[MAX_PATH];
    DWORD hn = sizeof host;
    sbuf b;
    int k, i;

    if (!base[0]) return 0;
    copy_str(root, base, MAX_PATH);
    if (root[strlen(root) - 1] != '\\') cat_str(root, "\\", MAX_PATH);
    if (!dir_ok(root)) return 0;

    clock_local(started_us, &st);
    if (!GetComputerNameA(host, &hn)) host[0] = 0;
    sb_init(&b, 64);
    sb_u32w(&b, st.wYear, 4); sb_u32w(&b, st.wMonth, 2); sb_u32w(&b, st.wDay, 2);
    sb_putc(&b, '-');
    sb_u32w(&b, st.wHour, 2); sb_u32w(&b, st.wMinute, 2); sb_u32w(&b, st.wSecond, 2);
    if (host[0]) {
        sb_putc(&b, '-');
        for (i = 0; host[i] && i < 24; i++) {
            char c = host[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) c = '_';
            sb_putc(&b, c);
        }
    }
    copy_str(name, b.p, sizeof name);
    sb_free(&b);
    for (k = 1; k < 20; k++) {
        copy_str(dir, root, MAX_PATH);
        cat_str(dir, name, MAX_PATH);
        if (k > 1) {
            char s[4];
            s[0] = '-'; s[1] = (char)('0' + k / 10); s[2] = (char)('0' + k % 10); s[3] = 0;
            if (k < 10) { s[1] = (char)('0' + k); s[2] = 0; }
            cat_str(dir, s, MAX_PATH);
        }
        if (CreateDirectoryA(dir, 0)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    if (k == 20) return 0;
    cat_str(dir, "\\", MAX_PATH);
    copy_str(path, dir, MAX_PATH);
    cat_str(path, "events.jsonl", MAX_PATH);
    hev = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (hev == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(dir);
        return 0;
    }
    return 1;
}

static int documents_dir(char *out)
{
    typedef BOOL (WINAPI *SHGSFP)(HWND, LPSTR, int, BOOL);
    HMODULE m = LoadLibraryA("shell32.dll");
    SHGSFP f = m ? (SHGSFP)(void *)GetProcAddress(m, "SHGetSpecialFolderPathA") : 0;
    int ok = 0;
    if (f && f(0, out, 5 /* CSIDL_PERSONAL */, FALSE)) ok = 1;
    if (m) FreeLibrary(m);
    return ok;
}

int session_start(char *err, u32 errcap)
{
    char base[MAX_PATH];
    int ok = 0;
    sbuf b;

    if (active) return 1;
    lock();
    started_us = clock_now_us();
    ended_us = 0;
    capture_proc[0] = 0;
    bytes = 0;
    write_failed = 0;
    nsrc = 0;
    g_frames_stored = 0;
    g_mark_n = 0;
    g_sync_n = 0;
    if (g_cfg.dir[0]) ok = try_root(g_cfg.dir);
    if (!ok) {
        copy_str(base, g_exe_dir, MAX_PATH);
        cat_str(base, "TickSessions", MAX_PATH);
        ok = try_root(base);
    }
    if (!ok && documents_dir(base)) {
        cat_str(base, "\\TickSessions", MAX_PATH);
        ok = try_root(base);
    }
    if (!ok && GetTempPathA(MAX_PATH, base)) {
        cat_str(base, "TickSessions", MAX_PATH);
        ok = try_root(base);
    }
    if (!ok) {
        unlock();
        copy_str(err, "Could not create a session folder (next to Tick.exe, in Documents or in TEMP).", errcap);
        return 0;
    }
    session_mkdir("frames");
    active = 1;
    write_session_json();
    last_flush = GetTickCount();
    unlock();

    sb_init(&b, 256);
    sb_str(&b, "Tick started recording into ");
    ansi_to_utf8(&b, dir);
    ev_status("status", b.p);
    sb_free(&b);
    return 1;
}

void session_stop(void)
{
    if (!active) return;
    ev_status("status", "Tick stopped recording");
    lock();
    active = 0;
    ended_us = clock_now_us();
    FlushFileBuffers(hev);
    CloseHandle(hev);
    hev = INVALID_HANDLE_VALUE;
    write_session_json();
    {
        int i;
        for (i = 0; i < nsrc; i++) { rt_free(srcs[i].json); srcs[i].json = 0; }
        nsrc = 0;
    }
    unlock();
}

void session_set_note(const char *n)
{
    lock();
    copy_str(note, n, sizeof note);
    if (active) write_session_json();
    unlock();
}

void ev_begin(sbuf *b, u64 t, const char *kind)
{
    sb_reset(b);
    sb_str(b, "{\"t\":");
    sb_time(b, t);
    sb_str(b, ",\"kind\":\"");
    sb_str(b, kind);
    sb_putc(b, '"');
}

/* is this the line of an event of the given kind? (ev_begin's layout) */
static int has_kind(const sbuf *b, const char *kind)
{
    u32 i, n = (u32)strlen(kind);
    for (i = 0; i + 9 + n < b->len && i < 40; i++)
        if (memcmp(b->p + i, ",\"kind\":\"", 9) == 0)
            return memcmp(b->p + i + 9, kind, n) == 0 && b->p[i + 9 + n] == '"';
    return 0;
}

void ev_end(sbuf *b)
{
    int urgent;
    sb_str(b, "}\n");
    if (b->oom) return;
    urgent = has_kind(b, "mark") || has_kind(b, "sync") || has_kind(b, "error");
    lock();
    if (active && hev != INVALID_HANDLE_VALUE) {
        DWORD now = GetTickCount();
        if (!write_all(hev, b->p, b->len)) write_failed = 1;
        else { bytes += b->len; dirty = 1; }
        /* marks and syncs are flushed at once, everything else at most 1 s later */
        if (urgent || now - last_flush >= 1000) {
            FlushFileBuffers(hev);
            last_flush = now;
            dirty = 0;
        }
    }
    unlock();
}

void ev_status(const char *kind, const char *text)
{
    sbuf b;
    sb_init(&b, 128);
    ev_begin(&b, clock_now_us(), kind);
    sb_str(&b, ",\"text\":");
    sb_jsonz(&b, text);
    ev_end(&b);
    sb_free(&b);
}

int session_has_source(const char *id)
{
    int i, r = 0;
    lock();
    for (i = 0; i < nsrc; i++) if (lstrcmpA(srcs[i].id, id) == 0) r = 1;
    unlock();
    return r;
}

void session_add_source(const char *id, const char *json)
{
    int i;
    lock();
    for (i = 0; i < nsrc; i++) if (lstrcmpA(srcs[i].id, id) == 0) break;
    if (i == nsrc && nsrc < MAXSRC && active) {
        u32 n = (u32)strlen(json);
        srcs[nsrc].json = rt_alloc(n + 1);
        if (srcs[nsrc].json) {
            memcpy(srcs[nsrc].json, json, n);
            copy_str(srcs[nsrc].id, id, sizeof srcs[nsrc].id);
            nsrc++;
            write_session_json();
        }
    }
    unlock();
}

void session_mkdir(const char *rel)
{
    char path[MAX_PATH];
    copy_str(path, dir, MAX_PATH);
    cat_str(path, rel, MAX_PATH);
    CreateDirectoryA(path, 0);
}

int session_write_file(const char *rel, const void *p, u32 n)
{
    char path[MAX_PATH];
    HANDLE h;
    int ok;
    copy_str(path, dir, MAX_PATH);
    cat_str(path, rel, MAX_PATH);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = write_all(h, p, n);
    CloseHandle(h);
    if (!ok) { DeleteFileA(path); return 0; }
    lock();
    bytes += n;
    unlock();
    return 1;
}

/* called about once a second by the window: flush a quiet events.jsonl */
void session_flush_if_due(void)
{
    lock();
    if (active && dirty && GetTickCount() - last_flush >= 1000) {
        FlushFileBuffers(hev);
        last_flush = GetTickCount();
        dirty = 0;
    }
    unlock();
}

int session_write_failed(void) { return write_failed; }

void session_set_capture(const char *process)
{
    lock();
    copy_str(capture_proc, process, sizeof capture_proc);
    if (active) write_session_json();
    unlock();
}

void process_name(DWORD pid, char *out)
{
    typedef HANDLE (WINAPI *SNAP)(DWORD, DWORD);
    typedef BOOL (WINAPI *P32)(HANDLE, LPPROCESSENTRY32);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    SNAP snap = (SNAP)(void *)GetProcAddress(k, "CreateToolhelp32Snapshot");
    P32 first = (P32)(void *)GetProcAddress(k, "Process32First");
    P32 next = (P32)(void *)GetProcAddress(k, "Process32Next");
    HANDLE s;
    PROCESSENTRY32 pe;
    out[0] = 0;
    if (!snap || !first || !next) return;
    s = snap(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE || !s) return;
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (first(s, &pe)) do {
        if (pe.th32ProcessID == pid) {
            const char *b = pe.szExeFile, *p;
            for (p = pe.szExeFile; *p; p++) if (*p == '\\') b = p + 1;
            copy_str(out, b, MAX_PATH);
            break;
        }
    } while (next(s, &pe));
    CloseHandle(s);
}
