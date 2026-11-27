/* plat_win.c - Windows side of the platform layer (core.h) */
#include "tick.h"
#include <tlhelp32.h>

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

void *pf_create(const char *path, int must_be_new)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, must_be_new ? CREATE_NEW : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    return h == INVALID_HANDLE_VALUE ? 0 : (void *)h;
}

int pf_write(void *f, const void *p, u32 n)
{
    DWORD w = 0;
    return WriteFile((HANDLE)f, p, n, &w, 0) && w == n;
}

void pf_flush(void *f) { FlushFileBuffers((HANDLE)f); }
void pf_close(void *f) { CloseHandle((HANDLE)f); }

int p_mkdir(const char *path)
{
    DWORD a;
    CreateDirectoryA(path, 0);
    a = GetFileAttributesA(path);
    return a != 0xffffffffu && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int p_mkdir_new(const char *path)
{
    if (CreateDirectoryA(path, 0)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

void p_rmdir(const char *path) { RemoveDirectoryA(path); }
void p_unlink(const char *path) { DeleteFileA(path); }
u32 p_ms(void) { return GetTickCount(); }

void p_hostname(char *out, u32 cap)
{
    char host[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD hn = sizeof host;
    if (!GetComputerNameA(host, &hn)) host[0] = 0;
    copy_str(out, host, cap);
}

void p_os(sbuf *b)
{
    OSVERSIONINFOA ov;
    int i;
    sb_str(b, "Windows ");
    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    if (!GetVersionExA(&ov)) return;
    if (ov.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        /* Windows 8.1 and later report 6.2 to programs without a compatibility manifest: ask ntdll for the real
         * version (RtlGetVersion: Windows 2000 and later, loaded by name; absent on 9x/NT 4, then keep 6.2-style) */
        typedef LONG (WINAPI *RGV)(OSVERSIONINFOW *);
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        RGV rgv = nt ? (RGV)(void *)GetProcAddress(nt, "RtlGetVersion") : 0;
        OSVERSIONINFOW w;
        memset(&w, 0, sizeof w);
        w.dwOSVersionInfoSize = sizeof w;
        if (rgv && rgv(&w) == 0) { ov.dwMajorVersion = w.dwMajorVersion; ov.dwMinorVersion = w.dwMinorVersion; ov.dwBuildNumber = w.dwBuildNumber; }
    }
    sb_u32(b, ov.dwMajorVersion); sb_putc(b, '.'); sb_u32(b, ov.dwMinorVersion);
    sb_str(b, " build ");
    sb_u32(b, ov.dwPlatformId == VER_PLATFORM_WIN32_NT ? ov.dwBuildNumber : (ov.dwBuildNumber & 0xffff));
    if (ov.szCSDVersion[0] && ov.szCSDVersion[0] != ' ') {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, ov.szCSDVersion);
        sb_putc(b, ' ');
        for (i = 0; i < (int)u.len; i++) if (u.p[i] != '"' && u.p[i] != '\\') sb_putc(b, u.p[i]);
        sb_free(&u);
    }
}

static CRITICAL_SECTION cs;
static int cs_ready;

void p_lock(void)
{
    if (!cs_ready) { InitializeCriticalSection(&cs); cs_ready = 1; }
    EnterCriticalSection(&cs);
}

void p_unlock(void) { LeaveCriticalSection(&cs); }

int p_documents(char *out, u32 cap)
{
    typedef BOOL (WINAPI *SHGSFP)(HWND, LPSTR, int, BOOL);
    HMODULE m = LoadLibraryA("shell32.dll");
    SHGSFP f = m ? (SHGSFP)(void *)GetProcAddress(m, "SHGetSpecialFolderPathA") : 0;
    int ok = 0;
    (void)cap;
    if (f && f(0, out, 5 /* CSIDL_PERSONAL */, FALSE)) ok = 1;
    if (m) FreeLibrary(m);
    return ok;
}

int p_temp(char *out, u32 cap)
{
    return GetTempPathA(cap, out) != 0;
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
