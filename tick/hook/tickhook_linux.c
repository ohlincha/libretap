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
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/usbdevice_fs.h>

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
static char ports[MAXFD][8];        /* "t3" tty, "u2" USB device, "n5" socket; "-" checked: not a TCP/UDP
                                       socket; "" = not tracked */
static tick_net nets[MAXFD];        /* addresses of tracked sockets */

static int left_out(u32 kind) { return R && (R->pad[2] & kind); }

static void new_port(char *port, char kind, volatile u32 *counter)
{
    u32 n = __sync_add_and_fetch(counter, 1);
    char d[8];
    int i = 0, j = 1;
    port[0] = kind;
    do { d[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 6);
    while (i) port[j++] = d[--i];
    port[j] = 0;
}

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
    if (fd < 0 || fd >= MAXFD || !ports[fd][0] || ports[fd][0] == '-') return 0;
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
    (void)n;
    if (starts(path, "/dev/bus/usb/") || starts(path, "/dev/hidraw") || starts(path, "/dev/usbtmc") ||
        starts(path, "/dev/usb/")) {                /* USB: usbfs (libusb), HID, USBTMC instruments */
        if (left_out(KIND_USB)) return;
        new_port(port, 'u', &R->pad[3]);
    } else {
        if (sc(SYS_ioctl, fd, TCGETS, (long)&k, 0, 0, 0) < 0) { *__errno_location() = err; return; }  /* not a tty */
        if (left_out(KIND_SERIAL)) { *__errno_location() = err; return; }
        new_port(port, 't', &R->pad[1]);
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

static void net_emit(u16 type, int fd, u32 call, u32 a, u32 b, const void *peer, u32 peerlen, const void *data, u32 n)
{
    tick_net ni = nets[fd];
    if (peer && peerlen) {
        u32 k = peerlen < 28 ? peerlen : 28;
        tk_memset(ni.remote, 0, 28);
        tk_memcpy(ni.remote, peer, k);
    }
    emit(type, fd, ports[fd], a, b, call, &ni, sizeof ni, data, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    long r = ret(sc(SYS_read, fd, (long)buf, (long)n, 0, 0, 0));
    char port[8];
    if (r > 0 && tracked(fd, port) && on()) {
        int e = *__errno_location();
        if (port[0] == 'n') net_emit(REC_NET_RECV, fd, CALL_READ, (u32)n, 0, 0, 0, buf, (u32)r);
        else emit(REC_READ, fd, port, (u32)n, 0, 1, buf, (u32)r, 0, 0);
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
        if (port[0] == 'n') net_emit(REC_NET_SEND, fd, CALL_WRITE, (u32)n, r < 0 ? 0 : (u32)r, 0, 0, buf, (u32)n);
        else emit(REC_WRITE, fd, port, (u32)n, r < 0 ? 0 : (u32)r, 2, buf, (u32)n, 0, 0);
        *__errno_location() = e;
    }
    return r;
}

static void iov_out(u16 type, int fd, const char *port, u32 call, const u8 *p, u32 n)
{
    if (port[0] == 'n') net_emit(type == REC_READ ? REC_NET_RECV : REC_NET_SEND, fd, call, n, n, 0, 0, p, n);
    else emit(type, fd, port, n, n, call, p, n, 0, 0);
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
            if (used == sizeof tmp) { iov_out(type, fd, port, call, tmp, used); used = 0; }
        }
    }
    if (used) iov_out(type, fd, port, call, tmp, used);
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

/* usbfs (libusb and friends): control, bulk and asynchronous URB transfers */
static int usbfs(int fd, const char *port, unsigned long req, void *arg, long r)
{
    if (!arg) return 0;
    if (req == USBDEVFS_CONTROL) {
        const struct usbdevfs_ctrltransfer *c = arg;
        u8 setup[8];
        u32 n = (c->bRequestType & 0x80) ? (r > 0 ? (u32)r : 0) : c->wLength;
        setup[0] = c->bRequestType; setup[1] = c->bRequest;
        setup[2] = (u8)c->wValue; setup[3] = (u8)(c->wValue >> 8);
        setup[4] = (u8)c->wIndex; setup[5] = (u8)(c->wIndex >> 8);
        setup[6] = (u8)c->wLength; setup[7] = (u8)(c->wLength >> 8);
        if (!c->data) n = 0;
        emit(REC_USB_CTRL, fd, port, r >= 0, r > 0 ? (u32)r : 0, CALL_USBFS_CONTROL, setup, 8, c->data, n);
        return 1;
    }
    if (req == USBDEVFS_BULK) {
        const struct usbdevfs_bulktransfer *b = arg;
        u32 n = (b->ep & 0x80) ? (r > 0 ? (u32)r : 0) : b->len;
        if (!b->data) n = 0;
        emit(REC_USB_XFER, fd, port, r >= 0, b->ep & 0xff, CALL_USBFS_BULK, b->data, n, 0, 0);
        return 1;
    }
    if (req == USBDEVFS_SUBMITURB || req == USBDEVFS_REAPURB || req == USBDEVFS_REAPURBNDELAY) {
        const struct usbdevfs_urb *u = req == USBDEVFS_SUBMITURB ? arg : (r == 0 ? *(struct usbdevfs_urb **)arg : 0);
        int submit = req == USBDEVFS_SUBMITURB, in;
        if (!u || u->type == USBDEVFS_URB_TYPE_ISO || !u->buffer) return 1;
        if (u->type == USBDEVFS_URB_TYPE_CONTROL) {
            const u8 *sp = u->buffer;
            in = (sp[0] & 0x80) != 0;
            if (u->buffer_length < 8) return 1;
            if (submit && !in) emit(REC_USB_CTRL, fd, port, r == 0, 0, CALL_USBFS_SUBMIT, sp, 8, sp + 8, (u32)u->buffer_length - 8);
            else if (!submit && in) emit(REC_USB_CTRL, fd, port, u->status == 0, (u32)u->actual_length, CALL_USBFS_REAP, sp, 8,
                                         sp + 8, u->actual_length > 0 ? (u32)u->actual_length : 0);
            return 1;
        }
        in = (u->endpoint & 0x80) != 0;
        if (submit && !in) emit(REC_USB_XFER, fd, port, r == 0, u->endpoint, CALL_USBFS_SUBMIT, u->buffer, (u32)u->buffer_length, 0, 0);
        else if (!submit && in) emit(REC_USB_XFER, fd, port, u->status == 0, u->endpoint, CALL_USBFS_REAP, u->buffer,
                                     u->actual_length > 0 ? (u32)u->actual_length : 0, 0, 0);
        return 1;
    }
    return 0;
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
        if (port[0] == 'u' && usbfs(fd, port, req, arg, r)) { *__errno_location() = e; return (int)r; }
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
    if (fd >= 0 && fd < MAXFD && ports[fd][0] == '-') ports[fd][0] = 0;
    if (t && on()) {
        int e = *__errno_location();
        if (port[0] == 'n') emit(REC_NET_CLOSE, fd, port, 0, 0, CALL_CLOSESOCKET, &nets[fd], sizeof nets[fd], 0, 0);
        else emit(REC_CLOSE, fd, port, r == 0, 0, 0, 0, 0, 0, 0);
        *__errno_location() = e;
    }
    return (int)r;
}

/* ---------------- sockets (TCP/UDP over IPv4/IPv6) ---------------- */

/* is fd a TCP/UDP socket? then track it ('n' port) with its addresses */
static int sock_track(int fd, const void *remote, u32 rlen)
{
    u8 la[128];
    u32 ll = sizeof la, rl = 28, type = 0, tl = sizeof type;
    u16 fam;
    if (fd < 0 || fd >= MAXFD || !on() || left_out(KIND_NET)) return 0;
    if (ports[fd][0] == 'n') return 1;
    if (ports[fd][0]) return 0;                      /* a tty/usb fd, or known not to be a socket of interest */
    if (sc(SYS_getsockname, fd, (long)la, (long)&ll, 0, 0, 0) < 0) { ports[fd][0] = '-'; ports[fd][1] = 0; return 0; }
    fam = (u16)(la[0] | la[1] << 8);
    if (fam != AF_INET && fam != AF_INET6) { ports[fd][0] = '-'; ports[fd][1] = 0; return 0; }
    sc(SYS_getsockopt, fd, SOL_SOCKET, SO_TYPE, (long)&type, (long)&tl, 0);
    tk_memset(&nets[fd], 0, sizeof nets[fd]);
    nets[fd].proto = type == SOCK_DGRAM ? 17 : 6;
    tk_memcpy(nets[fd].local, la, ll < 28 ? ll : 28);
    if (remote && rlen) tk_memcpy(nets[fd].remote, remote, rlen < 28 ? rlen : 28);
    else if (sc(SYS_getpeername, fd, (long)la, (long)&rl, 0, 0, 0) == 0) tk_memcpy(nets[fd].remote, la, rl < 28 ? rl : 28);
    new_port(ports[fd], 'n', &R->pad[4]);
    return 1;
}

static void sock_refresh(int fd)
{
    u8 a[128];
    u32 l = sizeof a;
    if (sc(SYS_getsockname, fd, (long)a, (long)&l, 0, 0, 0) == 0) { tk_memset(nets[fd].local, 0, 28); tk_memcpy(nets[fd].local, a, l < 28 ? l : 28); }
    l = sizeof a;
    if (sc(SYS_getpeername, fd, (long)a, (long)&l, 0, 0, 0) == 0) { tk_memset(nets[fd].remote, 0, 28); tk_memcpy(nets[fd].remote, a, l < 28 ? l : 28); }
}

int connect(int fd, const struct sockaddr *a, socklen_t n)
{
    long r = ret(sc(SYS_connect, fd, (long)a, n, 0, 0, 0));
    int e = *__errno_location();
    if (a && (a->sa_family == AF_INET || a->sa_family == AF_INET6) && sock_track(fd, a, n)) {
        if (r == 0) sock_refresh(fd);
        tk_memset(nets[fd].remote, 0, 28);
        tk_memcpy(nets[fd].remote, a, n < 28 ? n : 28);
        net_emit(REC_NET_OPEN, fd, CALL_CONNECT, r == 0 || e == 115 /* EINPROGRESS */, r == 0 ? 0 : (u32)e, 0, 0, 0, 0);
    }
    *__errno_location() = e;
    return (int)r;
}

static int after_accept(long r)
{
    int e = *__errno_location();
    if (r >= 0 && sock_track((int)r, 0, 0)) {
        sock_refresh((int)r);
        net_emit(REC_NET_OPEN, (int)r, CALL_ACCEPT, 1, 0, 0, 0, 0, 0);
    }
    *__errno_location() = e;
    return (int)r;
}

int accept(int fd, struct sockaddr *a, socklen_t *n) { return after_accept(ret(sc(SYS_accept4, fd, (long)a, (long)n, 0, 0, 0))); }
int accept4(int fd, struct sockaddr *a, socklen_t *n, int fl) { return after_accept(ret(sc(SYS_accept4, fd, (long)a, (long)n, fl, 0, 0))); }

ssize_t sendto(int fd, const void *buf, size_t n, int fl, const struct sockaddr *to, socklen_t tl)
{
    long r = ret(sc(SYS_sendto, fd, (long)buf, (long)n, fl, (long)to, tl));
    int e = *__errno_location();
    if (n && sock_track(fd, to, to ? tl : 0))
        net_emit(REC_NET_SEND, fd, to ? CALL_SENDTO : CALL_SEND, (u32)n, r < 0 ? 0 : (u32)r, to, to ? tl : 0, buf, (u32)n);
    *__errno_location() = e;
    return r;
}

ssize_t send(int fd, const void *buf, size_t n, int fl) { return sendto(fd, buf, n, fl, 0, 0); }

ssize_t recvfrom(int fd, void *buf, size_t n, int fl, struct sockaddr *from, socklen_t *fl2)
{
    long r = ret(sc(SYS_recvfrom, fd, (long)buf, (long)n, fl, (long)from, (long)fl2));
    int e = *__errno_location();
    if (r > 0 && sock_track(fd, 0, 0))
        net_emit(REC_NET_RECV, fd, from ? CALL_RECVFROM : CALL_RECV, (u32)n, 0, from, from && fl2 ? *fl2 : 0, buf, (u32)r);
    *__errno_location() = e;
    return r;
}

ssize_t recv(int fd, void *buf, size_t n, int fl) { return recvfrom(fd, buf, n, fl, 0, 0); }
ssize_t __recv_chk(int fd, void *buf, size_t n, size_t bl, int fl) { (void)bl; return recvfrom(fd, buf, n, fl, 0, 0); }
ssize_t __recvfrom_chk(int fd, void *buf, size_t n, size_t bl, int fl, struct sockaddr *f, socklen_t *l)
{ (void)bl; return recvfrom(fd, buf, n, fl, f, l); }

ssize_t sendmsg(int fd, const struct msghdr *m, int fl)
{
    long r = ret(sc(SYS_sendmsg, fd, (long)m, fl, 0, 0, 0));
    int e = *__errno_location();
    if (r > 0 && m && sock_track(fd, m->msg_name, m->msg_name ? m->msg_namelen : 0)) {
        char port[8];
        tk_memcpy(port, ports[fd], 8);
        if (m->msg_name) { tk_memset(nets[fd].remote, 0, 28); tk_memcpy(nets[fd].remote, m->msg_name, m->msg_namelen < 28 ? m->msg_namelen : 28); }
        emit_iov(REC_WRITE, fd, port, m->msg_iov, (int)m->msg_iovlen, r, CALL_SENDMSG);
    }
    *__errno_location() = e;
    return r;
}

ssize_t recvmsg(int fd, struct msghdr *m, int fl)
{
    long r = ret(sc(SYS_recvmsg, fd, (long)m, fl, 0, 0, 0));
    int e = *__errno_location();
    if (r > 0 && m && sock_track(fd, 0, 0)) {
        char port[8];
        tk_memcpy(port, ports[fd], 8);
        if (m->msg_name && m->msg_namelen) { tk_memset(nets[fd].remote, 0, 28); tk_memcpy(nets[fd].remote, m->msg_name, m->msg_namelen < 28 ? m->msg_namelen : 28); }
        emit_iov(REC_READ, fd, port, m->msg_iov, (int)m->msg_iovlen, r, CALL_RECVMSG);
    }
    *__errno_location() = e;
    return r;
}
