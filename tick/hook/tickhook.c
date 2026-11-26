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
#include <winsock2.h>
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

/* which kinds Tick asked to leave out (Advanced) */
static int left_out(u32 kind) { return R && (R->pad[2] & kind); }

static volatile LONG dev_counter;

/* a device path worth listening to (USB, HID, a vendor driver): "\\?\usb#...",
 * "\\.\SomeDriver"; not COM ports, pipes, disks, volumes or files */
static int device_path(const char *n)
{
    const char *p;
    static const char *skip[] = {"pipe\\", "mailslot\\", "physicaldrive", "harddisk", "volume", "cdrom", "tape",
                                 "globalroot", "unc\\", "con", "conin$", "conout$", 0};
    int i;
    if (!(n[0] == '\\' && n[1] == '\\' && (n[2] == '.' || n[2] == '?') && n[3] == '\\')) return 0;
    p = n + 4;
    if (p[0] && p[1] == ':') return 0;                       /* \\?\C:\... is a file */
    for (i = 0; skip[i]; i++) {
        const char *a = p, *b = skip[i];
        while (*b && (*a | 32) == *b) { a++; b++; }
        if (!*b && (skip[i][strlen(skip[i]) - 1] == '\\' || !*a || *a == '\\')) return 0;
    }
    return 1;
}

static void dev_port(char *port)
{
    u32 n = (u32)__sync_add_and_fetch(&dev_counter, 1), i = 1, k = 0;
    char d[8];
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 6);
    port[0] = 'u';
    while (k) port[i++] = d[--k];
    port[i] = 0;
}

static void after_create(HANDLE h, const char *name, u32 len, DWORD err, DWORD flags)
{
    char port[8];
    if (parse_com(name, port)) {
        if (left_out(KIND_SERIAL)) return;
    } else if (device_path(name)) {
        if (left_out(KIND_USB)) return;
        dev_port(port);
    } else return;
    if (h != INVALID_HANDLE_VALUE) add_handle(h, port);
    emit(REC_OPEN, h, port, h != INVALID_HANDLE_VALUE, h != INVALID_HANDLE_VALUE ? 0 : err, flags, name, len, 0, 0);
}

/* ---------------- hooks ---------------- */
static void nov_done(LPWSAOVERLAPPED ov, u32 n);

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    if (R && R->active && !IsBadStringPtrA(name, 512)) after_create(h, name, (u32)strlen(name), err, flags);
    SetLastError(err);
    return h;
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = o_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    char a[300];
    int i;
    if (R && R->active && !IsBadReadPtr(name, 2)) {
        for (i = 0; i < 299 && name[i]; i++) a[i] = name[i] < 128 ? (char)name[i] : '?';
        a[i] = 0;
        if (i < 299) after_create(h, a, (u32)i, err, flags);
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
    if (r && R && R->active && n) { ov_done(ov, *n, 3); nov_done((LPWSAOVERLAPPED)ov, *n); }
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

/* ---------------- network (Winsock) ---------------- */

typedef int (WINAPI *CONNECT_)(SOCKET, const struct sockaddr *, int);
typedef SOCKET (WINAPI *ACCEPT_)(SOCKET, struct sockaddr *, int *);
typedef int (WINAPI *SEND_)(SOCKET, const char *, int, int);
typedef int (WINAPI *RECV_)(SOCKET, char *, int, int);
typedef int (WINAPI *SENDTO_)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WINAPI *RECVFROM_)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WINAPI *WSASEND_)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *WSARECV_)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *WSASENDTO_)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *WSARECVFROM_)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef BOOL (WINAPI *WSAGETOVL_)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
typedef int (WINAPI *CLOSESOCKET_)(SOCKET);
typedef int (WINAPI *GETNAME_)(SOCKET, struct sockaddr *, int *);
typedef int (WINAPI *GETSOCKOPT_)(SOCKET, int, int, char *, int *);

static CONNECT_ o_connect;
static ACCEPT_ o_accept;
static SEND_ o_send;
static RECV_ o_recv;
static SENDTO_ o_sendto;
static RECVFROM_ o_recvfrom;
static WSASEND_ o_WSASend;
static WSARECV_ o_WSARecv;
static WSASENDTO_ o_WSASendTo;
static WSARECVFROM_ o_WSARecvFrom;
static WSAGETOVL_ o_WSAGetOverlappedResult;
static CLOSESOCKET_ o_closesocket;
static GETNAME_ o_getsockname, o_getpeername;     /* used, not hooked */
static GETSOCKOPT_ o_getsockopt;

#define MAXS 128
static struct { volatile SOCKET s; char port[8]; tick_net info; } st[MAXS];
static struct { volatile SOCKET s; } nonet[16];
static volatile LONG net_counter;

static int inet_family(int f) { return f == 2 || f == 23; }     /* AF_INET, AF_INET6 */

static void copy_addr(u8 *dst, const struct sockaddr *a, int len)
{
    int n = len < 28 ? len : 28, i;
    for (i = 0; i < 28; i++) dst[i] = 0;
    if (a && n > 0 && !IsBadReadPtr(a, (UINT_PTR)n)) for (i = 0; i < n; i++) dst[i] = ((const u8 *)a)[i];
}

/* the socket's table entry; tracked on first use if it is a TCP/UDP socket */
static int sock_entry(SOCKET s, const struct sockaddr *remote_hint, int hint_len)
{
    u8 la[28], ra[28];
    int i, ln = 28, rn = 28, type = 0, tl = sizeof type;
    if (s == INVALID_SOCKET || left_out(KIND_NET)) return -1;
    for (i = 0; i < MAXS; i++) if (st[i].s == s) return i;
    for (i = 0; i < 16; i++) if (nonet[i].s == s) return -1;
    if (!o_getsockname) return -1;
    for (i = 0; i < 28; i++) la[i] = ra[i] = 0;
    if (o_getsockname(s, (struct sockaddr *)la, &ln) != 0 || !inet_family(*(u16 *)la)) {
        static volatile LONG next;
        if (!remote_hint || !inet_family(((const struct sockaddr *)remote_hint)->sa_family)) {
            nonet[(u32)__sync_fetch_and_add(&next, 1) % 16].s = s;
            return -1;
        }
    }
    if (o_getpeername && o_getpeername(s, (struct sockaddr *)ra, &rn) != 0) for (i = 0; i < 28; i++) ra[i] = 0;
    if (remote_hint && !ra[0] && !ra[1]) copy_addr(ra, remote_hint, hint_len);
    if (o_getsockopt) o_getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &tl);
    lock();
    for (i = 0; i < MAXS; i++)
        if (!st[i].s) {
            u32 n = (u32)__sync_add_and_fetch(&net_counter, 1), k = 0, j = 1;
            char d[8];
            do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 6);
            st[i].port[0] = 'n';
            while (k) st[i].port[j++] = d[--k];
            st[i].port[j] = 0;
            st[i].info.proto = type == SOCK_DGRAM ? 17 : 6;
            for (k = 0; k < 28; k++) { st[i].info.local[k] = la[k]; st[i].info.remote[k] = ra[k]; }
            __sync_synchronize();
            st[i].s = s;
            break;
        }
    unlock();
    return i < MAXS ? i : -1;
}

/* refresh the local/remote addresses (after connect/accept) */
static void sock_refresh(int e)
{
    int ln = 28, rn = 28;
    u8 a[28];
    if (o_getsockname && o_getsockname(st[e].s, (struct sockaddr *)a, &ln) == 0) copy_addr(st[e].info.local, (struct sockaddr *)a, ln);
    if (o_getpeername && o_getpeername(st[e].s, (struct sockaddr *)a, &rn) == 0) copy_addr(st[e].info.remote, (struct sockaddr *)a, rn);
}

static void net_emit(u16 type, int e, u32 call, u32 a, u32 b, const struct sockaddr *peer, int peerlen,
                     const void *data, u32 n)
{
    tick_net ni = st[e].info;
    if (peer) copy_addr(ni.remote, peer, peerlen);
    emit(type, (HANDLE)(UINT_PTR)st[e].s, st[e].port, a, b, call, &ni, sizeof ni, data, n);
}

/* scatter/gather buffers -> one or more records */
static void net_emit_bufs(u16 type, int e, u32 call, LPWSABUF b, DWORD nb, u32 total, u32 sent,
                          const struct sockaddr *peer, int peerlen)
{
    u8 tmp[4096];
    u32 used = 0, k;
    for (k = 0; k < nb && total; k++) {
        const u8 *p = (const u8 *)b[k].buf;
        u32 len = b[k].len < total ? b[k].len : total;
        total -= len;
        if (!p || IsBadReadPtr(p, len)) continue;
        while (len) {
            u32 c = len < sizeof tmp - used ? len : (u32)sizeof tmp - used;
            memcpy(tmp + used, p, c);
            used += c; p += c; len -= c;
            if (used == sizeof tmp) { net_emit(type, e, call, used, sent, peer, peerlen, tmp, used); used = 0; }
        }
    }
    if (used) net_emit(type, e, call, used, sent, peer, peerlen, tmp, used);
}

static int WINAPI hk_connect(SOCKET s, const struct sockaddr *a, int n)
{
    int r = o_connect(s, a, n);
    DWORD err = GetLastError();
    int e;
    if (R && R->active && (e = sock_entry(s, a, n)) >= 0) {
        int ok = r == 0 || err == WSAEWOULDBLOCK;
        if (r == 0) sock_refresh(e);
        copy_addr(st[e].info.remote, a, n);
        net_emit(REC_NET_OPEN, e, CALL_CONNECT, (u32)ok, r == 0 ? 0 : err, 0, 0, 0, 0);
    }
    SetLastError(err);
    return r;
}

static SOCKET WINAPI hk_accept(SOCKET s, struct sockaddr *a, int *n)
{
    SOCKET r = o_accept(s, a, n);
    DWORD err = GetLastError();
    int e;
    if (r != INVALID_SOCKET && R && R->active && (e = sock_entry(r, 0, 0)) >= 0) {
        sock_refresh(e);
        net_emit(REC_NET_OPEN, e, CALL_ACCEPT, 1, 0, 0, 0, 0, 0);
    }
    SetLastError(err);
    return r;
}

static int WINAPI hk_send(SOCKET s, const char *buf, int n, int fl)
{
    int r = o_send(s, buf, n, fl);
    DWORD err = GetLastError();
    int e;
    if (n > 0 && R && R->active && (e = sock_entry(s, 0, 0)) >= 0 && !IsBadReadPtr(buf, (UINT_PTR)n))
        net_emit(REC_NET_SEND, e, CALL_SEND, (u32)n, r < 0 ? 0 : (u32)r, 0, 0, buf, (u32)n);
    SetLastError(err);
    return r;
}

static int WINAPI hk_recv(SOCKET s, char *buf, int n, int fl)
{
    int r = o_recv(s, buf, n, fl);
    DWORD err = GetLastError();
    int e;
    if (r > 0 && R && R->active && (e = sock_entry(s, 0, 0)) >= 0)
        net_emit(REC_NET_RECV, e, CALL_RECV, (u32)n, 0, 0, 0, buf, (u32)r);
    SetLastError(err);
    return r;
}

static int WINAPI hk_sendto(SOCKET s, const char *buf, int n, int fl, const struct sockaddr *to, int tl)
{
    int r = o_sendto(s, buf, n, fl, to, tl);
    DWORD err = GetLastError();
    int e;
    if (n > 0 && R && R->active && (e = sock_entry(s, to, tl)) >= 0 && !IsBadReadPtr(buf, (UINT_PTR)n))
        net_emit(REC_NET_SEND, e, CALL_SENDTO, (u32)n, r < 0 ? 0 : (u32)r, to, tl, buf, (u32)n);
    SetLastError(err);
    return r;
}

static int WINAPI hk_recvfrom(SOCKET s, char *buf, int n, int fl, struct sockaddr *from, int *fl2)
{
    int r = o_recvfrom(s, buf, n, fl, from, fl2);
    DWORD err = GetLastError();
    int e;
    if (r > 0 && R && R->active && (e = sock_entry(s, 0, 0)) >= 0)
        net_emit(REC_NET_RECV, e, CALL_RECVFROM, (u32)n, 0, from, from && fl2 ? *fl2 : 0, buf, (u32)r);
    SetLastError(err);
    return r;
}

/* overlapped WSARecv/WSARecvFrom still pending */
#define MAXNOV 32
static struct { LPWSAOVERLAPPED volatile ov; SOCKET s; LPWSABUF b; DWORD nb; u32 call; struct sockaddr *from; LPINT fromlen; } nov[MAXNOV];

static void nov_add(LPWSAOVERLAPPED ov, SOCKET s, LPWSABUF b, DWORD nb, u32 call, struct sockaddr *from, LPINT fl)
{
    int i;
    lock();
    for (i = 0; i < MAXNOV; i++)
        if (!nov[i].ov || nov[i].ov == ov) {
            nov[i].s = s; nov[i].b = b; nov[i].nb = nb; nov[i].call = call; nov[i].from = from; nov[i].fromlen = fl;
            nov[i].ov = ov;
            break;
        }
    unlock();
}

/* a pending receive finished with n bytes: the WSABUF array must still be valid (it usually is) */
static void nov_done(LPWSAOVERLAPPED ov, u32 n)
{
    int i, e;
    lock();
    for (i = 0; i < MAXNOV; i++) if (nov[i].ov == ov) break;
    if (i == MAXNOV) { unlock(); return; }
    nov[i].ov = 0;
    unlock();
    if (n && (e = sock_entry(nov[i].s, 0, 0)) >= 0 && !IsBadReadPtr(nov[i].b, nov[i].nb * sizeof(WSABUF)))
        net_emit_bufs(REC_NET_RECV, e, nov[i].call, nov[i].b, nov[i].nb, n, 0, nov[i].from,
                      nov[i].from && nov[i].fromlen && !IsBadReadPtr(nov[i].fromlen, 4) ? *nov[i].fromlen : 0);
}

static int WINAPI hk_WSASend(SOCKET s, LPWSABUF b, DWORD nb, LPDWORD sent, DWORD fl, LPWSAOVERLAPPED ov,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSASend(s, b, nb, sent, fl, ov, cr);
    DWORD err = GetLastError();
    int e;
    if (R && R->active && b && nb && (e = sock_entry(s, 0, 0)) >= 0) {
        u32 total = 0, k;
        for (k = 0; k < nb; k++) total += b[k].len;
        net_emit_bufs(REC_NET_SEND, e, CALL_WSASEND, b, nb, total, r == 0 && sent ? *sent : 0xffffffffu, 0, 0);
    }
    SetLastError(err);
    return r;
}

static int WINAPI hk_WSASendTo(SOCKET s, LPWSABUF b, DWORD nb, LPDWORD sent, DWORD fl, const struct sockaddr *to,
                               int tl, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSASendTo(s, b, nb, sent, fl, to, tl, ov, cr);
    DWORD err = GetLastError();
    int e;
    if (R && R->active && b && nb && (e = sock_entry(s, to, tl)) >= 0) {
        u32 total = 0, k;
        for (k = 0; k < nb; k++) total += b[k].len;
        net_emit_bufs(REC_NET_SEND, e, CALL_WSASENDTO, b, nb, total, r == 0 && sent ? *sent : 0xffffffffu, to, tl);
    }
    SetLastError(err);
    return r;
}

static int WINAPI hk_WSARecv(SOCKET s, LPWSABUF b, DWORD nb, LPDWORD got, LPDWORD fl, LPWSAOVERLAPPED ov,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSARecv(s, b, nb, got, fl, ov, cr);
    DWORD err = GetLastError();
    int e;
    if (R && R->active && b && nb && (e = sock_entry(s, 0, 0)) >= 0) {
        if (r == 0 && got && *got) net_emit_bufs(REC_NET_RECV, e, CALL_WSARECV, b, nb, *got, 0, 0, 0);
        else if (r != 0 && err == WSA_IO_PENDING && ov && !cr) nov_add(ov, s, b, nb, CALL_WSARECV, 0, 0);
    }
    SetLastError(err);
    return r;
}

static int WINAPI hk_WSARecvFrom(SOCKET s, LPWSABUF b, DWORD nb, LPDWORD got, LPDWORD fl, struct sockaddr *from,
                                 LPINT fromlen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSARecvFrom(s, b, nb, got, fl, from, fromlen, ov, cr);
    DWORD err = GetLastError();
    int e;
    if (R && R->active && b && nb && (e = sock_entry(s, 0, 0)) >= 0) {
        if (r == 0 && got && *got)
            net_emit_bufs(REC_NET_RECV, e, CALL_WSARECVFROM, b, nb, *got, 0, from, from && fromlen ? *fromlen : 0);
        else if (r != 0 && err == WSA_IO_PENDING && ov && !cr) nov_add(ov, s, b, nb, CALL_WSARECVFROM, from, fromlen);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD n, BOOL wait, LPDWORD fl)
{
    BOOL r = o_WSAGetOverlappedResult(s, ov, n, wait, fl);
    DWORD err = GetLastError();
    if (r && n && R && R->active) nov_done(ov, *n);
    SetLastError(err);
    return r;
}

static int WINAPI hk_closesocket(SOCKET s)
{
    int i, e = -1;
    int r;
    DWORD err;
    for (i = 0; i < MAXS; i++) if (st[i].s == s) { e = i; break; }
    if (e >= 0 && R && R->active) net_emit(REC_NET_CLOSE, e, CALL_CLOSESOCKET, 0, 0, 0, 0, 0, 0);
    r = o_closesocket(s);
    err = GetLastError();
    if (e >= 0) { lock(); st[e].s = 0; unlock(); }
    for (i = 0; i < 16; i++) if (nonet[i].s == s) nonet[i].s = 0;
    SetLastError(err);
    return r;
}

/* ---------------- USB: HID and WinUSB ---------------- */

typedef BOOLEAN (WINAPI *HIDBUF_)(HANDLE, PVOID, ULONG);
#pragma pack(push, 1)
typedef struct { u8 RequestType, Request; u16 Value, Index, Length; } tk_setup;
#pragma pack(pop)
typedef BOOL (WINAPI *WUINIT_)(HANDLE, PVOID *);
typedef BOOL (WINAPI *WUCTRL_)(PVOID, tk_setup, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
typedef BOOL (WINAPI *WUPIPE_)(PVOID, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
typedef BOOL (WINAPI *WUFREE_)(PVOID);

static HIDBUF_ o_HidD_GetFeature, o_HidD_SetFeature, o_HidD_GetInputReport, o_HidD_SetOutputReport;
static WUINIT_ o_WinUsb_Initialize;
static WUCTRL_ o_WinUsb_ControlTransfer;
static WUPIPE_ o_WinUsb_ReadPipe, o_WinUsb_WritePipe;
static WUFREE_ o_WinUsb_Free;

/* port of a device handle; a device opened before Tick attached gets one now */
static int usb_port(HANDLE h, char *port)
{
    if (left_out(KIND_USB) || !h || h == INVALID_HANDLE_VALUE) return 0;
    if (port_of(h, port)) return port[0] == 'u';
    dev_port(port);
    add_handle(h, port);
    emit(REC_OPEN, h, port, 1, 0, 0, "(device opened before Tick saw it)", 34, 0, 0);
    return 1;
}

static BOOLEAN hid_call(HIDBUF_ f, u32 call, int in, HANDLE h, PVOID buf, ULONG n)
{
    BOOLEAN r = f(h, buf, n);
    DWORD err = GetLastError();
    char port[8];
    if (R && R->active && buf && n && usb_port(h, port) && !IsBadReadPtr(buf, n))
        emit(REC_USB_XFER, h, port, r != 0, in ? 0x80u : 0, call, buf, in && !r ? 0 : n, 0, 0);   /* failed read: no data */
    SetLastError(err);
    return r;
}

static BOOLEAN WINAPI hk_HidD_GetFeature(HANDLE h, PVOID b, ULONG n) { return hid_call(o_HidD_GetFeature, CALL_HID_GETFEATURE, 1, h, b, n); }
static BOOLEAN WINAPI hk_HidD_SetFeature(HANDLE h, PVOID b, ULONG n) { return hid_call(o_HidD_SetFeature, CALL_HID_SETFEATURE, 0, h, b, n); }
static BOOLEAN WINAPI hk_HidD_GetInputReport(HANDLE h, PVOID b, ULONG n) { return hid_call(o_HidD_GetInputReport, CALL_HID_GETINPUT, 1, h, b, n); }
static BOOLEAN WINAPI hk_HidD_SetOutputReport(HANDLE h, PVOID b, ULONG n) { return hid_call(o_HidD_SetOutputReport, CALL_HID_SETOUTPUT, 0, h, b, n); }

/* WinUSB interface handle -> the device's port */
static struct { PVOID volatile ih; HANDLE dev; } wu[16];

static int wu_port(PVOID ih, char *port, HANDLE *dev)
{
    int i;
    for (i = 0; i < 16; i++)
        if (wu[i].ih == ih) { *dev = wu[i].dev; return usb_port(wu[i].dev, port); }
    return 0;
}

static BOOL WINAPI hk_WinUsb_Initialize(HANDLE dev, PVOID *ih)
{
    BOOL r = o_WinUsb_Initialize(dev, ih);
    DWORD err = GetLastError();
    char port[8];
    if (r && ih && R && R->active && usb_port(dev, port)) {
        int i;
        lock();
        for (i = 0; i < 16; i++) if (!wu[i].ih) { wu[i].dev = dev; wu[i].ih = *ih; break; }
        unlock();
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WinUsb_ControlTransfer(PVOID ih, tk_setup sp, PUCHAR buf, ULONG n, PULONG done, LPOVERLAPPED ov)
{
    BOOL r = o_WinUsb_ControlTransfer(ih, sp, buf, n, done, ov);
    DWORD err = GetLastError();
    char port[8];
    HANDLE dev;
    if (R && R->active && wu_port(ih, port, &dev)) {
        u32 k = (sp.RequestType & 0x80) ? (r && done ? *done : 0) : n;    /* in: what arrived; out: what was sent */
        if (k > n) k = n;
        if (buf && k && IsBadReadPtr(buf, k)) k = 0;
        emit(REC_USB_CTRL, dev, port, r != 0, r && done ? *done : 0, CALL_WINUSB_CONTROL, &sp, 8, buf, k);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WinUsb_ReadPipe(PVOID ih, UCHAR pipe, PUCHAR buf, ULONG n, PULONG done, LPOVERLAPPED ov)
{
    BOOL r = o_WinUsb_ReadPipe(ih, pipe, buf, n, done, ov);
    DWORD err = GetLastError();
    char port[8];
    HANDLE dev;
    if (R && R->active && wu_port(ih, port, &dev)) {
        u32 k = r && done ? *done : 0;
        if (k > n) k = n;
        if (r) emit(REC_USB_XFER, dev, port, 1, pipe, CALL_WINUSB_READ, buf, k, 0, 0);
        else if (err != ERROR_IO_PENDING) emit(REC_USB_XFER, dev, port, 0, pipe, CALL_WINUSB_READ, 0, 0, 0, 0);
    }
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WinUsb_WritePipe(PVOID ih, UCHAR pipe, PUCHAR buf, ULONG n, PULONG done, LPOVERLAPPED ov)
{
    BOOL r = o_WinUsb_WritePipe(ih, pipe, buf, n, done, ov);
    DWORD err = GetLastError();
    char port[8];
    HANDLE dev;
    if (R && R->active && wu_port(ih, port, &dev) && buf && n && !IsBadReadPtr(buf, n))
        emit(REC_USB_XFER, dev, port, r || err == ERROR_IO_PENDING, pipe, CALL_WINUSB_WRITE, buf, n, 0, 0);
    SetLastError(err);
    return r;
}

static BOOL WINAPI hk_WinUsb_Free(PVOID ih)
{
    BOOL r = o_WinUsb_Free(ih);
    DWORD err = GetLastError();
    int i;
    for (i = 0; i < 16; i++) if (wu[i].ih == ih) wu[i].ih = 0;
    SetLastError(err);
    return r;
}

/* ---------------- hook table and IAT patching ---------------- */

static struct { const char *dll; const char *name; void *hook; void **orig; } hooks[] = {
    {"kernel32.dll", "CreateFileA", (void *)hk_CreateFileA, (void **)&o_CreateFileA},
    {"kernel32.dll", "CreateFileW", (void *)hk_CreateFileW, (void **)&o_CreateFileW},
    {"kernel32.dll", "ReadFile", (void *)hk_ReadFile, (void **)&o_ReadFile},
    {"kernel32.dll", "WriteFile", (void *)hk_WriteFile, (void **)&o_WriteFile},
    {"kernel32.dll", "GetOverlappedResult", (void *)hk_GetOverlappedResult, (void **)&o_GetOverlappedResult},
    {"kernel32.dll", "DeviceIoControl", (void *)hk_DeviceIoControl, (void **)&o_DeviceIoControl},
    {"kernel32.dll", "SetCommState", (void *)hk_SetCommState, (void **)&o_SetCommState},
    {"kernel32.dll", "SetCommTimeouts", (void *)hk_SetCommTimeouts, (void **)&o_SetCommTimeouts},
    {"kernel32.dll", "EscapeCommFunction", (void *)hk_EscapeCommFunction, (void **)&o_EscapeCommFunction},
    {"kernel32.dll", "SetCommBreak", (void *)hk_SetCommBreak, (void **)&o_SetCommBreak},
    {"kernel32.dll", "ClearCommBreak", (void *)hk_ClearCommBreak, (void **)&o_ClearCommBreak},
    {"kernel32.dll", "PurgeComm", (void *)hk_PurgeComm, (void **)&o_PurgeComm},
    {"kernel32.dll", "SetupComm", (void *)hk_SetupComm, (void **)&o_SetupComm},
    {"kernel32.dll", "TransmitCommChar", (void *)hk_TransmitCommChar, (void **)&o_TransmitCommChar},
    {"kernel32.dll", "CloseHandle", (void *)hk_CloseHandle, (void **)&o_CloseHandle},
    {"kernel32.dll", "GetProcAddress", (void *)hk_GetProcAddress, (void **)&o_GetProcAddress},
    {"kernel32.dll", "LoadLibraryA", (void *)hk_LoadLibraryA, (void **)&o_LoadLibraryA},
    {"kernel32.dll", "LoadLibraryW", (void *)hk_LoadLibraryW, (void **)&o_LoadLibraryW},
    {"kernel32.dll", "LoadLibraryExA", (void *)hk_LoadLibraryExA, (void **)&o_LoadLibraryExA},
    {"kernel32.dll", "LoadLibraryExW", (void *)hk_LoadLibraryExW, (void **)&o_LoadLibraryExW},
    /* network: Winsock 2 (wsock32.dll forwards these to ws2_32.dll on NT) */
    {"ws2_32.dll", "connect", (void *)hk_connect, (void **)&o_connect},
    {"ws2_32.dll", "accept", (void *)hk_accept, (void **)&o_accept},
    {"ws2_32.dll", "send", (void *)hk_send, (void **)&o_send},
    {"ws2_32.dll", "recv", (void *)hk_recv, (void **)&o_recv},
    {"ws2_32.dll", "sendto", (void *)hk_sendto, (void **)&o_sendto},
    {"ws2_32.dll", "recvfrom", (void *)hk_recvfrom, (void **)&o_recvfrom},
    {"ws2_32.dll", "WSASend", (void *)hk_WSASend, (void **)&o_WSASend},
    {"ws2_32.dll", "WSARecv", (void *)hk_WSARecv, (void **)&o_WSARecv},
    {"ws2_32.dll", "WSASendTo", (void *)hk_WSASendTo, (void **)&o_WSASendTo},
    {"ws2_32.dll", "WSARecvFrom", (void *)hk_WSARecvFrom, (void **)&o_WSARecvFrom},
    {"ws2_32.dll", "WSAGetOverlappedResult", (void *)hk_WSAGetOverlappedResult, (void **)&o_WSAGetOverlappedResult},
    {"ws2_32.dll", "closesocket", (void *)hk_closesocket, (void **)&o_closesocket},
    /* USB: HID and WinUSB (device handles themselves come from CreateFile) */
    {"hid.dll", "HidD_GetFeature", (void *)hk_HidD_GetFeature, (void **)&o_HidD_GetFeature},
    {"hid.dll", "HidD_SetFeature", (void *)hk_HidD_SetFeature, (void **)&o_HidD_SetFeature},
    {"hid.dll", "HidD_GetInputReport", (void *)hk_HidD_GetInputReport, (void **)&o_HidD_GetInputReport},
    {"hid.dll", "HidD_SetOutputReport", (void *)hk_HidD_SetOutputReport, (void **)&o_HidD_SetOutputReport},
    {"winusb.dll", "WinUsb_Initialize", (void *)hk_WinUsb_Initialize, (void **)&o_WinUsb_Initialize},
    {"winusb.dll", "WinUsb_ControlTransfer", (void *)hk_WinUsb_ControlTransfer, (void **)&o_WinUsb_ControlTransfer},
    {"winusb.dll", "WinUsb_ReadPipe", (void *)hk_WinUsb_ReadPipe, (void **)&o_WinUsb_ReadPipe},
    {"winusb.dll", "WinUsb_WritePipe", (void *)hk_WinUsb_WritePipe, (void **)&o_WinUsb_WritePipe},
    {"winusb.dll", "WinUsb_Free", (void *)hk_WinUsb_Free, (void **)&o_WinUsb_Free},
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

/* originals of the hooked functions, from their DLLs once those are loaded
 * (ws2_32/wsock32, hid, winusb are often loaded late) */
static void resolve_originals(void)
{
    u32 i;
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    if (!ws) ws = GetModuleHandleA("wsock32.dll");       /* Windows 95 without Winsock 2 */
    for (i = 0; i < NHOOKS; i++) {
        HMODULE m;
        if (*hooks[i].orig) continue;
        m = !strcmp(hooks[i].dll, "ws2_32.dll") ? ws : GetModuleHandleA(hooks[i].dll);
        if (m) *hooks[i].orig = (void *)GetProcAddress(m, hooks[i].name);
    }
    if (ws && !o_getsockname) {
        o_getsockname = (GETNAME_)(void *)GetProcAddress(ws, "getsockname");
        o_getpeername = (GETNAME_)(void *)GetProcAddress(ws, "getpeername");
        o_getsockopt = (GETSOCKOPT_)(void *)GetProcAddress(ws, "getsockopt");
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
    resolve_originals();
    while (a < limit && VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
        u8 *next = (u8 *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase && (u8 *)mbi.BaseAddress != (u8 *)self &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            GetModuleFileNameA((HMODULE)mbi.BaseAddress, path, sizeof path)) {
            const char *b = path, *p;
            for (p = path; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
            /* not the system DLLs that implement hooked functions themselves (their own
             * internal calls would be recorded twice) */
            if (!ieq(b, "kernel32.dll") && !ieq(b, "kernelbase.dll") && !ieq(b, "ntdll.dll") &&
                !ieq(b, "ws2_32.dll") && !ieq(b, "mswsock.dll") && !ieq(b, "hid.dll") && !ieq(b, "winusb.dll"))
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
    (void)k;
    resolve_originals();
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
