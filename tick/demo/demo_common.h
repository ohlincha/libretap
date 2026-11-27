/* Shared by DemoMeter.exe and DemoInstrument.exe: a pretend instrument protocol (text lines ending in CR) for
 * trying Tick on a test PC or VM. No C runtime (Windows 95/98/2000/XP and later). */
#ifndef DEMO_COMMON_H
#define DEMO_COMMON_H
#include <winsock2.h>
#include <windows.h>

#define DEMO_PORT 5025

/* the compiler may emit these */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);

static __attribute__((unused)) int d_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static __attribute__((unused)) int d_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static __attribute__((unused)) int d_atoi(const char *s) { int v = 0; while (*s == ' ') s++; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

/* sine without a C runtime: range reduction and a Taylor series (ample for a demo signal) */
static __attribute__((unused)) double d_sin(double x)
{
    const double pi = 3.14159265358979323846;
    double t, s, x2;
    int k;
    while (x > pi) x -= 2 * pi;
    while (x < -pi) x += 2 * pi;
    x2 = x * x; t = x; s = x;
    for (k = 1; k < 10; k++) { t *= -x2 / ((2 * k) * (2 * k + 1)); s += t; }
    return s;
}

/* "0.5418" from a value in volts (4 decimals) */
static __attribute__((unused)) void d_volts(char *out, double v)
{
    int neg = v < 0, i, n;
    if (neg) v = -v;
    n = (int)(v * 10000 + 0.5);
    i = wsprintfA(out, "%s%d.", neg ? "-" : "", n / 10000);
    wsprintfA(out + i, "%04d", n % 10000);
}
#endif
