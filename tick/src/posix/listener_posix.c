/* listener_posix.c - `tick run PROGRAM`: start the vendor program with
 * libtickhook.so preloaded and turn its records (src/ring.h) into
 * api-serial events.  Like on Windows, Tick never stops or signals the
 * program; when Tick stops recording or exits the hooks pass calls through. */
#define _GNU_SOURCE
#include "tickx.h"
#include "../ring.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

extern const unsigned char tickhook_so[];
extern const unsigned int tickhook_so_len;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th;
static ring_hdr *ring;
static char ring_path[TICK_PATH], prog[256], status_text[800];
static volatile int running, stop_reader;
static pid_t child;
static u32 last_drops, bytes_in, bytes_out;
static u8 *recbuf;

/* short port ids from the hook ("t3") -> device paths */
static struct { char id[8]; char path[256]; char desc[400]; } ports[32];
static int nports;
static int last_port = -1;

int listener_running(void) { return running; }
int listener_pid(void) { return running ? (int)child : 0; }
const char *listener_status(void) { return status_text; }

static void set_status(void)
{
    if (running) {
        if (last_port >= 0)
            snprintf(status_text, sizeof status_text, "%s opened %s\nListening: %u bytes sent, %u received%s", prog,
                     ports[last_port].desc[0] ? ports[last_port].desc : ports[last_port].path, bytes_out, bytes_in,
                     session_active() ? "" : " (paused, not recording)");
        else snprintf(status_text, sizeof status_text, "%s: listening, no serial port opened yet", prog);
    } else if (prog[0]) snprintf(status_text, sizeof status_text, "%s has ended.", prog);
    else status_text[0] = 0;
}

static int port_index(const char *id)
{
    int i;
    for (i = 0; i < nports; i++) if (!strcmp(ports[i].id, id)) return i;
    return -1;
}

/* source id from a device path: /dev/ttyUSB0 -> ttyusb0, /dev/pts/3 -> pts3 */
static void src_id(char *out, const char *path)
{
    const char *b = path, *q;
    if (!strncmp(path, "/dev/", 5)) b = path + 5;
    else for (q = path; *q; q++) if (*q == '/') b = q + 1;     /* not under /dev: its name */
    int i = 0;
    for (; *b && i < 30; b++) {
        char c = *b;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[i++] = c;
    }
    out[i] = 0;
    if (!i) strcpy(out, "serial");
}

static void ensure_source(const char *id, const char *path)
{
    char json[400];
    sbuf b;
    if (session_has_source(id)) return;
    sb_init(&b, 300);
    sb_str(&b, "{\"id\": \"");
    sb_str(&b, id);
    sb_str(&b, "\", \"kind\": \"api-serial\", \"label\": ");
    snprintf(json, sizeof json, "%s (%s)", path, prog);
    sb_jsonz(&b, json);
    sb_str(&b, ", \"process\": ");
    sb_jsonz(&b, prog);
    sb_str(&b, ", \"device\": ");
    sb_jsonz(&b, path);
    sb_putc(&b, '}');
    session_add_source(id, b.p);
    sb_free(&b);
}

static void describe(int pi, const tick_termios *t)
{
    u32 bits = 5 + ((t->cflag >> 4) & 3);           /* CSIZE: CS5..CS8 */
    char par = !(t->cflag & PARENB) ? 'N' : (t->cflag & 01000000000) ? ((t->cflag & PARODD) ? 'M' : 'S')
             : (t->cflag & PARODD) ? 'O' : 'E';
    const char *hs = (t->cflag & 020000000000) ? "hardware handshake (RTS/CTS)"
                   : (t->iflag & (IXON | IXOFF)) ? "XON/XOFF handshake" : "no handshake";
    snprintf(ports[pi].desc, sizeof ports[pi].desc, "%s: %u baud, %u%c%s, %s", ports[pi].path, t->ospeed, bits, par,
             (t->cflag & CSTOPB) ? "2" : "1", hs);
}

static void record_to_event(const ring_rec *r, const u8 *p)
{
    sbuf b;
    char id[32], rp[8];
    int pi;
    u64 t = (r->flags & RECF_UNIXUS) ? r->t : clock_now_us();
    memcpy(rp, r->port, 8);
    rp[7] = 0;
    sb_init(&b, 256 + r->dlen * 2);
    if (r->type == REC_TEXT || r->type == REC_ERROR) {
        ev_begin(&b, t, r->type == REC_TEXT ? "status" : "error");
        sb_str(&b, ",\"text\":");
        sb_json(&b, (const char *)p, r->dlen);
        ev_end(&b);
        sb_free(&b);
        return;
    }
    if (r->type == REC_OPEN) {
        char path[256];
        u32 n = r->dlen < sizeof path - 1 ? r->dlen : (u32)sizeof path - 1;
        memcpy(path, p, n);
        path[n] = 0;
        src_id(id, path);
        ensure_source(id, path);
        if (r->a && nports < 32) {
            pi = port_index(rp);
            if (pi < 0) pi = nports++;
            copy_str(ports[pi].id, rp, sizeof ports[pi].id);
            copy_str(ports[pi].path, path, sizeof ports[pi].path);
            ports[pi].desc[0] = 0;
            last_port = pi;
        }
        ev_begin(&b, t, "open");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"path\":"); sb_jsonz(&b, path);
        sb_str(&b, r->a ? ",\"ok\":true" : ",\"ok\":false");
        if (!r->a) { sb_str(&b, ",\"error\":"); sb_jsonz(&b, strerror((int)r->b)); }
        ev_end(&b);
        sb_free(&b);
        return;
    }
    pi = port_index(rp);
    if (pi < 0) { sb_free(&b); return; }
    src_id(id, ports[pi].path);
    switch (r->type) {
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
        sb_str(&b, r->c == 6 ? "readv" : r->c == 7 ? "writev" : r->type == REC_READ ? "read" : "write");
        sb_str(&b, "\",\"n\":"); sb_u32(&b, r->dlen);
        if (r->type == REC_READ) { sb_str(&b, ",\"req\":"); sb_u32(&b, r->a); bytes_in += r->dlen; }
        else {
            bytes_out += r->dlen;
            if (r->b != r->dlen && r->c == 2) { sb_str(&b, ",\"written\":"); sb_u32(&b, r->b); }
        }
        break;
    case REC_TERMIOS:
        if (r->dlen >= sizeof(tick_termios)) {
            const tick_termios *tt = (const tick_termios *)p;
            u32 bits = 5 + ((tt->cflag >> 4) & 3);
            char par = !(tt->cflag & PARENB) ? 'N' : (tt->cflag & 01000000000) ? ((tt->cflag & PARODD) ? 'M' : 'S')
                     : (tt->cflag & PARODD) ? 'O' : 'E';
            if (r->a) describe(pi, tt);
            ev_begin(&b, t, "config");
            sb_str(&b, ",\"src\":\""); sb_str(&b, id);
            sb_str(&b, r->c == 2 ? "\",\"call\":\"ioctl TCSETS\"" : "\",\"call\":\"tcsetattr\"");
            sb_str(&b, ",\"baud\":"); sb_u32(&b, tt->ospeed);
            sb_str(&b, ",\"bits\":"); sb_u32(&b, bits);
            sb_str(&b, ",\"parity\":\""); sb_putc(&b, par);
            sb_str(&b, "\",\"stop\":"); sb_str(&b, (tt->cflag & CSTOPB) ? "2" : "1");
            if (tt->cflag & 020000000000) sb_str(&b, ",\"rts\":\"handshake\",\"cts_flow\":true");
            if (tt->iflag & (IXON | IXOFF)) {
                sb_str(&b, ",\"xonxoff\":\"");
                sb_str(&b, (tt->iflag & IXON) && (tt->iflag & IXOFF) ? "both" : (tt->iflag & IXON) ? "out" : "in");
                sb_putc(&b, '"');
            }
            sb_str(&b, (tt->lflag & ICANON) ? ",\"canonical\":true" : ",\"raw\":true");
            sb_str(&b, ",\"vmin\":"); sb_u32(&b, tt->cc[VMIN]);
            sb_str(&b, ",\"vtime\":"); sb_u32(&b, tt->cc[VTIME]);
            sb_str(&b, ",\"termios\":{\"iflag\":"); sb_u32(&b, tt->iflag);
            sb_str(&b, ",\"oflag\":"); sb_u32(&b, tt->oflag);
            sb_str(&b, ",\"cflag\":"); sb_u32(&b, tt->cflag);
            sb_str(&b, ",\"lflag\":"); sb_u32(&b, tt->lflag);
            sb_putc(&b, '}');
            if (!r->a) sb_str(&b, ",\"ok\":false");
        } else { sb_free(&b); return; }
        break;
    case REC_MODEM:
        ev_begin(&b, t, "config");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, r->a == 1 ? "\",\"call\":\"ioctl TIOCMSET\"" : r->a == 2 ? "\",\"call\":\"ioctl TIOCMBIS\"" : "\",\"call\":\"ioctl TIOCMBIC\"");
        if (r->a == 1 || (r->b & 0x002)) { sb_str(&b, ",\"dtr\":\""); sb_str(&b, (r->a == 3 || !(r->b & 0x002)) ? "off" : "on"); sb_putc(&b, '"'); }
        if (r->a == 1 || (r->b & 0x004)) { sb_str(&b, ",\"rts\":\""); sb_str(&b, (r->a == 3 || !(r->b & 0x004)) ? "off" : "on"); sb_putc(&b, '"'); }
        if (!r->c) sb_str(&b, ",\"ok\":false");
        break;
    case REC_ESCAPE:
        ev_begin(&b, t, "config");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        if (r->a == 10) sb_str(&b, "\",\"call\":\"tcsendbreak\",\"break\":\"pulse\"");
        else sb_str(&b, r->a == 8 ? "\",\"call\":\"ioctl TIOCSBRK\",\"break\":\"on\"" : "\",\"call\":\"ioctl TIOCCBRK\",\"break\":\"off\"");
        if (!r->b) sb_str(&b, ",\"ok\":false");
        break;
    case REC_PURGE:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"call\":\"tcflush\",\"queue\":\"");
        sb_str(&b, r->a == TCIFLUSH ? "in" : r->a == TCOFLUSH ? "out" : "both");
        sb_putc(&b, '"');
        break;
    case REC_IOCTL:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, "\",\"call\":\"ioctl\",\"code\":\"0x"); sb_hex32(&b, r->a, 4); sb_putc(&b, '"');
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
        char m[100];
        snprintf(m, sizeof m, "%u listener records lost (Tick could not keep up)", ring->drops - last_drops);
        last_drops = ring->drops;
        if (session_active()) ev_status("error", m);
    }
}

static void cleanup(void)
{
    if (ring) {
        ring->active = 0;
        munmap(ring, sizeof(ring_hdr) + RING_DATA);
        ring = 0;
    }
    if (ring_path[0]) { unlink(ring_path); ring_path[0] = 0; }
}

static void *reader(void *arg)
{
    u32 unfinished = 0;
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 25000000};
        int st = 0, done;
        u32 tail;
        nanosleep(&ts, 0);
        done = waitpid(child, &st, WNOHANG) == child;
        pthread_mutex_lock(&lk);
        tail = ring ? ring->tail : 0;
        drain();
        if (ring && ring->tail == tail && ring->tail != ring->reserve) {
            if (!unfinished) unfinished = p_ms();
            else if (p_ms() - unfinished > 2000) { ring_skip(ring); unfinished = 0; }
        } else unfinished = 0;
        if (done || stop_reader) {
            char m[400];
            if (done) {
                if (WIFEXITED(st)) snprintf(m, sizeof m, "%s ended (exit code %d)", prog, WEXITSTATUS(st));
                else snprintf(m, sizeof m, "%s ended (signal %d)", prog, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            } else snprintf(m, sizeof m, "Tick stopped listening to %s (the program keeps running)", prog);
            if (session_active()) ev_status("status", m);
            cleanup();
            running = 0;
            set_status();
            pthread_mutex_unlock(&lk);
            return 0;
        }
        set_status();
        pthread_mutex_unlock(&lk);
    }
}

void listener_session_changed(void)
{
    pthread_mutex_lock(&lk);
    drain();
    if (ring) ring->active = session_active() ? 1 : 0;
    set_status();
    pthread_mutex_unlock(&lk);
}

void listener_detach_all(void)
{
    if (!running) return;
    pthread_mutex_lock(&lk);
    if (ring) ring->active = 0;
    stop_reader = 1;
    pthread_mutex_unlock(&lk);
    pthread_join(th, 0);
}

/* write libtickhook.so where it can be mapped executable: session tick/, then
 * $XDG_RUNTIME_DIR, then /tmp (a noexec mount fails the mmap test) */
static int place_hook(char *out, u32 cap)
{
    const char *dirs[3];
    char d[TICK_PATH];
    int i;
    snprintf(d, sizeof d, "%stick", session_dir());
    p_mkdir(d);
    dirs[0] = d;
    dirs[1] = getenv("XDG_RUNTIME_DIR");
    dirs[2] = "/tmp";
    for (i = 0; i < 3; i++) {
        int fd;
        void *m;
        if (!dirs[i] || !*dirs[i]) continue;
        snprintf(out, cap, "%s/libtickhook-%d.so", dirs[i], (int)getpid());
        if (i == 0) snprintf(out, cap, "%s/libtickhook.so", dirs[i]);
        fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
        if (fd < 0) continue;
        if (write(fd, tickhook_so, tickhook_so_len) != (ssize_t)tickhook_so_len) { close(fd); unlink(out); continue; }
        close(fd);
        fd = open(out, O_RDONLY | O_CLOEXEC);
        m = fd >= 0 ? mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        if (fd >= 0) close(fd);
        if (m != MAP_FAILED) { munmap(m, 4096); return 1; }
        unlink(out);
    }
    return 0;
}

/* rings left behind by a Tick that was killed (their Tick process is gone) */
static void remove_stale_rings(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        int pid;
        char p[TICK_PATH];
        if (sscanf(e->d_name, "tick-ring-%d-", &pid) != 1 || pid <= 0) continue;
        if (kill(pid, 0) < 0 && errno == ESRCH) {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    }
    closedir(d);
}

static int make_ring(void)
{
    const char *dirs[3] = {"/dev/shm", getenv("XDG_RUNTIME_DIR"), "/tmp"};
    static int n;
    int i, fd = -1;
    for (i = 0; i < 3; i++) if (dirs[i]) remove_stale_rings(dirs[i]);
    for (i = 0; i < 3 && fd < 0; i++) {
        if (!dirs[i]) continue;
        snprintf(ring_path, sizeof ring_path, "%s/tick-ring-%d-%d", dirs[i], (int)getpid(), ++n);
        fd = open(ring_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    }
    if (fd < 0) { ring_path[0] = 0; return 0; }
    if (ftruncate(fd, sizeof(ring_hdr) + RING_DATA) < 0) { close(fd); unlink(ring_path); ring_path[0] = 0; return 0; }
    ring = mmap(0, sizeof(ring_hdr) + RING_DATA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ring == MAP_FAILED) { ring = 0; unlink(ring_path); ring_path[0] = 0; return 0; }
    ring_init(ring, RING_DATA);
    ring->tick_pid = (u32)getpid();
    ring->pad[0] = 1;
    ring->active = session_active() ? 1 : 0;
    last_drops = bytes_in = bytes_out = 0;
    nports = 0;
    last_port = -1;
    if (!recbuf) recbuf = malloc(RING_MAXREC + 256);
    return recbuf != 0;
}

int listener_run(char **argv, int quiet_output, char *err, u32 errcap)
{
    char so[TICK_PATH], pre[TICK_PATH * 2], logp[TICK_PATH];
    const char *old = getenv("LD_PRELOAD");
    const char *b = argv[0], *q;
    int pipefd[2];
    if (running) { snprintf(err, errcap, "Tick is already listening to a program"); return 0; }
    for (q = argv[0]; *q; q++) if (*q == '/') b = q + 1;
    copy_str(prog, b, sizeof prog);
    pthread_mutex_lock(&lk);
    if (!make_ring() || !place_hook(so, sizeof so)) {
        cleanup();
        pthread_mutex_unlock(&lk);
        snprintf(err, errcap, "Could not prepare the listener (shared memory or a place for libtickhook.so)");
        return 0;
    }
    snprintf(pre, sizeof pre, "%s%s%s", so, old && *old ? ":" : "", old && *old ? old : "");
    snprintf(logp, sizeof logp, "%stick/program-output.log", session_dir());
    if (pipe2(pipefd, O_CLOEXEC) < 0) pipefd[0] = pipefd[1] = -1;
    child = fork();
    if (child == 0) {
        int e;
        setpgid(0, 0);                      /* the terminal's keys and hang-up stay with Tick */
        setenv("LD_PRELOAD", pre, 1);
        setenv("TICK_RING", ring_path, 1);
        if (quiet_output) {                 /* keep the program's text out of Tick's terminal screen */
            int in = open("/dev/null", O_RDONLY), out = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (in >= 0) dup2(in, 0);
            if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        }
        execvp(argv[0], argv);
        e = errno;
        if (pipefd[1] >= 0) { ssize_t w = write(pipefd[1], &e, sizeof e); (void)w; }
        _exit(127);
    }
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (child < 0) {
        cleanup();
        pthread_mutex_unlock(&lk);
        snprintf(err, errcap, "Could not start %s: %s", argv[0], strerror(errno));
        return 0;
    }
    if (pipefd[0] >= 0) {
        int e = 0;
        ssize_t r = read(pipefd[0], &e, sizeof e);     /* closes on successful exec */
        close(pipefd[0]);
        if (r == (ssize_t)sizeof e) {
            waitpid(child, 0, 0);
            cleanup();
            pthread_mutex_unlock(&lk);
            snprintf(err, errcap, "Could not start %s: %s", argv[0], strerror(e));
            return 0;
        }
    }
    {
        char m[600];
        int i;
        size_t o = (size_t)snprintf(m, sizeof m, "Started");
        for (i = 0; argv[i] && o < sizeof m - 2; i++) o += (size_t)snprintf(m + o, sizeof m - o, " %s", argv[i]);
        snprintf(m + (o < sizeof m ? o : sizeof m - 1), sizeof m - (o < sizeof m ? o : sizeof m - 1),
                 " (process %d) with Tick's serial listener", (int)child);
        ev_status("status", m);
    }
    stop_reader = 0;
    running = 1;
    set_status();
    pthread_create(&th, 0, reader, 0);
    pthread_mutex_unlock(&lk);
    return 1;
}
