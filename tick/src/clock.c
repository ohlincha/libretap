/* clock.c - event timestamps: the PC's wall clock (GetSystemTimeAsFileTime),
 * smoothed with QueryPerformanceCounter when there is one.  The QPC estimate
 * is snapped back to the wall clock whenever they differ by more than 150 ms,
 * so Tick's clock always shows (and records) the PC's own time. */
#include "tick.h"

#define FT_UNIX_EPOCH 116444736000000000ull     /* 1970-01-01 in FILETIME units */
#define SNAP_US 150000

static CRITICAL_SECTION cs;
static int have_qpc;
static u64 qfreq, base_q, base_us;

static u64 ft_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

u64 clock_ft_to_us(u64 ft)
{
    return ft > FT_UNIX_EPOCH ? rt_udiv64(ft - FT_UNIX_EPOCH, 10, 0) : 0;
}

static u64 q_to_dus(u64 dq)      /* QPC ticks -> microseconds without overflow */
{
    u64 r, s = rt_udiv64(dq, qfreq, &r);
    return s * 1000000 + rt_udiv64(r * 1000000, qfreq, 0);
}

void clock_init(void)
{
    LARGE_INTEGER f, q;
    InitializeCriticalSection(&cs);
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 1000 && QueryPerformanceCounter(&q)) {
        have_qpc = 1;
        qfreq = (u64)f.QuadPart;
        base_q = (u64)q.QuadPart;
        base_us = clock_ft_to_us(ft_now());
    }
}

u64 clock_qpc_to_us(u64 qpc)
{
    u64 us;
    if (!have_qpc) return clock_ft_to_us(ft_now());
    EnterCriticalSection(&cs);
    us = qpc >= base_q ? base_us + q_to_dus(qpc - base_q) : base_us - q_to_dus(base_q - qpc);
    LeaveCriticalSection(&cs);
    return us;
}

u64 clock_now_us(void)
{
    u64 sys = clock_ft_to_us(ft_now()), est;
    LARGE_INTEGER q;
    if (!have_qpc || !QueryPerformanceCounter(&q)) return sys;
    EnterCriticalSection(&cs);
    est = base_us + q_to_dus((u64)q.QuadPart - base_q);
    if (est > sys + SNAP_US || sys > est + SNAP_US) {
        base_q = (u64)q.QuadPart;
        base_us = est = sys;
    }
    LeaveCriticalSection(&cs);
    return est;
}

void clock_local(u64 us, SYSTEMTIME *st)
{
    u64 v = us * 10 + FT_UNIX_EPOCH;
    FILETIME ft, lft;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    if (!FileTimeToLocalFileTime(&ft, &lft)) lft = ft;
    FileTimeToSystemTime(&lft, st);
}

int clock_utc_offset_min(void)
{
    u64 v = ft_now(), lv;
    FILETIME ft, lft;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    if (!FileTimeToLocalFileTime(&ft, &lft)) return 0;
    lv = ((u64)lft.dwHighDateTime << 32) | lft.dwLowDateTime;
    if (lv >= v) return (int)rt_udiv64(lv - v + 300000000, 600000000, 0);
    return -(int)rt_udiv64(v - lv + 300000000, 600000000, 0);
}

void clock_iso(sbuf *b, u64 us)
{
    SYSTEMTIME st;
    int off = clock_utc_offset_min();
    clock_local(us, &st);
    sb_u32w(b, st.wYear, 4); sb_putc(b, '-');
    sb_u32w(b, st.wMonth, 2); sb_putc(b, '-');
    sb_u32w(b, st.wDay, 2); sb_putc(b, 'T');
    sb_u32w(b, st.wHour, 2); sb_putc(b, ':');
    sb_u32w(b, st.wMinute, 2); sb_putc(b, ':');
    sb_u32w(b, st.wSecond, 2); sb_putc(b, '.');
    sb_u32w(b, st.wMilliseconds, 3);
    sb_putc(b, off < 0 ? '-' : '+');
    if (off < 0) off = -off;
    sb_u32w(b, (u32)off / 60, 2); sb_putc(b, ':');
    sb_u32w(b, (u32)off % 60, 2);
}
