/* TickHook.dll - loaded by Tick into the vendor program to record what the
 * program does on its serial (COM) ports: opens, port settings, bytes
 * written and read.
 *
 * How: the import tables (IAT) of the program's modules are patched so calls
 * to CreateFile/ReadFile/WriteFile/SetCommState/... go through the hooks
 * below; GetProcAddress and LoadLibrary are hooked too, for modules and
 * functions bound late.  Every hook calls the real function first, keeps its
 * result and GetLastError exactly, and only then describes the call in a
 * record in shared memory (ring.h) that Tick reads.  Writing a record never
 * waits: if the ring is full the record is dropped and counted.  If Tick is
 * gone or stopped recording ('active' is 0) the hooks just pass calls through.
 * Nothing is written to disk or registry; everything ends with the process.
 *
 * Adding a family later (network, USB, files): add hook functions and lines
 * to the hooks[] table; the patching code is generic. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/rt.h"
#include "../src/ring.h"

#ifdef _WIN64
#define MY_MACHINE IMAGE_FILE_MACHINE_AMD64     /* TickHook64.dll, for 64-bit programs */
#else
#define MY_MACHINE IMAGE_FILE_MACHINE_I386
#endif

static HINSTANCE self;
static ring_hdr *R;
static int use_qpc, attached, ready;

/* ---------------- originals ---------------- */
typedef HANDLE (WINAPI *CREATEFILEA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CREATEFILEW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *READFILE)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WRITEFILE)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *GETOVERLAPPEDRESULT)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef BOOL (WINAPI *DEVICEIOCONTROL)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *SETCOMMSTATE)(HANDLE, LPDCB);
typedef BOOL (WINAPI *SETCOMMTIMEOUTS)(HANDLE, LPCOMMTIMEOUTS);
typedef BOOL (WINAPI *ESCAPECOMMFUNCTION)(HANDLE, DWORD);
typedef BOOL (WINAPI *PURGECOMM)(HANDLE, DWORD);
typedef BOOL (WINAPI *SETUPCOMM)(HANDLE, DWORD, DWORD);
typedef BOOL (WINAPI *HANDLEFN)(HANDLE);
typedef BOOL (WINAPI *TRANSMITCOMMCHAR)(HANDLE, char);
typedef FARPROC (WINAPI *GETPROCADDRESS)(HMODULE, LPCSTR);
typedef HMODULE (WINAPI *LOADLIBRARYA)(LPCSTR);
typedef HMODULE (WINAPI *LOADLIBRARYW)(LPCWSTR);
typedef HMODULE (WINAPI *LOADLIBRARYEXA)(LPCSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *LOADLIBRARYEXW)(LPCWSTR, HANDLE, DWORD);

static CREATEFILEA o_CreateFileA;
static CREATEFILEW o_CreateFileW;
static READFILE o_ReadFile;
static WRITEFILE o_WriteFile;
static GETOVERLAPPEDRESULT o_GetOverlappedResult;
static DEVICEIOCONTROL o_DeviceIoControl;
static SETCOMMSTATE o_SetCommState;
static SETCOMMTIMEOUTS o_SetCommTimeouts;
static ESCAPECOMMFUNCTION o_EscapeCommFunction;
static PURGECOMM o_PurgeComm;
static SETUPCOMM o_SetupComm;
static HANDLEFN o_SetCommBreak, o_ClearCommBreak, o_CloseHandle;
static TRANSMITCOMMCHAR o_TransmitCommChar;
static GETPROCADDRESS o_GetProcAddress;
static LOADLIBRARYA o_LoadLibraryA;
static LOADLIBRARYW o_LoadLibraryW;
static LOADLIBRARYEXA o_LoadLibraryExA;
static LOADLIBRARYEXW o_LoadLibraryExW;

/* ---------------- records ---------------- */

static void emit(u16 type, HANDLE h, const char *port, u32 a, u32 b, u32 c,
                 const void *p1, u32 n1, const void *p2, u32 n2)
{
    ring_rec rec;
    int i;
    if (!R || !R->active) return;
    memset(&rec, 0, sizeof rec);
    rec.type = type;
    rec.tid = GetCurrentThreadId();
    rec.handle = (u32)(UINT_PTR)h;
    rec.a = a; rec.b = b; rec.c = c;
    for (i = 0; port && port[i] && i < 7; i++) rec.port[i] = port[i];
    if (use_qpc) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        rec.t = (u64)q.QuadPart;
        rec.flags = RECF_QPC;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        rec.t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    if (n1 + n2 <= RING_MAXREC) { ring_put(R, &rec, p1, n1, p2, n2); return; }
    while (n1) {                    /* long transfers: several records */
        u32 k = n1 < RING_MAXREC ? n1 : RING_MAXREC;
        ring_put(R, &rec, p1, k, 0, 0);
        p1 = (const u8 *)p1 + k;
        n1 -= k;
    }
}

static void emit_text(u16 type, const char *s)
{
    emit(type, 0, 0, 0, 0, 0, s, (u32)strlen(s), 0, 0);
}

/* ---------------- COM handle table ---------------- */

#define MAXH 64
static struct { volatile HANDLE h; char port[8]; } ht[MAXH];
static struct { volatile HANDLE h; } nocom[16];     /* attach mode: handles known not to be COM */
static volatile LONG lk;

static void lock(void) { while (__sync_lock_test_and_set(&lk, 1)) Sleep(0); }
static void unlock(void) { __sync_lock_release(&lk); }

static int port_of(HANDLE h, char *port)
{
    int i, j;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; i < MAXH; i++)
        if (ht[i].h == h) {
            for (j = 0; j < 8; j++) port[j] = ht[i].port[j];
            return 1;
        }
    return 0;
}

static void add_handle(HANDLE h, const char *port)
{
    int i, j;
    lock();
    for (i = 0; i < MAXH; i++)
        if (!ht[i].h) {
            for (j = 0; j < 7 && port[j]; j++) ht[i].port[j] = port[j];
            ht[i].port[j] = 0;
            __sync_synchronize();
            ht[i].h = h;
            break;
        }
    unlock();
}

static void remove_handle(HANDLE h)
{
    int i;
    lock();
    for (i = 0; i < MAXH; i++) if (ht[i].h == h) ht[i].h = 0;
    for (i = 0; i < 16; i++) if (nocom[i].h == h) nocom[i].h = 0;
    unlock();
}

/* attached to a running program: COM ports opened before we came are found
 * by asking the handle itself (character device that answers GetCommState) */
static int is_com(HANDLE h, char *port)
{
    static volatile LONG next;
    DCB d;
    int i;
    if (port_of(h, port)) return 1;
    if (!attached || !h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; i < 16; i++) if (nocom[i].h == h) return 0;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (GetFileType(h) == FILE_TYPE_CHAR && GetCommState(h, &d)) {
        port[0] = 'C'; port[1] = 'O'; port[2] = 'M'; port[3] = '?'; port[4] = 0;
        add_handle(h, port);
        emit(REC_OPEN, h, port, 1, 0, 1, "(opened before Tick attached)", 29, 0, 0);
        emit(REC_DCB, h, port, 1, 0, 1, &d, sizeof d, 0, 0);
        return 1;
    }
    nocom[(u32)__sync_fetch_and_add(&next, 1) % 16].h = h;
    return 0;
}

/* "COM3", "\\.\COM12", "com1:" -> "COM3" etc. */
static int parse_com(const char *n, char *port)
{
    int i = 0, k;
    if (!n) return 0;
    if (n[0] == '\\' && n[1] == '\\' && (n[2] == '.' || n[2] == '?') && n[3] == '\\') n += 4;
    if ((n[0] | 32) != 'c' || (n[1] | 32) != 'o' || (n[2] | 32) != 'm') return 0;
    port[0] = 'C'; port[1] = 'O'; port[2] = 'M';
    for (k = 3; n[k] >= '0' && n[k] <= '9' && i < 3; k++, i++) port[3 + i] = n[k];
    port[3 + i] = 0;
    if (!i || n[3] == '0') return 0;
    if (n[k] == ':') k++;
    return n[k] == 0;
}

/* ---------------- pending overlapped reads ---------------- */

#define MAXOV 32
static struct { OVERLAPPED *volatile ov; HANDLE h; void *buf; u32 req; char port[8]; } ovt[MAXOV];

static void ov_add(OVERLAPPED *ov, HANDLE h, void *buf, u32 req, const char *port)
{
    int i, j;
    lock();
    for (i = 0; i < MAXOV; i++)
        if (!ovt[i].ov || ovt[i].ov == ov) {
            ovt[i].h = h; ovt[i].buf = buf; ovt[i].req = req;
            for (j = 0; j < 8; j++) ovt[i].port[j] = port[j];
            ovt[i].ov = ov;
            break;
        }
    unlock();
}

/* finished read on ov? log it (call 3 = GetOverlappedResult, 4 = noticed later) */
static void ov_done(OVERLAPPED *ov, u32 n, u32 call)
{
    int i;
    void *buf = 0;
    HANDLE h = 0;
    u32 req = 0;
    char port[8];
    lock();
    for (i = 0; i < MAXOV; i++)
        if (ovt[i].ov == ov) {
            buf = ovt[i].buf; h = ovt[i].h; req = ovt[i].req;
            memcpy(port, ovt[i].port, 8);
            ovt[i].ov = 0;
            break;
        }
    unlock();
    if (buf && n && n <= req) emit(REC_READ, h, port, req, 0, call, buf, n, 0, 0);
}

/* ---------------- hooks ---------------- */

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    char port[8];
    if (R && R->active && !IsBadStringPtrA(name, 512) && parse_com(name, port)) {
        if (h != INVALID_HANDLE_VALUE) add_handle(h, port);
        emit(REC_OPEN, h, port, h != INVALID_HANDLE_VALUE, h != INVALID_HANDLE_VALUE ? 0 : err, flags,
             name, (u32)strlen(name), 0, 0);
    }
    SetLastError(err);
    return h;
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = o_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    char a[64], port[8];
    int i;
    if (R && R->active && !IsBadReadPtr(name, 2)) {
        for (i = 0; i < 63 && name[i]; i++) a[i] = name[i] < 128 ? (char)name[i] : '?';
        a[i] = 0;
        if (i < 63 && parse_com(a, port)) {
            if (h != INVALID_HANDLE_VALUE) add_handle(h, port);
            emit(REC_OPEN, h, port, h != INVALID_HANDLE_VALUE, h != INVALID_HANDLE_VALUE ? 0 : err, flags,
                 a, (u32)i, 0, 0);
        }
    }
    SetLastError(err);
    return h;
}

static BOOL WINAPI hk_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    char port[8];
    BOOL r;
    DWORD err;
    int com = R && R->active && is_com(h, port);
    if (com && ov) ov_done(ov, ov->Internal != 0x103 ? (u32)ov->InternalHigh : 0, 4);   /* reused ov */
    r = o_ReadFile(h, buf, n, got, ov);
    err = GetLastError();
    if (com) {
        if (r) {
            u32 k = got ? *got : ov ? (u32)ov->InternalHigh : 0;
            if (k && k <= n) emit(REC_READ, h, port, n, 0, 1, buf, k, 0, 0);
        } else if (ov && err == ERROR_IO_PENDING) ov_add(ov, h, buf, n, port);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
    char port[8];
    BOOL r;
    DWORD err;
    int com = R && R->active && is_com(h, port);
    r = o_WriteFile(h, buf, n, put, ov);
    err = GetLastError();
    if (com && n && !IsBadReadPtr(buf, n)) {
        u32 written = r ? (put ? *put : n) : (ov && err == ERROR_IO_PENDING ? 0xffffffffu : 0);
        emit(REC_WRITE, h, port, n, written, r ? 2 : 0, buf, n, 0, 0);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait)
{
    BOOL r = o_GetOverlappedResult(h, ov, n, wait);
    DWORD err = GetLastError();
    if (r && R && R->active && n) ov_done(ov, *n, 3);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD nin, LPVOID out, DWORD nout,
                                      LPDWORD got, LPOVERLAPPED ov)
{
    char port[8];
    BOOL r;
    DWORD err;
    int com = R && R->active && is_com(h, port);
    r = o_DeviceIoControl(h, code, in, nin, out, nout, got, ov);
    err = GetLastError();
    if (com) {
        u32 ni = in && nin <= 4096 && !IsBadReadPtr(in, nin) ? nin : 0;
        u32 no = r && out && got && *got <= nout && *got <= 4096 ? *got : 0;
        emit(REC_IOCTL, h, port, code, ni, no, in, ni, out, no);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_SetCommState(HANDLE h, LPDCB d)
{
    char port[8];
    BOOL r = o_SetCommState(h, d);
    DWORD err = GetLastError();
    if (R && R->active && d && is_com(h, port)) emit(REC_DCB, h, port, r != 0, err, 0, d, sizeof(DCB), 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t)
{
    char port[8];
    BOOL r = o_SetCommTimeouts(h, t);
    DWORD err = GetLastError();
    if (R && R->active && t && is_com(h, port)) emit(REC_TIMEOUTS, h, port, r != 0, 0, 0, t, sizeof *t, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_EscapeCommFunction(HANDLE h, DWORD f)
{
    char port[8];
    BOOL r = o_EscapeCommFunction(h, f);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_ESCAPE, h, port, f, r != 0, 0, 0, 0, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_SetCommBreak(HANDLE h)
{
    char port[8];
    BOOL r = o_SetCommBreak(h);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_ESCAPE, h, port, SETBREAK, r != 0, 0, 0, 0, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_ClearCommBreak(HANDLE h)
{
    char port[8];
    BOOL r = o_ClearCommBreak(h);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_ESCAPE, h, port, CLRBREAK, r != 0, 0, 0, 0, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_PurgeComm(HANDLE h, DWORD f)
{
    char port[8];
    BOOL r = o_PurgeComm(h, f);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_PURGE, h, port, f, r != 0, 0, 0, 0, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_SetupComm(HANDLE h, DWORD in, DWORD out)
{
    char port[8];
    BOOL r = o_SetupComm(h, in, out);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_SETUP, h, port, in, out, 0, 0, 0, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_TransmitCommChar(HANDLE h, char c)
{
    char port[8];
    BOOL r = o_TransmitCommChar(h, c);
    DWORD err = GetLastError();
    if (R && R->active && is_com(h, port)) emit(REC_WRITE, h, port, 1, r ? 1 : 0, 5, &c, 1, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_CloseHandle(HANDLE h)
{
    char port[8];
    int com = port_of(h, port);
    BOOL r = o_CloseHandle(h);
    DWORD err = GetLastError();
    if (com) {
        remove_handle(h);
        emit(REC_CLOSE, h, port, r != 0, 0, 0, 0, 0, 0, 0);
    } else if (attached) remove_handle(h);
    SetLastError(err);
    return r;
}

static void scan_modules(void);
static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name);

static void after_load(HMODULE m)
{
    if (m && ready) scan_modules();
}

static HMODULE WINAPI hk_LoadLibraryA(LPCSTR n)
{
    HMODULE m = o_LoadLibraryA(n);
    DWORD err = GetLastError();
    after_load(m);
    SetLastError(err);
    return m;
}

static HMODULE WINAPI hk_LoadLibraryW(LPCWSTR n)
{
    HMODULE m = o_LoadLibraryW(n);
    DWORD err = GetLastError();
    after_load(m);
    SetLastError(err);
    return m;
}

static HMODULE WINAPI hk_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl)
{
    HMODULE m = o_LoadLibraryExA(n, f, fl);
    DWORD err = GetLastError();
    if (!(fl & (LOAD_LIBRARY_AS_DATAFILE | 0x20 /* AS_IMAGE_RESOURCE */))) after_load(m);
    SetLastError(err);
    return m;
}

static HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl)
{
    HMODULE m = o_LoadLibraryExW(n, f, fl);
    DWORD err = GetLastError();
    if (!(fl & (LOAD_LIBRARY_AS_DATAFILE | 0x20))) after_load(m);
    SetLastError(err);
    return m;
}

/* ---------------- hook table and IAT patching ---------------- */

static struct { const char *name; void *hook; void **orig; } hooks[] = {
    {"CreateFileA", (void *)hk_CreateFileA, (void **)&o_CreateFileA},
    {"CreateFileW", (void *)hk_CreateFileW, (void **)&o_CreateFileW},
    {"ReadFile", (void *)hk_ReadFile, (void **)&o_ReadFile},
    {"WriteFile", (void *)hk_WriteFile, (void **)&o_WriteFile},
    {"GetOverlappedResult", (void *)hk_GetOverlappedResult, (void **)&o_GetOverlappedResult},
    {"DeviceIoControl", (void *)hk_DeviceIoControl, (void **)&o_DeviceIoControl},
    {"SetCommState", (void *)hk_SetCommState, (void **)&o_SetCommState},
    {"SetCommTimeouts", (void *)hk_SetCommTimeouts, (void **)&o_SetCommTimeouts},
    {"EscapeCommFunction", (void *)hk_EscapeCommFunction, (void **)&o_EscapeCommFunction},
    {"SetCommBreak", (void *)hk_SetCommBreak, (void **)&o_SetCommBreak},
    {"ClearCommBreak", (void *)hk_ClearCommBreak, (void **)&o_ClearCommBreak},
    {"PurgeComm", (void *)hk_PurgeComm, (void **)&o_PurgeComm},
    {"SetupComm", (void *)hk_SetupComm, (void **)&o_SetupComm},
    {"TransmitCommChar", (void *)hk_TransmitCommChar, (void **)&o_TransmitCommChar},
    {"CloseHandle", (void *)hk_CloseHandle, (void **)&o_CloseHandle},
    {"GetProcAddress", (void *)hk_GetProcAddress, (void **)&o_GetProcAddress},
    {"LoadLibraryA", (void *)hk_LoadLibraryA, (void **)&o_LoadLibraryA},
    {"LoadLibraryW", (void *)hk_LoadLibraryW, (void **)&o_LoadLibraryW},
    {"LoadLibraryExA", (void *)hk_LoadLibraryExA, (void **)&o_LoadLibraryExA},
    {"LoadLibraryExW", (void *)hk_LoadLibraryExW, (void **)&o_LoadLibraryExW},
};
#define NHOOKS (sizeof hooks / sizeof hooks[0])

static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC r = o_GetProcAddress(m, name);
    DWORD err = GetLastError();
    u32 i;
    if (r && ready)
        for (i = 0; i < NHOOKS; i++)
            if (*hooks[i].orig && (void *)r == *hooks[i].orig) { r = (FARPROC)hooks[i].hook; break; }
    SetLastError(err);
    return r;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static volatile LONG patched_slots;
static volatile LONG scan_lock;

static void patch_module(u8 *base, u32 region)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *d;
    u32 rva, size;
    if (region < 512 || dos->e_magic != IMAGE_DOS_SIGNATURE || (u32)dos->e_lfanew > region - 256) return;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != MY_MACHINE) return;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!rva || !size || rva >= nt->OptionalHeader.SizeOfImage) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); !IsBadReadPtr(d, sizeof *d) && d->Name; d++) {
        UINT_PTR *slot;
        if (!d->FirstThunk || d->FirstThunk >= nt->OptionalHeader.SizeOfImage) continue;
        for (slot = (UINT_PTR *)(base + d->FirstThunk); !IsBadReadPtr(slot, sizeof *slot) && *slot; slot++) {
            u32 i;
            for (i = 0; i < NHOOKS; i++) {
                if (*hooks[i].orig && *slot == (UINT_PTR)*hooks[i].orig) {
                    DWORD old;
                    if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
                        *slot = (UINT_PTR)hooks[i].hook;
                        VirtualProtect(slot, sizeof *slot, old, &old);
                        __sync_fetch_and_add(&patched_slots, 1);
                    } else if (VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &old)) {
                        *slot = (UINT_PTR)hooks[i].hook;
                        VirtualProtect(slot, sizeof *slot, old, &old);
                        __sync_fetch_and_add(&patched_slots, 1);
                    }
                    break;
                }
            }
        }
    }
}

/* walk the address space for loaded modules (works on Windows 9x and NT) */
static void scan_modules(void)
{
    u8 *a = (u8 *)0x10000;
#ifdef _WIN64
    u8 *limit = (u8 *)(UINT_PTR)0x7ffffffeffffull;
#else
    u8 *limit = (GetVersion() & 0x80000000u) ? (u8 *)0x80000000u : (u8 *)0xfffe0000u;
#endif
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH];
    if (__sync_lock_test_and_set(&scan_lock, 1)) return;    /* another thread is scanning */
    while (a < limit && VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
        u8 *next = (u8 *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase && (u8 *)mbi.BaseAddress != (u8 *)self &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            GetModuleFileNameA((HMODULE)mbi.BaseAddress, path, sizeof path)) {
            const char *b = path, *p;
            for (p = path; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
            if (!ieq(b, "kernel32.dll") && !ieq(b, "kernelbase.dll") && !ieq(b, "ntdll.dll"))
                patch_module((u8 *)mbi.BaseAddress, (u32)mbi.RegionSize);
        }
        if (next <= a) break;
        a = next;
    }
    __sync_lock_release(&scan_lock);
}

/* Called by Tick's loader stub right after LoadLibrary (outside the loader
 * lock).  Without Tick's shared ring nothing is hooked at all. */
__declspec(dllexport) void WINAPI TickInit(void)
{
    char name[32], text[300];
    sbuf b;
    HANDLE m;
    HMODULE k;
    LARGE_INTEGER f;
    u32 i;
    if (ready || R) return;
    b.p = 0;
    sb_init(&b, 32);
    sb_str(&b, "TickRing_");
    sb_u32(&b, GetCurrentProcessId());
    for (i = 0; i < 31 && b.p[i]; i++) name[i] = b.p[i];
    name[i] = 0;
    sb_free(&b);
    m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return;
    R = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!R || R->magic != RING_MAGIC || R->version != RING_VERSION) { R = 0; return; }
    attached = R->pad[0] == 2;
    use_qpc = QueryPerformanceFrequency(&f) && f.QuadPart > 1000;
    k = GetModuleHandleA("kernel32.dll");
    for (i = 0; i < NHOOKS; i++) *hooks[i].orig = (void *)GetProcAddress(k, hooks[i].name);
    /* a hook whose original is missing (Windows 95: some W functions) is
     * simply never installed: patch_module only replaces known originals */
    ready = 1;
    scan_modules();
    R->hooked = (u32)patched_slots;
    sb_init(&b, 300);
    sb_str(&b, "TickHook active in ");
    GetModuleFileNameA(0, text, sizeof text);
    sb_str(&b, text);
    sb_str(&b, ": ");
    sb_u32(&b, (u32)patched_slots);
    sb_str(&b, " import slots hooked");
    emit_text(REC_TEXT, b.p);
    sb_free(&b);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) self = h;
    return TRUE;
}
