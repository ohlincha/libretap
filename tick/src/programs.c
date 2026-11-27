/* programs.c - choosing the program Tick listens to, in Tick's window (Windows):
 *   - a list: programs running now (with a window), then the Start menu and Desktop
 *     shortcuts (.lnk files read by lnk.c, no COM);
 *   - Pick window: the operator activates the program's window;
 *   - Browse... (.exe or .lnk), or a file dropped onto Tick's window.
 * A program that is already running is closed (WM_CLOSE, as its own close button does; never
 * terminated) and started again under Tick, from its shortcut when there is one; or, on
 * Windows NT/2000 and later, listened to without restarting. */
#include "tick.h"
#include "resource.h"
#include "lnk.h"
#include <commdlg.h>
#include <tlhelp32.h>
#include <shellapi.h>

#ifndef TICK_SCREEN_ONLY

typedef struct {
    int running;                /* 1: running now (pid), 0: a shortcut */
    DWORD pid;
    char label[300];
    char target[MAX_PATH], args[LNK_TEXT], workdir[MAX_PATH];
    char where[160];            /* "Start menu > Vendor", "Desktop", "Installed" */
    char install_dir[MAX_PATH]; /* installed software without a known .exe: browse here */
} prog_entry;

#define MAX_ENTRIES 600
static prog_entry *ents;
static int nents;
static int chosen = -1;

/* restart in progress */
static HANDLE rs_proc;
static DWORD rs_pid, rs_deadline;
static prog_entry rs_ent;

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int ends_with(const char *s, const char *suf)
{
    int a = lstrlenA(s), b = lstrlenA(suf), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++) if (lower(s[a - b + i]) != lower(suf[i])) return 0;
    return 1;
}

static int contains_ci(const char *h, const char *n)
{
    int i, j;
    for (i = 0; h[i]; i++) {
        for (j = 0; n[j] && lower(h[i + j]) == lower(n[j]); j++) ;
        if (!n[j]) return 1;
    }
    return 0;
}

static int same_path(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

/* programs that run other programs (cmd.exe /c ..., javaw -jar ..., wscript x.vbs): a running
 * one is never taken for the shortcut's program */
static int generic(const char *exe)
{
    static const char *g[] = {"cmd.exe", "command.com", "wscript.exe", "cscript.exe", "java.exe", "javaw.exe", "rundll32.exe",
                              "mshta.exe", "powershell.exe", "pwsh.exe", "explorer.exe", "msiexec.exe", "mmc.exe", "start.exe",
                              "python.exe", "pythonw.exe", "py.exe", "pyw.exe", "perl.exe", "conhost.exe", "wine.exe", 0};
    const char *b = exe, *p;
    int i;
    for (p = exe; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    for (i = 0; g[i]; i++) if (!lstrcmpiA(b, g[i])) return 1;
    return 0;
}

static const char *base_of(const char *p)
{
    const char *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

/* consoles, shells and installers: never offered as "Running" (closing them closes other work) */
static int shell_like(const char *exe)
{
    static const char *g[] = {"cmd.exe", "command.com", "conhost.exe", "powershell.exe", "pwsh.exe", "rundll32.exe",
                              "msiexec.exe", "mmc.exe", "start.exe", "wine.exe", "taskmgr.exe", 0};
    int i;
    for (i = 0; g[i]; i++) if (!lstrcmpiA(base_of(exe), g[i])) return 1;
    return 0;
}

static void w2a(const u16 *w, u32 n, char *out, u32 cap)
{
    int k = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)w, (int)n, out, (int)cap - 1, 0, 0);
    out[k > 0 ? k : 0] = 0;
}

/* full path of a running program's file */
static void process_path(DWORD pid, char *out)
{
    typedef BOOL (WINAPI *QFPIN)(HANDLE, DWORD, LPSTR, PDWORD);
    typedef DWORD (WINAPI *GMFNE)(HANDLE, HMODULE, LPSTR, DWORD);
    QFPIN q = (QFPIN)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryFullProcessImageNameA");
    HANDLE h;
    out[0] = 0;
    if (q && (h = OpenProcess(0x1000 /* QUERY_LIMITED_INFORMATION */, FALSE, pid))) {
        DWORD n = MAX_PATH;
        if (!q(h, 0, out, &n)) out[0] = 0;
        CloseHandle(h);
        if (out[0]) return;
    }
    if (!g_win9x) {
        HMODULE ps = LoadLibraryA("psapi.dll");
        GMFNE g = ps ? (GMFNE)(void *)GetProcAddress(ps, "GetModuleFileNameExA") : 0;
        if (g && (h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid))) {
            if (!g(h, 0, out, MAX_PATH)) out[0] = 0;
            CloseHandle(h);
        }
        if (ps) FreeLibrary(ps);
        if (out[0]) return;
    }
    {   /* Windows 9x: the snapshot has the full path; NT: the file name only */
        typedef HANDLE (WINAPI *SNAP)(DWORD, DWORD);
        typedef BOOL (WINAPI *P32)(HANDLE, LPPROCESSENTRY32);
        HMODULE k = GetModuleHandleA("kernel32.dll");
        SNAP snap = (SNAP)(void *)GetProcAddress(k, "CreateToolhelp32Snapshot");
        P32 first = (P32)(void *)GetProcAddress(k, "Process32First"), next = (P32)(void *)GetProcAddress(k, "Process32Next");
        PROCESSENTRY32 pe;
        HANDLE s;
        if (!snap || !first || !next) return;
        s = snap(TH32CS_SNAPPROCESS, 0);
        if (s == INVALID_HANDLE_VALUE || !s) return;
        memset(&pe, 0, sizeof pe);
        pe.dwSize = sizeof pe;
        if (first(s, &pe)) do {
            if (pe.th32ProcessID == pid) { copy_str(out, pe.szExeFile, MAX_PATH); break; }
        } while (next(s, &pe));
        CloseHandle(s);
    }
}

/* ---------------- reading a shortcut ---------------- */

/* path of a .lnk -> program, arguments, folder; 0 with a reason */
static int read_shortcut(const char *lnk, prog_entry *e, char *err, u32 errcap)
{
    HANDLE f = CreateFileA(lnk, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD n = 0, size;
    u8 *buf;
    lnk_info li;
    if (f == INVALID_HANDLE_VALUE) { copy_str(err, "Cannot read the shortcut.", errcap); return 0; }
    size = GetFileSize(f, 0);
    if (size == 0xffffffffu || size > 1024 * 1024) size = 1024 * 1024;
    buf = rt_alloc(size + 1);
    if (!buf || !ReadFile(f, buf, size, &n, 0)) n = 0;
    CloseHandle(f);
    if (!buf || !lnk_parse(buf, n, &li, w2a)) {
        rt_free(buf);
        copy_str(err, "Not a shortcut Tick can read.", errcap);
        return 0;
    }
    rt_free(buf);
    if (li.target_has_env) {
        typedef DWORD (WINAPI *EXP)(LPCSTR, LPSTR, DWORD);
        EXP x = (EXP)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExpandEnvironmentStringsA");
        char t[MAX_PATH];
        if (x && x(li.target, t, MAX_PATH)) copy_str(li.target, t, LNK_TEXT);
    }
    if (!li.target[0] && li.relative[0]) {          /* relative to the shortcut's folder */
        int k;
        copy_str(li.target, lnk, LNK_TEXT);
        for (k = lstrlenA(li.target); k > 0 && li.target[k - 1] != '\\'; k--) ;
        li.target[k] = 0;
        cat_str(li.target, li.relative, LNK_TEXT);
    }
    copy_str(e->target, li.target, MAX_PATH);
    copy_str(e->args, li.args, LNK_TEXT);
    copy_str(e->workdir, li.workdir, MAX_PATH);
    if (!e->target[0]) { copy_str(err, "The shortcut names no program.", errcap); return 0; }
    if (!ends_with(e->target, ".exe")) {
        sbuf b;
        sb_init(&b, 300);
        sb_str(&b, "The shortcut does not start a program: ");
        sb_str(&b, e->target);
        copy_str(err, b.p, errcap);
        sb_free(&b);
        return 0;
    }
    return 1;
}

/* ---------------- the list ---------------- */

static void add(const prog_entry *e)
{
    int i;
    if (nents >= MAX_ENTRIES) return;
    for (i = 0; i < nents; i++) {
        if (ents[i].running || e->running) continue;
        if (e->target[0] && same_path(ents[i].target, e->target)) return;          /* same program: keep the first */
        if (!e->target[0] && !lstrcmpiA(ents[i].label, e->label)) return;
    }
    ents[nents++] = *e;
}

static void scan(const char *dir, const char *label, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    sbuf pat;
    if (depth > 5 || nents >= MAX_ENTRIES) return;
    sb_init(&pat, MAX_PATH);
    sb_str(&pat, dir);
    sb_str(&pat, "\\*");
    h = FindFirstFileA(pat.p, &fd);
    sb_free(&pat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH], lab[300];
        if (fd.cFileName[0] == '.') continue;
        copy_str(path, dir, MAX_PATH);
        cat_str(path, "\\", MAX_PATH);
        cat_str(path, fd.cFileName, MAX_PATH);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            copy_str(lab, label, sizeof lab);
            cat_str(lab, " > ", sizeof lab);
            cat_str(lab, fd.cFileName, sizeof lab);
            scan(path, lab, depth + 1);
        } else if (ends_with(fd.cFileName, ".lnk")) {
            prog_entry e;
            char err[200];
            int k;
            memset(&e, 0, sizeof e);
            copy_str(lab, fd.cFileName, sizeof lab);
            k = lstrlenA(lab);
            lab[k - 4] = 0;                         /* no ".lnk" */
            if (contains_ci(lab, "uninst") || contains_ci(lab, "remove ")) continue;   /* never offer an uninstaller */
            if (!read_shortcut(path, &e, err, sizeof err)) continue;
            if (contains_ci(base_of(e.target), "unins")) continue;
            copy_str(e.label, lab, sizeof e.label);
            copy_str(e.where, label, sizeof e.where);
            add(&e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int folder(int csidl, char *out)
{
    typedef BOOL (WINAPI *SGSFP)(HWND, LPSTR, int, BOOL);
    HMODULE sh = LoadLibraryA("shell32.dll");
    SGSFP f = sh ? (SGSFP)(void *)GetProcAddress(sh, "SHGetSpecialFolderPathA") : 0;
    int ok = f && f(0, out, csidl, FALSE) && out[0];
    if (sh) FreeLibrary(sh);
    return ok;
}

/* ---- installed software (Uninstall keys), advapi32 loaded at run time ---- */
typedef LONG (WINAPI *ROK)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *REK)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, PFILETIME);
typedef LONG (WINAPI *RQV)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *RCK)(HKEY);
static ROK r_open;
static REK r_enum;
static RQV r_query;
static RCK r_close;

static int reg_str(HKEY k, const char *name, char *out, DWORD cap)
{
    DWORD type = 0, n = cap - 1;
    out[0] = 0;
    if (r_query(k, name, 0, &type, (LPBYTE)out, &n) || (type != REG_SZ && type != REG_EXPAND_SZ)) { out[0] = 0; return 0; }
    out[n < cap ? n : cap - 1] = 0;
    return out[0] != 0;
}

static int reg_dword(HKEY k, const char *name)
{
    DWORD type = 0, v = 0, n = 4;
    if (r_query(k, name, 0, &type, (LPBYTE)&v, &n) || type != REG_DWORD) return 0;
    return (int)v;
}

/* "C:\Program Files\Vendor\acq.exe",0  ->  C:\Program Files\Vendor\acq.exe */
static void icon_exe(const char *icon, char *out)
{
    int i = 0, k;
    const char *p = icon;
    if (*p == '"') p++;
    while (*p && *p != '"' && i < MAX_PATH - 1) out[i++] = *p++;
    out[i] = 0;
    for (k = i - 1; k > 0 && out[k] >= '0' && out[k] <= '9'; k--) ;
    if (k > 0 && out[k] == ',' && k < i - 1) out[k] = 0;
    if (k > 0 && out[k] == '-' && out[k - 1] == ',') out[k - 1] = 0;
}

static void scan_uninstall(HKEY root, REGSAM view)
{
    HKEY k;
    DWORD i;
    if (r_open(root, "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &k)) return;
    for (i = 0; i < 2000 && nents < MAX_ENTRIES; i++) {
        char sub[256], name[300], icon[MAX_PATH], loc[MAX_PATH], parent[100];
        DWORD n = sizeof sub;
        HKEY s;
        prog_entry e;
        if (r_enum(k, i, sub, &n, 0, 0, 0, 0)) break;
        if (r_open(k, sub, 0, KEY_READ | view, &s)) continue;
        if (reg_str(s, "DisplayName", name, sizeof name) && !reg_dword(s, "SystemComponent") &&
            !reg_str(s, "ParentKeyName", parent, sizeof parent) && !contains_ci(name, "update for") &&
            !contains_ci(name, "hotfix") && !contains_ci(name, "redistributable") && !contains_ci(name, "runtime")) {
            memset(&e, 0, sizeof e);
            copy_str(e.label, name, sizeof e.label);
            copy_str(e.where, "Installed", sizeof e.where);
            if (reg_str(s, "DisplayIcon", icon, sizeof icon)) {
                icon_exe(icon, e.target);
                if (!ends_with(e.target, ".exe") || contains_ci(base_of(e.target), "unins") ||
                    GetFileAttributesA(e.target) == 0xffffffffu) e.target[0] = 0;
            }
            if (reg_str(s, "InstallLocation", loc, sizeof loc)) {
                int L = lstrlenA(loc);
                if (L && loc[0] == '"') { copy_str(loc, loc + 1, sizeof loc); L--; if (L && loc[L - 1] == '"') loc[--L] = 0; }
                copy_str(e.install_dir, loc, MAX_PATH);
            }
            if (e.target[0] || e.install_dir[0]) add(&e);
        }
        r_close(s);
    }
    r_close(k);
}

static void scan_installed(void)
{
    HMODULE a = LoadLibraryA("advapi32.dll");
    if (!a) return;
    r_open = (ROK)(void *)GetProcAddress(a, "RegOpenKeyExA");
    r_enum = (REK)(void *)GetProcAddress(a, "RegEnumKeyExA");
    r_query = (RQV)(void *)GetProcAddress(a, "RegQueryValueExA");
    r_close = (RCK)(void *)GetProcAddress(a, "RegCloseKey");
    if (r_open && r_enum && r_query && r_close) {
        scan_uninstall(HKEY_LOCAL_MACHINE, 0);
        scan_uninstall(HKEY_CURRENT_USER, 0);
        {   /* 32-bit Tick on 64-bit Windows: also the 64-bit programs' list */
            typedef BOOL (WINAPI *ISW)(HANDLE, PBOOL);
            ISW f = (ISW)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
            BOOL w = FALSE;
            if (f && f(GetCurrentProcess(), &w) && w) scan_uninstall(HKEY_LOCAL_MACHINE, 0x0100 /* KEY_WOW64_64KEY */);
        }
    }
    FreeLibrary(a);
}

static BOOL CALLBACK enum_running(HWND w, LPARAM l)
{
    DWORD pid = 0;
    char title[200], path[MAX_PATH];
    prog_entry e;
    int i;
    (void)l;
    if (!IsWindowVisible(w) || GetParent(w) || w == g_hwnd) return TRUE;
    title[0] = 0;
    GetWindowTextA(w, title, sizeof title);
    if (!title[0]) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return TRUE;
    for (i = 0; i < nents; i++) if (ents[i].running && ents[i].pid == pid) return TRUE;
    process_path(pid, path);
    if (!path[0] || !lstrcmpiA(base_of(path), "explorer.exe") || !lstrcmpiA(base_of(path), "progman.exe") || shell_like(path))
        return TRUE;
    memset(&e, 0, sizeof e);
    e.running = 1;
    e.pid = pid;
    copy_str(e.target, path, MAX_PATH);
    copy_str(e.label, "Running: ", sizeof e.label);
    cat_str(e.label, title, sizeof e.label);
    cat_str(e.label, " (", sizeof e.label);
    cat_str(e.label, base_of(path), sizeof e.label);
    cat_str(e.label, ")", sizeof e.label);
    add(&e);
    return TRUE;
}

static void sort_range(int from, int to)
{
    int i, j;
    for (i = from + 1; i < to; i++) {               /* insertion sort: a few hundred entries */
        prog_entry t = ents[i];
        for (j = i; j > from && lstrcmpiA(ents[j - 1].label, t.label) > 0; j--) ents[j] = ents[j - 1];
        ents[j] = t;
    }
}

static void build_list(void)
{
    char d[MAX_PATH];
    int run_end;
    if (!ents) ents = rt_alloc(sizeof *ents * MAX_ENTRIES);
    nents = 0;
    if (!ents) return;
    EnumWindows(enum_running, 0);
    run_end = nents;
    if (folder(0x17, d)) scan(d, "Start menu", 0);          /* all users' programs */
    if (folder(0x02, d)) scan(d, "Start menu", 0);          /* this user's programs */
    if (folder(0x19, d)) scan(d, "Desktop", 0);
    if (folder(0x10, d)) scan(d, "Desktop", 0);
    if (nents == run_end) {                                 /* Windows 95 without the shell update */
        GetWindowsDirectoryA(d, MAX_PATH);
        cat_str(d, "\\Start Menu\\Programs", MAX_PATH);
        scan(d, "Start menu", 0);
        GetWindowsDirectoryA(d, MAX_PATH);
        cat_str(d, "\\Desktop", MAX_PATH);
        scan(d, "Desktop", 0);
    }
    scan_installed();                                       /* after the shortcuts: they win duplicates */
    sort_range(0, run_end);
    sort_range(run_end, nents);
}

/* a shortcut that starts the same program (its arguments and folder are the vendor's) */
static const prog_entry *shortcut_for(const char *exe)
{
    int i;
    for (i = 0; i < nents; i++) if (!ents[i].running && same_path(ents[i].target, exe)) return &ents[i];
    return 0;
}

/* ---------------- starting ---------------- */

static int launch_entry(const prog_entry *e)
{
    char err[300];
    if (GetFileAttributesA(e->target) == 0xffffffffu) {
        sbuf b;
        sb_init(&b, 300);
        sb_str(&b, "Program not found: ");
        sb_str(&b, e->target);
        MessageBoxA(g_hwnd, b.p, "Tick", MB_OK | MB_ICONERROR);
        sb_free(&b);
        return 0;
    }
    if (!tick_start_recording()) return 0;
    if (!launch_program(e->target, e->args, e->workdir[0] ? e->workdir : 0, err, sizeof err)) {
        MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
        return 0;
    }
    return 1;
}

static BOOL CALLBACK close_windows(HWND w, LPARAM l)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == (DWORD)l && IsWindowVisible(w) && !GetParent(w)) PostMessageA(w, WM_CLOSE, 0, 0);
    return TRUE;
}

/* a running program: restart it under Tick (or, NT, listen without restarting) */
static void choose_running(DWORD pid, const char *exe)
{
    const prog_entry *sc;
    prog_entry e;
    sbuf b;
    int r;
    if (listener_running()) {
        MessageBoxA(g_hwnd, "Tick is already listening to a program.", "Tick", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!ents) build_list();
    sc = shortcut_for(exe);
    memset(&e, 0, sizeof e);
    if (sc) e = *sc;
    else copy_str(e.target, exe, MAX_PATH);
    sb_init(&b, 600);
    sb_str(&b, "Restart ");
    sb_str(&b, base_of(exe));
    sb_str(&b, " under Tick to record its ports?\n\nIt will be closed and started again");
    sb_str(&b, sc ? " from its shortcut." : ".");
    sb_str(&b, "\n");
    sb_str(&b, exe);
    if (!g_win9x) sb_str(&b, "\n\nYes: restart (recommended).\nNo: listen without restarting; port settings made earlier are read once.");
    r = MessageBoxA(g_hwnd, b.p, "Tick", (g_win9x ? MB_OKCANCEL : MB_YESNOCANCEL) | MB_ICONQUESTION);
    sb_free(&b);
    if (r == IDNO) {
        char err[300];
        if (tick_start_recording() && !attach_process(pid, base_of(exe), err, sizeof err))
            MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR);
        return;
    }
    if (r != IDYES && r != IDOK) return;
    rs_proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!rs_proc && !g_win9x) {
        MessageBoxA(g_hwnd, "Tick may not close that program (it runs as another user or administrator).", "Tick",
                    MB_OK | MB_ICONERROR);
        return;
    }
    rs_pid = pid;
    rs_ent = e;
    rs_deadline = GetTickCount() + 20000;
    EnumWindows(close_windows, (LPARAM)pid);
    tick_show_msg("Closing the program...");
}

/* called every 50 ms: finish a restart when the program has closed */
void programs_timer(void)
{
    int gone;
    if (!rs_pid) return;
    if (rs_proc) gone = WaitForSingleObject(rs_proc, 0) == WAIT_OBJECT_0;
    else {                                          /* Windows 9x without a handle: its windows are gone */
        char p[MAX_PATH];
        process_path(rs_pid, p);
        gone = !p[0];
    }
    if (gone) {
        prog_entry e = rs_ent;
        if (rs_proc) CloseHandle(rs_proc);
        rs_proc = 0;
        rs_pid = 0;
        launch_entry(&e);
        return;
    }
    if (GetTickCount() > rs_deadline) {
        int r;
        sbuf b;
        sb_init(&b, 400);
        sb_str(&b, base_of(rs_ent.target));
        sb_str(&b, " has not closed (is it asking a question?). Close it yourself; Tick then starts it.\n\n"
                   "OK: keep waiting. Cancel: do not restart.");
        rs_deadline = GetTickCount() + 600000;     /* no new box while this one is open */
        r = MessageBoxA(g_hwnd, b.p, "Tick", MB_OKCANCEL | MB_ICONINFORMATION);
        sb_free(&b);
        if (r == IDCANCEL) {
            if (rs_proc) CloseHandle(rs_proc);
            rs_proc = 0;
            rs_pid = 0;
            tick_show_msg("Not restarted.");
            return;
        }
        rs_deadline = GetTickCount() + 60000;
    }
}

/* running copies of a program file (in ents): how many, and the first one's process */
static int running_copies(const char *target, DWORD *pid)
{
    int i, n = 0;
    *pid = 0;
    if (generic(target)) return 0;                  /* cmd.exe /c ..., javaw -jar ...: says nothing about which program */
    for (i = 0; i < nents; i++)
        if (ents[i].running && same_path(ents[i].target, target)) { if (!n) *pid = ents[i].pid; n++; }
    return n;
}

static int several_copies(int n, const char *target)
{
    char m[MAX_PATH + 100];
    if (n < 2) return 0;
    wsprintfA(m, "%d copies of %s are running. Close them, then choose it again.", n, base_of(target));
    MessageBoxA(g_hwnd, m, "Tick", MB_OK | MB_ICONINFORMATION);
    return 1;
}

/* a file chosen with Browse or dropped onto Tick: a program or a shortcut */
void programs_open_file(const char *path)
{
    prog_entry e;
    char err[300];
    int i;
    memset(&e, 0, sizeof e);
    if (listener_running()) {
        MessageBoxA(g_hwnd, "Tick is already listening to a program.", "Tick", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (ends_with(path, ".lnk")) {
        if (!read_shortcut(path, &e, err, sizeof err)) { MessageBoxA(g_hwnd, err, "Tick", MB_OK | MB_ICONERROR); return; }
    } else if (ends_with(path, ".exe")) {
        copy_str(e.target, path, MAX_PATH);
    } else {
        MessageBoxA(g_hwnd, "Not a program (.exe) or a shortcut (.lnk).", "Tick", MB_OK | MB_ICONERROR);
        return;
    }
    /* already running? then it has to be restarted */
    nents = 0;
    if (!ents) ents = rt_alloc(sizeof *ents * MAX_ENTRIES);
    if (ents) EnumWindows(enum_running, 0);
    {
        DWORD pid;
        int n = running_copies(e.target, &pid);
        if (several_copies(n, e.target)) return;
        if (n == 1) {
            build_list();
            if (!shortcut_for(e.target) && nents < MAX_ENTRIES) {   /* restart with the dropped shortcut */
                copy_str(e.label, "dropped", sizeof e.label);
                ents[nents++] = e;
            }
            choose_running(pid, e.target);
            return;
        }
    }
    (void)i;
    launch_entry(&e);
}

void programs_picked(DWORD pid)
{
    char exe[MAX_PATH];
    process_path(pid, exe);
    if (!exe[0]) { MessageBoxA(g_hwnd, "Tick cannot see which program that window belongs to.", "Tick", MB_OK | MB_ICONERROR); return; }
    build_list();
    choose_running(pid, exe);
}

static void browse(HWND owner, const char *start_dir)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH];
    file[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = 76;           /* OPENFILENAME_SIZE_VERSION_400: Windows 95..11 */
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Programs and shortcuts (*.exe, *.lnk)\0*.exe;*.lnk\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Program that talks to the instrument";
    ofn.lpstrInitialDir = start_dir && start_dir[0] ? start_dir : 0;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_NODEREFERENCELINKS;
    if (GetOpenFileNameA(&ofn)) programs_open_file(file);
}

static int action;          /* after the dialog: 1 start entry, 2 pick window, 3 browse */

static void show_detail(HWND d)
{
    HWND lb = GetDlgItem(d, IDC_PLIST);
    LRESULT i = SendMessageA(lb, LB_GETCURSEL, 0, 0);
    sbuf b;
    if (i == LB_ERR) { SetDlgItemTextA(d, IDC_PDETAIL, ""); return; }
    i = SendMessageA(lb, LB_GETITEMDATA, (WPARAM)i, 0);
    sb_init(&b, 600);
    if (ents[i].where[0]) { sb_str(&b, ents[i].where); sb_str(&b, ": "); }
    sb_str(&b, ents[i].target[0] ? ents[i].target : ents[i].install_dir);
    if (ents[i].args[0]) { sb_putc(&b, ' '); sb_str(&b, ents[i].args); }
    SetDlgItemTextA(d, IDC_PDETAIL, b.p);
    sb_free(&b);
}

static INT_PTR CALLBACK programs_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_INITDIALOG) {
        HWND lb = GetDlgItem(d, IDC_PLIST);
        int i, widest = 0;
        for (i = 0; i < nents; i++) {
            LRESULT k = SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)ents[i].label);
            SendMessageA(lb, LB_SETITEMDATA, (WPARAM)k, (LPARAM)i);
            if (lstrlenA(ents[i].label) > widest) widest = lstrlenA(ents[i].label);
        }
        SendMessageA(lb, LB_SETHORIZONTALEXTENT, (WPARAM)(widest * 7), 0);
        if (!nents) SetDlgItemTextA(d, IDC_PDETAIL, "No shortcuts found. Use Browse or Pick window.");
        return TRUE;
    }
    if (m == WM_COMMAND) {
        switch (LOWORD(w)) {
        case IDC_PLIST:
            if (HIWORD(w) == LBN_SELCHANGE) show_detail(d);
            if (HIWORD(w) != LBN_DBLCLK) return TRUE;
            /* fall through: double click = Start */
        case IDOK: {
            LRESULT i = SendMessageA(GetDlgItem(d, IDC_PLIST), LB_GETCURSEL, 0, 0);
            if (i == LB_ERR) return TRUE;
            chosen = (int)SendMessageA(GetDlgItem(d, IDC_PLIST), LB_GETITEMDATA, (WPARAM)i, 0);
            action = 1;
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDC_PPICK: action = 2; EndDialog(d, IDOK); return TRUE;
        case IDC_PBROWSE: action = 3; EndDialog(d, IDOK); return TRUE;
        case IDCANCEL: action = 0; EndDialog(d, IDCANCEL); return TRUE;
        }
    }
    return FALSE;
}

void programs_dialog(HINSTANCE hi)
{
    HCURSOR old;
    if (listener_running()) {
        MessageBoxA(g_hwnd, "Tick is already listening to a program.", "Tick", MB_OK | MB_ICONINFORMATION);
        return;
    }
    old = SetCursor(LoadCursorA(0, (LPCSTR)IDC_WAIT));
    build_list();
    SetCursor(old);
    action = 0;
    chosen = -1;
    DialogBoxParamA(hi, MAKEINTRESOURCEA(IDD_PROGRAMS), g_hwnd, programs_dlg, 0);
    if (action == 1 && chosen >= 0 && chosen < nents) {
        prog_entry e = ents[chosen];
        int i;
        if (e.running) { choose_running(e.pid, e.target); return; }
        if (!e.target[0]) { browse(g_hwnd, e.install_dir); return; }   /* installed, .exe unknown: its folder */
        /* the shortcut's program is running already: restart it (one copy only: never guess) */
        {
            DWORD pid;
            int n = running_copies(e.target, &pid);
            if (several_copies(n, e.target)) return;
            if (n == 1) { choose_running(pid, e.target); return; }
        }
        (void)i;
        launch_entry(&e);
    } else if (action == 2) tick_pick_window(1);
    else if (action == 3) browse(g_hwnd, 0);
}

/* files dropped onto Tick's window (shell32 loaded when Tick starts, if present) */
typedef UINT (WINAPI *DQF)(HDROP, UINT, LPSTR, UINT);
typedef void (WINAPI *DF)(HDROP);
typedef void (WINAPI *DAF)(HWND, BOOL);
static DQF dqf;
static DF dfinish;

void programs_accept_drops(HWND w)
{
    HMODULE sh = LoadLibraryA("shell32.dll");
    DAF daf = sh ? (DAF)(void *)GetProcAddress(sh, "DragAcceptFiles") : 0;
    dqf = sh ? (DQF)(void *)GetProcAddress(sh, "DragQueryFileA") : 0;
    dfinish = sh ? (DF)(void *)GetProcAddress(sh, "DragFinish") : 0;
    if (daf && dqf && dfinish) daf(w, TRUE);        /* shell32 stays loaded for the drops */
}

void programs_drop(HANDLE hd)
{
    HDROP h = (HDROP)hd;
    char path[MAX_PATH];
    if (!dqf || !dfinish) return;
    path[0] = 0;
    dqf(h, 0, path, MAX_PATH);
    dfinish(h);
    SetForegroundWindow(g_hwnd);
    if (path[0]) programs_open_file(path);
}

#else
void programs_timer(void) {}
#endif
