/* libtickhook.so - Tick's serial listener for Linux programs (LD_PRELOAD).
 *
 * `tick run PROGRAM` starts the program with LD_PRELOAD=libtickhook.so and
 * TICK_RING=<shared memory file>.  This library defines open/openat/read/
 * write/readv/writev/ioctl/tcsetattr/tcflush/tcsendbreak/close (and the
 * _FORTIFY_SOURCE variants); for terminal devices (serial ports, USB serial
 * adapters, pseudo-terminals) it describes each call in a record in the
 * shared ring (src/ring.h, the same one Tick for Windows uses).
 *
 * Rules as on Windows: the real work is done first and its result and errno
 * are returned unchanged; writing a record never waits (full ring: dropped
 * and counted); if Tick is gone or not recording ('active' 0) calls just pass
 * through.  Nothing is written to disk.
 *
 * The real calls are made as raw system calls (x86-64), so the library needs
 * nothing from libc but getenv and __errno_location: it loads into programs
 * built for any glibc (or musl) and does not depend on dlsym's location.
 * Limits: statically linked and setuid programs ignore LD_PRELOAD; 32-bit
 * programs cannot load this 64-bit library (they run, unrecorded); data moved
 * through stdio (fopen/fread/fprintf on a port) and io_uring is not seen. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

extern char *getenv(const char *);
extern int *__errno_location(void);

/* no libc string functions (their symbol versions differ between glibc releases) */
static void *tk_memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
static void *tk_memset(void *d, int c, size_t n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
#define memcpy tk_memcpy
#define memset tk_memset
#define TICK_RT_H                                   /* ring.h's rt.h is replaced by the lines above */
#define RT_NOINCLUDE
#include "../src/ring.h"
#include "../src/ring.c"

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static long ret(long r)             /* libc convention: -1 and errno */
{
    if (r < 0 && r > -4096) { *__errno_location() = (int)-r; return -1; }
    return r;
}

static ring_hdr *R;
static volatile int init_done;

#define MAXFD 1024
static char ports[MAXFD][8];        /* "t3": short id of a tracked tty, "" = not tracked */

static void init(void)
{
    const char *p;
    long fd, m;
    if (__sync_lock_test_and_set(&init_done, 1)) return;
    p = getenv("TICK_RING");
    if (!p || !*p) return;
    fd = sc(SYS_open, (long)p, O_RDWR | O_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return;
    m = sc(SYS_mmap, 0, sizeof(ring_hdr) + RING_DATA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (m < 0 && m > -4096) return;
    if (((ring_hdr *)m)->magic != RING_MAGIC || ((ring_hdr *)m)->version != RING_VERSION) {
        sc(SYS_munmap, m, sizeof(ring_hdr) + RING_DATA, 0, 0, 0, 0);
        return;
    }
    R = (ring_hdr *)m;
}

__attribute__((constructor)) static void ctor(void) { init(); }

static int on(void)
{
    if (!init_done) init();
    return R && R->active;
}

static void emit(u16 type, int fd, const char *port, u32 a, u32 b, u32 c, const void *p1, u32 n1, const void *p2, u32 n2)
{
    ring_rec rec;
    struct { long s, ns; } ts;
    u32 i;
    if (!R || !R->active) return;
    tk_memset(&rec, 0, sizeof rec);
    rec.type = type;
    rec.flags = RECF_UNIXUS;
    rec.tid = (u32)sc(SYS_gettid, 0, 0, 0, 0, 0, 0);
    rec.handle = (u32)fd;
    rec.a = a; rec.b = b; rec.c = c;
    for (i = 0; port && port[i] && i < 7; i++) rec.port[i] = port[i];
    sc(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, (long)&ts, 0, 0, 0, 0);
    rec.t = (u64)ts.s * 1000000 + (u64)ts.ns / 1000;
    if (n1 + n2 <= RING_MAXREC) { ring_put(R, &rec, p1, n1, p2, n2); return; }
    while (n1) {
        u32 k = n1 < RING_MAXREC ? n1 : RING_MAXREC;
        ring_put(R, &rec, p1, k, 0, 0);
        p1 = (const u8 *)p1 + k;
        n1 -= k;
    }
}

static int tracked(int fd, char *port)
{
    if (fd < 0 || fd >= MAXFD || !ports[fd][0]) return 0;
    tk_memcpy(port, ports[fd], 8);
    return 1;
}

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *a, const char *b) { while (*b && *a == *b) { a++; b++; } return !*b; }
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* a serial-like device name worth noting even when the open fails */
static int serial_name(const char *p)
{
    return starts(p, "/dev/tty") && !streq(p, "/dev/tty") ? 1 : starts(p, "/dev/serial/") || starts(p, "/dev/pts/") ||
           starts(p, "/dev/cu") || starts(p, "/dev/rfcomm");
}

static void after_open(const char *path, long fd, int flags)
{
    struct { u32 f[4]; u8 line, cc[19]; } k;
    char port[8];
    u32 n;
    int err;
    if (!on() || !path) return;
    if (fd < 0) {
        if (serial_name(path)) emit(REC_OPEN, -1, "t?", 0, (u32)*__errno_location(), (u32)flags, path, slen(path), 0, 0);
        return;
    }
    if (fd >= MAXFD || streq(path, "/dev/tty") || streq(path, "/dev/console") || streq(path, "/dev/ptmx")) return;
    err = *__errno_location();
    if (sc(SYS_ioctl, fd, TCGETS, (long)&k, 0, 0, 0) < 0) { *__errno_location() = err; return; }  /* not a tty */
    n = __sync_add_and_fetch(&R->pad[1], 1);
    port[0] = 't';
    {
        char d[8];
        int i = 0, j = 1;
        do { d[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 6);
        while (i) port[j++] = d[--i];
        port[j] = 0;
    }
    tk_memcpy(ports[fd], port, 8);
    emit(REC_OPEN, (int)fd, port, 1, 0, (u32)flags, path, slen(path), 0, 0);
    *__errno_location() = err;
}

static u32 baud_of(u32 cflag)
{
    static const u32 low[16] = {0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400};
    static const u32 high[16] = {0, 57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000, 1152000,
                                 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};
    u32 b = cflag & 0x100f;     /* CBAUD */
    return b & 0x1000 ? high[b & 15] : low[b & 15];
}

static void emit_termios(int fd, const char *port, const void *kt, int ok, u32 call)
{
    const struct { u32 f[4]; u8 line, cc[19]; } *k = kt;
    tick_termios t;
    tk_memset(&t, 0, sizeof t);
    t.iflag = k->f[0]; t.oflag = k->f[1]; t.cflag = k->f[2]; t.lflag = k->f[3];
    t.ispeed = t.ospeed = baud_of(k->f[2]);
    tk_memcpy(t.cc, k->cc, 19);
    emit(REC_TERMIOS, fd, port, (u32)ok, 0, call, &t, sizeof t, 0, 0);
}

/* ---------------- the wrapped functions ---------------- */

static int open_mode(int flags, va_list ap) { return (flags & (O_CREAT | O_TMPFILE)) ? va_arg(ap, int) : 0; }

int open(const char *path, int flags, ...)
{
    va_list ap;
    long fd;
    int mode;
    va_start(ap, flags);
    mode = open_mode(flags, ap);
    va_end(ap);
    fd = ret(sc(SYS_openat, AT_FDCWD, (long)path, flags, mode, 0, 0));
    after_open(path, fd, flags);
    return (int)fd;
}

int openat(int dir, const char *path, int flags, ...)
{
    va_list ap;
    long fd;
    int mode;
    va_start(ap, flags);
    mode = open_mode(flags, ap);
    va_end(ap);
    fd = ret(sc(SYS_openat, dir, (long)path, flags, mode, 0, 0));
    after_open(path, fd, flags);
    return (int)fd;
}

int open64(const char *path, int flags, ...) __attribute__((alias("open")));
int openat64(int dir, const char *path, int flags, ...) __attribute__((alias("openat")));
int __open_2(const char *path, int flags) { return open(path, flags); }
int __open64_2(const char *path, int flags) { return open(path, flags); }
int __openat_2(int dir, const char *path, int flags) { return openat(dir, path, flags); }
int __openat64_2(int dir, const char *path, int flags) { return openat(dir, path, flags); }

ssize_t read(int fd, void *buf, size_t n)
{
    long r = ret(sc(SYS_read, fd, (long)buf, (long)n, 0, 0, 0));
    char port[8];
    if (r > 0 && tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit(REC_READ, fd, port, (u32)n, 0, 1, buf, (u32)r, 0, 0);
        *__errno_location() = e;
    }
    return r;
}

ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen) { (void)buflen; return read(fd, buf, n); }

ssize_t write(int fd, const void *buf, size_t n)
{
    long r = ret(sc(SYS_write, fd, (long)buf, (long)n, 0, 0, 0));
    char port[8];
    if (n && tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit(REC_WRITE, fd, port, (u32)n, r < 0 ? 0 : (u32)r, 2, buf, (u32)n, 0, 0);
        *__errno_location() = e;
    }
    return r;
}

static void emit_iov(u16 type, int fd, const char *port, const struct iovec *iov, int cnt, long total, u32 call)
{
    u8 tmp[4096];
    u32 used = 0;
    int i;
    for (i = 0; i < cnt && total > 0; i++) {        /* gather into small records */
        const u8 *p = iov[i].iov_base;
        u32 len = (u32)(iov[i].iov_len < (size_t)total ? iov[i].iov_len : (size_t)total);
        total -= len;
        while (len) {
            u32 k = len < sizeof tmp - used ? len : (u32)sizeof tmp - used;
            tk_memcpy(tmp + used, p, k);
            used += k; p += k; len -= k;
            if (used == sizeof tmp) { emit(type, fd, port, used, used, call, tmp, used, 0, 0); used = 0; }
        }
    }
    if (used) emit(type, fd, port, used, used, call, tmp, used, 0, 0);
}

ssize_t readv(int fd, const struct iovec *iov, int cnt)
{
    long r = ret(sc(SYS_readv, fd, (long)iov, cnt, 0, 0, 0));
    char port[8];
    if (r > 0 && tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit_iov(REC_READ, fd, port, iov, cnt, r, 6);
        *__errno_location() = e;
    }
    return r;
}

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{
    long r = ret(sc(SYS_writev, fd, (long)iov, cnt, 0, 0, 0));
    char port[8];
    if (r > 0 && tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit_iov(REC_WRITE, fd, port, iov, cnt, r, 7);
        *__errno_location() = e;
    }
    return r;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    long r;
    char port[8];
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    r = ret(sc(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0));
    if (tracked(fd, port) && on() && req != TCGETS && req != TIOCMGET && req != FIONREAD && req != TIOCOUTQ) {
        int e = *__errno_location();
        switch (req) {
        case TCSETS: case TCSETSW: case TCSETSF:
            if (arg) emit_termios(fd, port, arg, r == 0, 2);
            break;
        case TIOCMSET: case TIOCMBIS: case TIOCMBIC:
            if (arg) emit(REC_MODEM, fd, port, req == TIOCMSET ? 1 : req == TIOCMBIS ? 2 : 3, (u32)*(int *)arg, r == 0, 0, 0, 0, 0);
            break;
        case TCFLSH:
            emit(REC_PURGE, fd, port, (u32)(long)arg, 0, 0, 0, 0, 0, 0);
            break;
        case TIOCSBRK: case TIOCCBRK:
            emit(REC_ESCAPE, fd, port, req == TIOCSBRK ? 8 : 9, r == 0, 0, 0, 0, 0, 0);
            break;
        case TCSBRK:
            emit(REC_ESCAPE, fd, port, 10, r == 0, 0, 0, 0, 0, 0);
            break;
        default:
            emit(REC_IOCTL, fd, port, (u32)req, 0, 0, 0, 0, 0, 0);
        }
        *__errno_location() = e;
    }
    return (int)r;
}

/* glibc's tcsetattr: struct termios -> kernel termios, ioctl TCSETS(+W/F) */
int tcsetattr(int fd, int act, const struct termios *t)
{
    struct { u32 f[4]; u8 line, cc[19]; } k;
    long r;
    char port[8];
    if (act < 0 || act > 2) { *__errno_location() = 22; return -1; }
    k.f[0] = t->c_iflag; k.f[1] = t->c_oflag; k.f[2] = t->c_cflag; k.f[3] = t->c_lflag;
    k.line = t->c_line;
    tk_memcpy(k.cc, t->c_cc, 19);
    r = ret(sc(SYS_ioctl, fd, TCSETS + act, (long)&k, 0, 0, 0));
    if (tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit_termios(fd, port, &k, r == 0, 1);
        *__errno_location() = e;
    }
    return (int)r;
}

int tcflush(int fd, int q)
{
    long r = ret(sc(SYS_ioctl, fd, TCFLSH, q, 0, 0, 0));
    char port[8];
    if (tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit(REC_PURGE, fd, port, (u32)q, 0, 0, 0, 0, 0, 0);
        *__errno_location() = e;
    }
    return (int)r;
}

int tcsendbreak(int fd, int dur)
{
    long r = ret(sc(SYS_ioctl, fd, TCSBRK, 0, 0, 0, 0));
    char port[8];
    (void)dur;
    if (tracked(fd, port) && on()) {
        int e = *__errno_location();
        emit(REC_ESCAPE, fd, port, 10, r == 0, 0, 0, 0, 0, 0);
        *__errno_location() = e;
    }
    return (int)r;
}

int close(int fd)
{
    char port[8];
    int t = tracked(fd, port);
    long r;
    if (t) ports[fd][0] = 0;
    r = ret(sc(SYS_close, fd, 0, 0, 0, 0, 0));
    if (t && on()) {
        int e = *__errno_location();
        emit(REC_CLOSE, fd, port, r == 0, 0, 0, 0, 0, 0, 0);
        *__errno_location() = e;
    }
    return (int)r;
}
