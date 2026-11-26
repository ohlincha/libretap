/* TickHelper64.exe - loads TickHook64.dll into a 64-bit program for Tick.
 *
 * Tick.exe is 32-bit (it must run on Windows 95/2000) and cannot load a
 * 64-bit DLL into a 64-bit program itself.  On 64-bit Windows it unpacks this
 * helper and TickHook64.dll into the session's tick\ folder and runs:
 *   TickHelper64.exe inject PID TID DLLPATH   program created suspended by Tick:
 *       point thread TID at a stub that loads the DLL, calls TickInit and
 *       continues where the thread was going to start (Tick then resumes it)
 *   TickHelper64.exe attach PID DLLPATH       running program: remote thread
 * Exit code 0 = done.  The helper does nothing else and exits at once; the
 * DLL then writes into the shared ring Tick created (same layout as 32-bit).
 * No C runtime, like Tick. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/rt.h"

static const char *arg(const char *p, char *out, u32 cap)
{
    u32 n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (n + 1 < cap) out[n++] = *p; p++; }
        if (*p) p++;
    } else
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < cap) out[n++] = *p; p++; }
    out[n] = 0;
    return p;
}

static u32 num(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0');
    return v;
}

static void put64(u8 *p, u64 v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i));
}

/* x86-64 stub: save volatile registers, align the stack, LoadLibraryA(dll),
 * GetProcAddress(h, "TickInit")(), restore, then jump to ret_rip (or return
 * 0 when used as a thread procedure).  Data after the code at +256. */
static u32 build_stub(u8 *s, u64 remote, const char *dll, u64 ret_rip, int thread_proc)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    u64 path = remote + 256 + 16, name = remote + 256;
    u32 i = 0, n;
    static const u8 save[] = {0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
                              0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0x83, 0xec, 0x20};
    static const u8 restore[] = {0x48, 0x89, 0xec, 0x5d, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                                 0x5a, 0x59, 0x58, 0x9d};
    memcpy(s, save, sizeof save); i = sizeof save;
    s[i++] = 0x48; s[i++] = 0xb9; put64(s + i, path); i += 8;                     /* mov rcx, path */
    s[i++] = 0x48; s[i++] = 0xb8; put64(s + i, (u64)(UINT_PTR)GetProcAddress(k, "LoadLibraryA")); i += 8;
    s[i++] = 0xff; s[i++] = 0xd0;                                                  /* call rax */
    s[i++] = 0x48; s[i++] = 0x85; s[i++] = 0xc0;                                   /* test rax, rax */
    s[i++] = 0x74; s[i++] = 0x20;                                                  /* jz skip (+32) */
    s[i++] = 0x48; s[i++] = 0x89; s[i++] = 0xc1;                                   /* mov rcx, rax */
    s[i++] = 0x48; s[i++] = 0xba; put64(s + i, name); i += 8;                      /* mov rdx, "TickInit" */
    s[i++] = 0x48; s[i++] = 0xb8; put64(s + i, (u64)(UINT_PTR)GetProcAddress(k, "GetProcAddress")); i += 8;
    s[i++] = 0xff; s[i++] = 0xd0;                                                  /* call rax */
    s[i++] = 0x48; s[i++] = 0x85; s[i++] = 0xc0;                                   /* test rax, rax */
    s[i++] = 0x74; s[i++] = 0x02;                                                  /* jz skip */
    s[i++] = 0xff; s[i++] = 0xd0;                                                  /* call rax (TickInit) */
    memcpy(s + i, restore, sizeof restore); i += sizeof restore;                   /* skip: */
    if (thread_proc) {
        s[i++] = 0x31; s[i++] = 0xc0;                                              /* xor eax, eax */
        s[i++] = 0xc3;                                                             /* ret */
    } else {
        s[i++] = 0xff; s[i++] = 0x25; s[i++] = 0; s[i++] = 0; s[i++] = 0; s[i++] = 0;  /* jmp [rip+0] */
        put64(s + i, ret_rip); i += 8;
    }
    memcpy(s + 256, "TickInit", 9);
    n = (u32)strlen(dll);
    memcpy(s + 256 + 16, dll, n + 1);
    return 256 + 16 + n + 1;
}

static u32 run(void)
{
    char cmd[16], a[32], b[32], dll[MAX_PATH];
    const char *p = arg(GetCommandLineA(), dll, sizeof dll);     /* skip our own name */
    u8 stub[1024];
    HANDLE proc, th = 0;
    u8 *remote;
    u32 n, pid, tid = 0;
    SIZE_T w = 0;
    int inject;

    p = arg(p, cmd, sizeof cmd);
    inject = !lstrcmpA(cmd, "inject");
    if (!inject && lstrcmpA(cmd, "attach")) return 2;
    p = arg(p, a, sizeof a);
    pid = num(a);
    if (inject) { p = arg(p, b, sizeof b); tid = num(b); }
    arg(p, dll, sizeof dll);
    if (!pid || !dll[0] || (inject && !tid)) return 2;

    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                       PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 3;
    remote = VirtualAllocEx(proc, 0, sizeof stub, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) return 4;
    memset(stub, 0, sizeof stub);
    if (inject) {
        CONTEXT ctx;
        th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) return 5;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(th, &ctx)) return 6;
        n = build_stub(stub, (u64)(UINT_PTR)remote, dll, ctx.Rip, 0);
        if (!WriteProcessMemory(proc, remote, stub, n, &w) || w != n) return 7;
        FlushInstructionCache(proc, remote, n);
        ctx.Rip = (u64)(UINT_PTR)remote;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!SetThreadContext(th, &ctx)) return 8;
        CloseHandle(th);
    } else {
        n = build_stub(stub, (u64)(UINT_PTR)remote, dll, 0, 1);
        if (!WriteProcessMemory(proc, remote, stub, n, &w) || w != n) return 7;
        FlushInstructionCache(proc, remote, n);
        th = CreateRemoteThread(proc, 0, 0, (LPTHREAD_START_ROUTINE)(void *)remote, 0, 0, 0);
        if (!th) return 9;
        CloseHandle(th);
    }
    CloseHandle(proc);
    return 0;
}

void HelperMain(void)
{
    ExitProcess(run());
}
