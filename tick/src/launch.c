/* launch.c - "Start a program...": start the vendor program suspended, load
 * TickHook.dll into it, resume it, and turn the hook's records (ring.h) into
 * api-serial events.  Also "listen to a running program" (NT family only).
 *
 * Loading the DLL: the suspended main thread's instruction pointer is moved
 * to a 60-byte stub in the program's memory that calls
 * LoadLibraryA("...TickHook.dll") and TickInit(), restores all registers and
 * jumps to where the thread was going to start.  Works on Windows 2000..11
 * (also 32-bit programs on 64-bit Windows); on Windows 95/98 the stub lives in
 * shared memory (best effort, untested).
 *
 * Tick never terminates or pauses the program afterwards.  When Tick stops
 * or closes, it sets the ring's 'active' flag to 0 and the hooks pass every
 * call straight through. */
#include "tick.h"
#include "ring.h"
#include "resource.h"

static CRITICAL_SECTION lcs;
static int lcs_ready;
static HANDLE hproc, hthread_reader, hmap, hstubmap;
static ring_hdr *ring;
static volatile int stop_reader, running;
static char prog_name[MAX_PATH], prog_path[MAX_PATH];
static char status_text[200];
static u32 last_drops, bytes_in, bytes_out;
static char ports_seen[64];
static u8 *recbuf;

static void lock(void)
{
    if (!lcs_ready) { InitializeCriticalSection(&lcs); lcs_ready = 1; }
    EnterCriticalSection(&lcs);
}
static void unlock(void) { LeaveCriticalSection(&lcs); }

int listener_running(void) { return running; }
const char *listener_status(void) { return status_text; }

static void set_status(void)
{
    sbuf b;
    sb_init(&b, 200);
    if (running) {
        sb_str(&b, "Listening to ");
        sb_str(&b, prog_name);
        if (ports_seen[0]) {
            sb_str(&b, ": ");
            sb_str(&b, ports_seen);
            sb_str(&b, ", ");
            sb_u32(&b, bytes_out);
            sb_str(&b, " bytes out, ");
            sb_u32(&b, bytes_in);
            sb_str(&b, " in");
        } else sb_str(&b, " (no COM port opened yet)");
        if (!session_active()) sb_str(&b, " - paused, not recording");
    } else if (prog_name[0]) {
        sb_str(&b, prog_name);
        sb_str(&b, " has ended.");
    }
    copy_str(status_text, b.p ? b.p : "", sizeof status_text);
    sb_free(&b);
}

/* ---------------- records -> events ---------------- */

static void src_id(char *id, const char *port)
{
    int i;
    for (i = 0; port[i] && i < 7; i++) id[i] = (char)(port[i] >= 'A' && port[i] <= 'Z' ? port[i] + 32 : port[i]);
    id[i] = 0;
    if (!lstrcmpA(id, "com?")) copy_str(id, "com-attached", 16);
}

static void ensure_source(const char *port)
{
    char id[16];
    sbuf b, u;
    src_id(id, port);
    if (session_has_source(id)) return;
    sb_init(&b, 200);
    sb_init(&u, 64);
    ansi_to_utf8(&u, prog_name);
    sb_str(&b, "{\"id\": \"");
    sb_str(&b, id);
    sb_str(&b, "\", \"kind\": \"api-serial\", \"label\": ");
    {
        sbuf l;
        sb_init(&l, 64);
        sb_str(&l, port);
        sb_str(&l, " (");
        sb_mem(&l, u.p, u.len);
        sb_putc(&l, ')');
        sb_json(&b, l.p, l.len);
        sb_free(&l);
    }
    sb_str(&b, ", \"process\": ");
    sb_json(&b, u.p ? u.p : "", u.len);
    sb_putc(&b, '}');
    session_add_source(id, b.p);
    sb_free(&b);
    sb_free(&u);
    if (lstrlenA(ports_seen) + lstrlenA(port) + 3 < (int)sizeof ports_seen) {
        int i, n = lstrlenA(ports_seen), found = 0;
        for (i = 0; i + lstrlenA(port) <= n; i++)
            if (!memcmp(ports_seen + i, port, (u32)lstrlenA(port))) found = 1;
        if (!found) {
            if (n) lstrcatA(ports_seen, " ");
            lstrcatA(ports_seen, port);
        }
    }
}

static const char *call_name(u32 type, u32 c)
{
    if (type == REC_READ) return c == 3 ? "GetOverlappedResult" : c == 4 ? "ReadFile (overlapped, seen late)" : "ReadFile";
    if (type == REC_WRITE) return c == 5 ? "TransmitCommChar" : "WriteFile";
    return "";
}

static void put_onoff(sbuf *b, const char *key, int on)
{
    sb_str(b, ",\"");
    sb_str(b, key);
    sb_str(b, on ? "\":\"on\"" : "\":\"off\"");
}

static void record_to_event(const ring_rec *r, const u8 *p)
{
    sbuf b;
    char id[16];
    u64 t = (r->flags & RECF_QPC) ? clock_qpc_to_us(r->t) : clock_ft_to_us(r->t);
    sb_init(&b, 256 + r->dlen * 2);
    if (r->type == REC_TEXT || r->type == REC_ERROR) {
        ev_begin(&b, t, r->type == REC_TEXT ? "status" : "error");
        sb_str(&b, ",\"text\":");
        sb_json(&b, (const char *)p, r->dlen);
        ev_end(&b);
        sb_free(&b);
        return;
    }
    if (!r->port[0]) { sb_free(&b); return; }
    ensure_source(r->port);
    src_id(id, r->port);
    switch (r->type) {
    case REC_OPEN:
        ev_begin(&b, t, "open");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"path\":");
        sb_json(&b, (const char *)p, r->dlen);
        sb_str(&b, r->a ? ",\"ok\":true" : ",\"ok\":false");
        if (!r->a) { sb_str(&b, ",\"error\":"); sb_u32(&b, r->b); }
        if (r->c & FILE_FLAG_OVERLAPPED) sb_str(&b, ",\"overlapped\":true");
        break;
    case REC_CLOSE:
        ev_begin(&b, t, "close");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id); sb_putc(&b, '"');
        break;
    case REC_READ:
    case REC_WRITE:
        ev_begin(&b, t, "data");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, r->type == REC_READ ? "\",\"dir\":\"in\",\"hex\":\"" : "\",\"dir\":\"out\",\"hex\":\"");
        sb_hexbytes(&b, p, r->dlen);
        sb_str(&b, "\",\"call\":\"");
        sb_str(&b, call_name(r->type, r->c));
        sb_str(&b, "\",\"n\":");
        sb_u32(&b, r->dlen);
        if (r->type == REC_READ) { sb_str(&b, ",\"req\":"); sb_u32(&b, r->a); bytes_in += r->dlen; }
        else {
            bytes_out += r->dlen;
            if (r->b == 0xffffffffu) sb_str(&b, ",\"pending\":true");
            else if (r->b != r->dlen) { sb_str(&b, ",\"written\":"); sb_u32(&b, r->b); }
        }
        break;
    case REC_DCB:
        if (r->dlen >= sizeof(DCB)) {
            const DCB *d = (const DCB *)p;
            static const char *rts[4] = {"off", "on", "handshake", "toggle"};
            static const char *dtr[4] = {"off", "on", "handshake", "?"};
            static const char par[5] = {'N', 'O', 'E', 'M', 'S'};
            ev_begin(&b, t, "config");
            sb_str(&b, ",\"src\":\""); sb_str(&b, id);
            sb_str(&b, "\",\"call\":\"SetCommState\",\"baud\":"); sb_u32(&b, d->BaudRate);
            sb_str(&b, ",\"bits\":"); sb_u32(&b, d->ByteSize);
            sb_str(&b, ",\"parity\":\""); sb_putc(&b, d->Parity < 5 ? par[d->Parity] : '?');
            sb_str(&b, "\",\"stop\":"); sb_str(&b, d->StopBits == 0 ? "1" : d->StopBits == 1 ? "1.5" : "2");
            sb_str(&b, ",\"rts\":\""); sb_str(&b, rts[d->fRtsControl & 3]);
            sb_str(&b, "\",\"dtr\":\""); sb_str(&b, dtr[d->fDtrControl & 3]);
            sb_putc(&b, '"');
            if (d->fOutxCtsFlow) sb_str(&b, ",\"cts_flow\":true");
            if (d->fOutxDsrFlow) sb_str(&b, ",\"dsr_flow\":true");
            if (d->fOutX || d->fInX) {
                sb_str(&b, ",\"xonxoff\":\"");
                sb_str(&b, d->fOutX && d->fInX ? "both" : d->fOutX ? "out" : "in");
                sb_str(&b, "\",\"xon\":"); sb_u32(&b, (u8)d->XonChar);
                sb_str(&b, ",\"xoff\":"); sb_u32(&b, (u8)d->XoffChar);
            }
            if (r->c) sb_str(&b, ",\"current\":true");     /* read from the port when attaching */
            if (!r->a) sb_str(&b, ",\"ok\":false");
        } else { sb_free(&b); return; }
        break;
    case REC_TIMEOUTS:
        if (r->dlen >= sizeof(COMMTIMEOUTS)) {
            const COMMTIMEOUTS *c = (const COMMTIMEOUTS *)p;
            ev_begin(&b, t, "config");
            sb_str(&b, ",\"src\":\""); sb_str(&b, id);
            sb_str(&b, "\",\"call\":\"SetCommTimeouts\",\"timeouts\":{\"read_interval\":");
            sb_u32(&b, c->ReadIntervalTimeout);
            sb_str(&b, ",\"read_total_multiplier\":"); sb_u32(&b, c->ReadTotalTimeoutMultiplier);
            sb_str(&b, ",\"read_total_constant\":"); sb_u32(&b, c->ReadTotalTimeoutConstant);
            sb_str(&b, ",\"write_total_multiplier\":"); sb_u32(&b, c->WriteTotalTimeoutMultiplier);
            sb_str(&b, ",\"write_total_constant\":"); sb_u32(&b, c->WriteTotalTimeoutConstant);
            sb_putc(&b, '}');
        } else { sb_free(&b); return; }
        break;
    case REC_ESCAPE:
        ev_begin(&b, t, "config");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, r->a == SETBREAK || r->a == CLRBREAK ? "\",\"call\":\"SetCommBreak\"" : "\",\"call\":\"EscapeCommFunction\"");
        switch (r->a) {
        case SETDTR: put_onoff(&b, "dtr", 1); break;
        case CLRDTR: put_onoff(&b, "dtr", 0); break;
        case SETRTS: put_onoff(&b, "rts", 1); break;
        case CLRRTS: put_onoff(&b, "rts", 0); break;
        case SETXOFF: sb_str(&b, ",\"xoff_sent\":true"); break;
        case SETXON: sb_str(&b, ",\"xon_sent\":true"); break;
        case SETBREAK: put_onoff(&b, "break", 1); break;
        case CLRBREAK: put_onoff(&b, "break", 0); break;
        default: sb_str(&b, ",\"function\":"); sb_u32(&b, r->a);
        }
        if (!r->b) sb_str(&b, ",\"ok\":false");
        break;
    case REC_PURGE:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"call\":\"PurgeComm\",\"flags\":\"0x"); sb_hex32(&b, r->a, 2); sb_putc(&b, '"');
        break;
    case REC_SETUP:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"call\":\"SetupComm\",\"in_queue\":"); sb_u32(&b, r->a);
        sb_str(&b, ",\"out_queue\":"); sb_u32(&b, r->b);
        break;
    case REC_IOCTL:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"call\":\"DeviceIoControl\",\"code\":\"0x"); sb_hex32(&b, r->a, 8);
        sb_str(&b, "\",\"in\":\"");
        sb_hexbytes(&b, p, r->b <= r->dlen ? r->b : 0);
        sb_str(&b, "\",\"out\":\"");
        if (r->b + r->c <= r->dlen) sb_hexbytes(&b, p + r->b, r->c);
        sb_putc(&b, '"');
        break;
    default:
        sb_free(&b);
        return;
    }
    ev_end(&b);
    sb_free(&b);
}

static void drain(void)
{
    u32 n;
    if (!ring) return;
    while ((n = ring_get(ring, recbuf, RING_MAXREC + 256)) != 0) {
        const ring_rec *r = (const ring_rec *)recbuf;
        if (n >= sizeof(ring_rec) && session_active() && r->dlen <= n - sizeof(ring_rec))
            record_to_event(r, recbuf + sizeof(ring_rec));
    }
    if (ring->drops != last_drops) {
        sbuf b;
        sb_init(&b, 100);
        sb_u32(&b, ring->drops - last_drops);
        sb_str(&b, " listener records lost (Tick could not keep up)");
        last_drops = ring->drops;
        if (session_active()) ev_status("error", b.p);
        sb_free(&b);
    }
}

static void cleanup(void)
{
    if (ring) { ring->active = 0; UnmapViewOfFile(ring); ring = 0; }
    if (hmap) { CloseHandle(hmap); hmap = 0; }
    if (hstubmap) { CloseHandle(hstubmap); hstubmap = 0; }
    if (hproc) { CloseHandle(hproc); hproc = 0; }
}

static DWORD WINAPI reader(LPVOID arg)
{
    DWORD unfinished_since = 0;
    (void)arg;
    for (;;) {
        DWORD w = WaitForSingleObject(hproc, 25);
        u32 tail;
        lock();
        tail = ring ? ring->tail : 0;
        drain();
        /* a record reserved but never finished (writer thread killed): skip it after 2 s */
        if (ring && ring->tail == tail && ring->tail != ring->reserve) {
            if (!unfinished_since) unfinished_since = GetTickCount();
            else if (GetTickCount() - unfinished_since > 2000) { ring_skip(ring); unfinished_since = 0; }
        } else unfinished_since = 0;
        if (w == WAIT_OBJECT_0 || stop_reader) {
            if (w == WAIT_OBJECT_0) {
                DWORD code = 0;
                sbuf b;
                GetExitCodeProcess(hproc, &code);
                sb_init(&b, 300);
                sb_str(&b, prog_name);
                sb_str(&b, " ended (exit code ");
                sb_u32(&b, code);
                sb_putc(&b, ')');
                if (session_active()) ev_status("status", b.p);
                sb_free(&b);
            } else if (session_active()) {
                sbuf b;
                sb_init(&b, 300);
                sb_str(&b, "Tick stopped listening to ");
                sb_str(&b, prog_name);
                sb_str(&b, " (the program keeps running)");
                ev_status("status", b.p);
                sb_free(&b);
            }
            cleanup();
            running = 0;
            set_status();
            unlock();
            PostMessageA(g_hwnd, WM_TICK_LISTENER, 0, 0);
            return 0;
        }
        set_status();
        unlock();
    }
}

void listener_session_changed(void)
{
    lock();
    drain();
    if (ring) ring->active = session_active() ? 1 : 0;
    set_status();
    unlock();
}

void listener_detach_all(void)
{
    HANDLE t;
    if (!hthread_reader) return;
    lock();
    if (ring) ring->active = 0;
    stop_reader = 1;
    t = hthread_reader;
    unlock();
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    hthread_reader = 0;
}

/* ---------------- injection ---------------- */

static int extract_dll(char *path)
{
    HRSRC r = FindResourceA(0, MAKEINTRESOURCEA(IDR_HOOKDLL), (LPCSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(0, r) : 0;
    void *p = g ? LockResource(g) : 0;
    DWORD n = r ? SizeofResource(0, r) : 0;
    HANDLE h;
    DWORD w;
    if (!p || !n) return 0;
    session_mkdir("tick");
    copy_str(path, session_dir(), MAX_PATH);
    lstrcatA(path, "tick\\TickHook.dll");
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        /* still in use by an earlier program of this session: fine if it is there */
        return GetFileAttributesA(path) != 0xffffffffu;
    }
    if (!WriteFile(h, p, n, &w, 0) || w != n) { CloseHandle(h); return 0; }
    CloseHandle(h);
    return 1;
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* code at 0, data at 96: LoadLibraryA, GetProcAddress, return EIP, "TickInit", path */
static u32 build_stub(u8 *s, u32 remote, const char *dll, u32 ret_eip, int thread_proc)
{
    u32 d = remote + 96, i = 0;
    HMODULE k = GetModuleHandleA("kernel32.dll");
    s[i++] = 0x9c;                                  /* pushfd */
    s[i++] = 0x60;                                  /* pushad */
    s[i++] = 0x68; put32(s + i, d + 24); i += 4;    /* push path */
    s[i++] = 0xff; s[i++] = 0x15; put32(s + i, d); i += 4;          /* call [LoadLibraryA] */
    s[i++] = 0x85; s[i++] = 0xc0;                   /* test eax,eax */
    s[i++] = 0x74; s[i++] = 0x12;                   /* jz skip */
    s[i++] = 0x68; put32(s + i, d + 12); i += 4;    /* push "TickInit" */
    s[i++] = 0x50;                                  /* push eax */
    s[i++] = 0xff; s[i++] = 0x15; put32(s + i, d + 4); i += 4;      /* call [GetProcAddress] */
    s[i++] = 0x85; s[i++] = 0xc0;                   /* test eax,eax */
    s[i++] = 0x74; s[i++] = 0x02;                   /* jz skip */
    s[i++] = 0xff; s[i++] = 0xd0;                   /* call eax (TickInit) */
    s[i++] = 0x61;                                  /* skip: popad */
    s[i++] = 0x9d;                                  /* popfd */
    if (thread_proc) {
        s[i++] = 0x31; s[i++] = 0xc0;               /* xor eax,eax */
        s[i++] = 0xc2; s[i++] = 0x04; s[i++] = 0x00; /* ret 4 */
    } else {
        s[i++] = 0xff; s[i++] = 0x25; put32(s + i, d + 8); i += 4;  /* jmp [return EIP] */
    }
    put32(s + 96, (u32)(UINT_PTR)GetProcAddress(k, "LoadLibraryA"));
    put32(s + 100, (u32)(UINT_PTR)GetProcAddress(k, "GetProcAddress"));
    put32(s + 104, ret_eip);
    memcpy(s + 108, "TickInit", 9);
    copy_str((char *)s + 120, dll, MAX_PATH);
    return 120 + (u32)lstrlenA(dll) + 1;
}

typedef LPVOID (WINAPI *VALLOCEX)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
typedef HANDLE (WINAPI *CREMOTETHREAD)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

/* memory for the stub inside the target: VirtualAllocEx on NT, shared
 * memory (visible in every process) on Windows 9x */
static u32 remote_mem(HANDLE proc, u32 size, u8 **local9x)
{
    VALLOCEX va = (VALLOCEX)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "VirtualAllocEx");
    *local9x = 0;
    if (va) return (u32)(UINT_PTR)va(proc, 0, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    hstubmap = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, size, 0);
    if (!hstubmap) return 0;
    *local9x = MapViewOfFile(hstubmap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    return (u32)(UINT_PTR)*local9x;
}

static int write_remote(HANDLE proc, u32 addr, u8 *local9x, const void *p, u32 n)
{
    DWORD w = 0;
    if (local9x) { memcpy(local9x, p, n); return 1; }
    return WriteProcessMemory(proc, (LPVOID)(UINT_PTR)addr, p, n, &w) && w == n;
}

static int make_ring(DWORD pid, int mode)
{
    sbuf b;
    sb_init(&b, 32);
    sb_str(&b, "TickRing_");
    sb_u32(&b, pid);
    hmap = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, sizeof(ring_hdr) + RING_DATA, b.p);
    sb_free(&b);
    if (!hmap) return 0;
    ring = MapViewOfFile(hmap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ring) return 0;
    ring_init(ring, RING_DATA);
    ring->tick_pid = GetCurrentProcessId();
    ring->target_pid = pid;
    ring->pad[0] = (u32)mode;             /* 1 started by Tick, 2 attached */
    ring->active = session_active() ? 1 : 0;
    last_drops = 0;
    bytes_in = bytes_out = 0;
    ports_seen[0] = 0;
    if (!recbuf) recbuf = rt_alloc(RING_MAXREC + 256);
    return recbuf != 0;
}

static void start_reader(void)
{
    DWORD id;
    stop_reader = 0;
    running = 1;
    set_status();
    hthread_reader = CreateThread(0, 0, reader, 0, 0, &id);
    PostMessageA(g_hwnd, WM_TICK_LISTENER, 0, 0);
}

static void base_name(char *out, const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    copy_str(out, b, MAX_PATH);
}

/* copy the program's small settings files (*.ini, *.cfg, ...) into files/ */
static void copy_settings(const char *exe)
{
    static const char *pat[] = {"*.ini", "*.cfg", "*.cnf", "*.conf", "*.set", "*.par", "*.prm"};
    char dir[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    int i, k, n = 0;
    copy_str(dir, exe, MAX_PATH);
    for (k = lstrlenA(dir); k > 0 && dir[k - 1] != '\\'; k--) ;
    dir[k] = 0;
    if (!k) return;
    for (i = 0; i < (int)(sizeof pat / sizeof pat[0]); i++) {
        WIN32_FIND_DATAA fd;
        HANDLE f;
        copy_str(src, dir, MAX_PATH);
        lstrcatA(src, pat[i]);
        f = FindFirstFileA(src, &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.nFileSizeHigh || fd.nFileSizeLow > (1u << 20)) continue;
            if (n == 0) session_mkdir("files");
            copy_str(src, dir, MAX_PATH);
            lstrcatA(src, fd.cFileName);
            copy_str(dst, session_dir(), MAX_PATH);
            lstrcatA(dst, "files\\");
            lstrcatA(dst, fd.cFileName);
            if (CopyFileA(src, dst, TRUE)) n++;
        } while (n < 100 && FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (n) {
        sbuf b;
        sb_init(&b, 100);
        sb_str(&b, "Copied ");
        sb_u32(&b, (u32)n);
        sb_str(&b, " settings file(s) of the program into files/ (before it started)");
        ev_status("status", b.p);
        sb_free(&b);
    }
}

/* 1 if the file is a 32-bit x86 Windows program (the only kind TickHook can enter) */
static int exe_is_i386(const char *exe)
{
    u8 h[1024];
    DWORD n = 0;
    u32 off;
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ReadFile(f, h, sizeof h, &n, 0);
    CloseHandle(f);
    if (n < 64 || h[0] != 'M' || h[1] != 'Z') return 0;
    off = h[60] | h[61] << 8 | h[62] << 16 | (u32)h[63] << 24;
    if (off + 6 > n) return 0;
    return h[off] == 'P' && h[off + 1] == 'E' && h[off + 2] == 0 && h[off + 3] == 0 &&
           h[off + 4] == 0x4c && h[off + 5] == 0x01;
}

int launch_program(const char *exe, const char *args, char *err, u32 errcap)
{
    char dll[MAX_PATH], dir[MAX_PATH];
    sbuf cmd;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    CONTEXT ctx;
    u8 stub[512], *local9x;
    u32 remote, n;
    int k;

    if (running) { copy_str(err, "Tick is already listening to a program.", errcap); return 0; }
    if (!exe_is_i386(exe)) {
        copy_str(err, "This is not a 32-bit Windows program (64-bit, DOS or 16-bit?): Tick cannot listen to it. "
                 "Start it normally; Tick still records the screen.", errcap);
        return 0;
    }
    lock();
    cleanup();
    copy_str(prog_path, exe, MAX_PATH);
    base_name(prog_name, exe);
    if (g_cfg.copy_settings) copy_settings(exe);
    if (!extract_dll(dll)) {
        unlock();
        copy_str(err, "Could not write TickHook.dll into the session folder.", errcap);
        return 0;
    }
    copy_str(dir, exe, MAX_PATH);
    for (k = lstrlenA(dir); k > 0 && dir[k - 1] != '\\'; k--) ;
    dir[k > 0 ? k - 1 : 0] = 0;
    if (k == 3) { dir[2] = '\\'; dir[3] = 0; }      /* "C:\" */
    sb_init(&cmd, 512);
    sb_putc(&cmd, '"');
    sb_str(&cmd, exe);
    sb_putc(&cmd, '"');
    if (args && *args) { sb_putc(&cmd, ' '); sb_str(&cmd, args); }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(0, cmd.p, 0, 0, FALSE, CREATE_SUSPENDED, 0, dir[0] ? dir : 0, &si, &pi)) {
        sb_free(&cmd);
        unlock();
        copy_str(err, "Could not start the program.", errcap);
        return 0;
    }
    sb_free(&cmd);
    hproc = pi.hProcess;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!make_ring(pi.dwProcessId, 1) || !GetThreadContext(pi.hThread, &ctx) ||
        !(remote = remote_mem(pi.hProcess, 512, &local9x))) {
        ResumeThread(pi.hThread);           /* run it anyway, just without listening */
        CloseHandle(pi.hThread);
        cleanup();
        unlock();
        copy_str(err, "The program was started, but Tick could not listen to it.", errcap);
        return 0;
    }
    memset(stub, 0, sizeof stub);
    n = build_stub(stub, remote, dll, ctx.Eip, 0);
    if (!write_remote(pi.hProcess, remote, local9x, stub, n)) {
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        cleanup();
        unlock();
        copy_str(err, "The program was started, but Tick could not listen to it.", errcap);
        return 0;
    }
    ctx.Eip = remote;
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!SetThreadContext(pi.hThread, &ctx)) {
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        cleanup();
        unlock();
        copy_str(err, "The program was started, but Tick could not listen to it.", errcap);
        return 0;
    }
    {
        sbuf b, u;
        sb_init(&b, 300);
        sb_init(&u, 300);
        ansi_to_utf8(&u, exe);
        sb_str(&b, "Started ");
        sb_mem(&b, u.p, u.len);
        sb_str(&b, " (process ");
        sb_u32(&b, pi.dwProcessId);
        sb_str(&b, ") and listening to its serial ports");
        ev_status("status", b.p);
        sb_free(&b);
        sb_free(&u);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    unlock();
    start_reader();
    return 1;
}

int attach_process(DWORD pid, const char *name, char *err, u32 errcap)
{
    CREMOTETHREAD crt = (CREMOTETHREAD)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateRemoteThread");
    char dll[MAX_PATH];
    u8 stub[512], *local9x;
    u32 remote, n;
    HANDLE t;
    if (g_win9x || !crt) { copy_str(err, "Listening to a running program needs Windows NT, 2000 or later.", errcap); return 0; }
    if (running) { copy_str(err, "Tick is already listening to a program.", errcap); return 0; }
    lock();
    cleanup();
    copy_str(prog_name, name, MAX_PATH);
    prog_path[0] = 0;
    hproc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                        PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!hproc) { unlock(); copy_str(err, "Could not open that program (not allowed?).", errcap); return 0; }
    if (!extract_dll(dll) || !make_ring(pid, 2) || !(remote = remote_mem(hproc, 512, &local9x))) {
        cleanup();
        unlock();
        copy_str(err, "Could not prepare listening to that program.", errcap);
        return 0;
    }
    memset(stub, 0, sizeof stub);
    n = build_stub(stub, remote, dll, 0, 1);
    if (!write_remote(hproc, remote, local9x, stub, n) ||
        !(t = crt(hproc, 0, 0, (LPTHREAD_START_ROUTINE)(UINT_PTR)remote, 0, 0, 0))) {
        cleanup();
        unlock();
        copy_str(err, "Could not load the listener into that program (64-bit program?).", errcap);
        return 0;
    }
    CloseHandle(t);
    {
        sbuf b;
        sb_init(&b, 300);
        sb_str(&b, "Listening to running program ");
        sb_str(&b, name);
        sb_str(&b, " (process ");
        sb_u32(&b, pid);
        sb_str(&b, ")");
        ev_status("status", b.p);
        sb_free(&b);
    }
    unlock();
    start_reader();
    return 1;
}
