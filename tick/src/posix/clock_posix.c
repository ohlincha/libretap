/* clock_posix.c - event timestamps on Linux: CLOCK_REALTIME, the PC's own
 * wall clock (the same clock Tick shows). */
#define _GNU_SOURCE
#include "../core.h"
#include <time.h>

void clock_init(void) {}

u64 clock_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000000 + (u64)ts.tv_nsec / 1000;
}

void clock_tm(u64 us, tick_tm *t)
{
    time_t s = (time_t)(us / 1000000);
    struct tm tm;
    localtime_r(&s, &tm);
    t->year = tm.tm_year + 1900; t->mon = tm.tm_mon + 1; t->day = tm.tm_mday;
    t->hour = tm.tm_hour; t->min = tm.tm_min; t->sec = tm.tm_sec;
    t->ms = (int)(us / 1000 % 1000); t->wday = tm.tm_wday;
}

int clock_utc_offset_min(void)
{
    time_t s = time(0);
    struct tm tm;
    localtime_r(&s, &tm);
    return (int)(tm.tm_gmtoff / 60);
}

void clock_iso(sbuf *b, u64 us)
{
    tick_tm t;
    int off = clock_utc_offset_min();
    clock_tm(us, &t);
    sb_u32w(b, (u32)t.year, 4); sb_putc(b, '-');
    sb_u32w(b, (u32)t.mon, 2); sb_putc(b, '-');
    sb_u32w(b, (u32)t.day, 2); sb_putc(b, 'T');
    sb_u32w(b, (u32)t.hour, 2); sb_putc(b, ':');
    sb_u32w(b, (u32)t.min, 2); sb_putc(b, ':');
    sb_u32w(b, (u32)t.sec, 2); sb_putc(b, '.');
    sb_u32w(b, (u32)t.ms, 3);
    sb_putc(b, off < 0 ? '-' : '+');
    if (off < 0) off = -off;
    sb_u32w(b, (u32)off / 60, 2); sb_putc(b, ':');
    sb_u32w(b, (u32)off % 60, 2);
}
