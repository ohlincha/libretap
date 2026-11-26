/* tick_linux.c - Tick for Linux: terminal UI (big clock, s/m/y/q keys),
 * control socket for agents and scripts, `tick run PROGRAM` with the serial
 * listener.  Same session format as Tick for Windows (docs/FORMAT.md). */
#define _GNU_SOURCE
#include "tickx.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

tick_cfg g_cfg;
char g_exe_dir[TICK_PATH];

static int tui, quit, screen_on = 1, utf8;
static struct termios saved_tty;
static int tty_saved;
static char note[256], msg[300];
static int note_mode;
static u32 msg_until, flash_until, rec_start;
static int flash_n;
static char ctl_path[TICK_PATH];
static int ctl_fd = -1;

static void usage(void)
{
    puts("Tick " TICK_VERSION " - records the screen and a program's serial port for LibreTap\n"
         "\n"
         "  tick [options]                      recorder with a terminal clock (keys: s m y q)\n"
         "  tick run [options] [--] PROGRAM [ARGS]\n"
         "                                      record, and start PROGRAM with the serial listener\n"
         "                                      (recommended way to record the serial port)\n"
         "  tick ctl COMMAND [NOTE]             control a running tick: status start stop mark sync quit\n"
         "\n"
         "options: --dir DIR (sessions folder), --interval MS (500), --full-every N (60),\n"
         "         --no-screen, --no-titles, --start (record at once), --no-tui (no terminal UI),\n"
         "         --until-exit (with run: stop and quit when the program ends),\n"
         "         --control PATH (control socket; default $XDG_RUNTIME_DIR/tick.sock or /tmp/tick-UID.sock)");
}

static void default_ctl_path(char *out, u32 cap)
{
    const char *r = getenv("XDG_RUNTIME_DIR");
    if (r && *r) snprintf(out, cap, "%s/tick.sock", r);
    else snprintf(out, cap, "/tmp/tick-%d.sock", (int)getuid());
}

/* ---------------- actions ---------------- */

static void show(const char *m)
{
    copy_str(msg, m, sizeof msg);
    msg_until = p_ms() + 6000;
    if (!tui) { printf("tick: %s\n", m); fflush(stdout); }
}

static int start_rec(void)
{
    char err[300];
    if (session_active()) return 1;
    if (!session_start(err, sizeof err)) { show(err); return 0; }
    rec_start = p_ms();
    if (screen_on && !capture_start()) show(capture_error());
    listener_session_changed();
    if (!tui) printf("tick: recording into %s\n", session_dir());
    return 1;
}

static void stop_rec(void)
{
    if (!session_active()) return;
    capture_stop();
    listener_session_changed();
    session_stop();
    if (!tui) printf("tick: stopped\n");
}

static int do_mark(const char *text)
{
    sbuf b;
    long n;
    char m[400];
    if (!session_active()) return 0;
    n = __sync_add_and_fetch(&g_mark_n, 1);
    sb_init(&b, 300);
    ev_begin(&b, clock_now_us(), "mark");
    sb_str(&b, ",\"n\":");
    sb_u32(&b, (u32)n);
    sb_str(&b, ",\"note\":");
    sb_jsonz(&b, text ? text : "");
    ev_end(&b);
    sb_free(&b);
    snprintf(m, sizeof m, "Mark %ld saved%s%s", n, text && *text ? ": " : "", text ? text : "");
    show(m);
    return (int)n;
}

static int do_sync(void)
{
    sbuf b;
    long n;
    if (!session_active()) return 0;
    n = __sync_add_and_fetch(&g_sync_n, 1);
    sb_init(&b, 64);
    ev_begin(&b, clock_now_us(), "sync");
    sb_str(&b, ",\"n\":");
    sb_u32(&b, (u32)n);
    ev_end(&b);
    sb_free(&b);
    flash_n = (int)n;
    flash_until = p_ms() + 1000;
    if (!tui) printf("tick: sync %ld\n", n);
    return (int)n;
}

/* ---------------- terminal UI ---------------- */

static const char *font[10][5] = {
    {"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"}, {"###", "  #", "###", "#  ", "###"},
    {"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
    {"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
    {"###", "# #", "###", "  #", "###"}};

static void tty_restore(void)
{
    if (tty_saved) {
        tcsetattr(0, TCSAFLUSH, &saved_tty);
        fputs("\033[?25h\033[0m\n", stdout);
        fflush(stdout);
        tty_saved = 0;
    }
}

static void tty_raw(void)
{
    struct termios t;
    if (tcgetattr(0, &saved_tty) < 0) return;
    tty_saved = 1;
    t = saved_tty;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    fputs("\033[2J\033[?25l", stdout);
}

static int cols = 80;

/* a line that fits the terminal: long texts keep their end ("...end") */
static const char *fit(const char *text, char *buf, size_t cap, int indent)
{
    int w = cols - indent - 1;
    size_t n = strlen(text);
    if (w < 10 || (int)n <= w) return text;
    snprintf(buf, cap, "...%s", text + n - (size_t)(w - 3));
    return buf;
}

static void draw(void)
{
    char out[8192], line[256];
    size_t o = 0;
    tick_tm t;
    u64 now = clock_now_us();
    int r, i, rec = session_active(), flash = flash_until && p_ms() < flash_until;
    int off = clock_utc_offset_min();
    static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    char digits[32];
    {
        struct winsize ws;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) cols = ws.ws_col;
    }
    clock_tm(now, &t);
    snprintf(digits, sizeof digits, "%02d:%02d:%02d.%d", t.hour, t.min, t.sec, t.ms / 100);
#define PUT(...) do { o += (size_t)snprintf(out + o, sizeof out - o, __VA_ARGS__); if (o >= sizeof out) o = sizeof out - 1; } while (0)
    PUT("\033[H");
    PUT("%s", flash ? "\033[7m" : rec ? "\033[1;31m" : "\033[1m");
    for (r = 0; r < 5; r++) {
        PUT("  ");
        for (i = 0; digits[i]; i++) {
            const char *g;
            if (digits[i] == ':') g = (r == 1 || r == 3) ? " # " : "   ";
            else if (digits[i] == '.') g = r == 4 ? " # " : "   ";
            else g = font[digits[i] - '0'][r];
            {   /* "#" becomes a full block on UTF-8 terminals */
                int k;
                for (k = 0; k < 3; k++) {
                    if (g[k] == '#') PUT("%s", utf8 ? "\xe2\x96\x88\xe2\x96\x88" : "##");
                    else PUT("  ");
                }
                PUT(" ");
            }
        }
        PUT("\033[K\n");
    }
    PUT("\033[0m");
    if (flash) snprintf(line, sizeof line, "  SYNC %d", flash_n);
    else snprintf(line, sizeof line, "  %s%s %04d-%02d-%02d   UTC%c%02d:%02d", rec ? "* REC   " : "", wd[t.wday % 7],
                  t.year, t.mon, t.day, off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
    PUT("%s%s\033[0m\033[K\n\033[K\n", rec ? "\033[31m" : "", line);
    if (msg_until && p_ms() < msg_until) PUT("  \033[34m%s\033[0m\033[K\n", fit(msg, line, sizeof line, 2));
    else if (rec) {
        u32 s = (p_ms() - rec_start) / 1000, kb = session_bytes_kb();
        PUT("  Recording %02u:%02u:%02u - %ld screenshots, %u %s, %ld marks\033[K\n", s / 3600, s / 60 % 60, s % 60,
            g_frames_stored, kb < 1024 ? kb : kb / 1024, kb < 1024 ? "kB" : "MB", g_mark_n);
    } else PUT("  Not recording. Press s to start.\033[K\n");
    {
        char f[TICK_PATH + 20];
        snprintf(f, sizeof f, "%s%s", rec ? "Folder: " : session_dir()[0] ? "Last session: " : "", session_dir());
        PUT("  %s\033[K\n", fit(f, line, sizeof line, 2));
    }
    {
        const char *ls = listener_status(), *nl = strchr(ls, '\n');
        if (*ls) {
            char first[800];
            snprintf(first, sizeof first, "%.*s", nl ? (int)(nl - ls) : (int)strlen(ls), ls);
            PUT("  %s\033[K\n", fit(first, line, sizeof line, 2));
            PUT("  %s\033[K\n", nl ? fit(nl + 1, line, sizeof line, 2) : "");
        } else PUT("  Serial port too? Start the program with: tick run PROGRAM (recommended)\033[K\n\033[K\n");
    }
    if (note_mode) PUT("\n  Note for mark (Enter = mark, Esc = cancel): %s_\033[K\n", note);
    else PUT("\n  [s] %s   [m] mark with note   [y] sync   [q] quit\033[K\n", rec ? "stop" : "start");
    PUT("\033[J");
#undef PUT
    if (write(1, out, o) < 0) {}
}

static void key(int c)
{
    if (note_mode) {
        size_t n = strlen(note);
        if (c == '\r' || c == '\n') { do_mark(note); note[0] = 0; note_mode = 0; }
        else if (c == 27) { note[0] = 0; note_mode = 0; }
        else if ((c == 127 || c == 8) && n) note[n - 1] = 0;
        else if (c >= 32 && n + 1 < sizeof note) { note[n] = (char)c; note[n + 1] = 0; }
        return;
    }
    switch (c) {
    case 's': case 'S': if (session_active()) stop_rec(); else start_rec(); break;
    case 'm': case 'M': if (session_active()) note_mode = 1; else show("Press s to start recording first"); break;
    case 'y': case 'Y': if (!do_sync()) show("Press s to start recording first"); break;
    case 'q': case 'Q': case 3: quit = 1; break;
    }
}

/* ---------------- control socket ---------------- */

static int ctl_listen(void)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", ctl_path);
    {   /* a stale socket of a Tick that did not exit cleanly */
        int t = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (t >= 0 && connect(t, (struct sockaddr *)&a, sizeof a) < 0) unlink(ctl_path);
        if (t >= 0) close(t);
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
    chmod(ctl_path, 0600);
    return fd;
}

static void json_status(char *out, size_t cap)
{
    sbuf b;
    sb_init(&b, 512);
    sb_str(&b, "{\"ok\":true,\"recording\":");
    sb_str(&b, session_active() ? "true" : "false");
    sb_str(&b, ",\"session\":");
    sb_jsonz(&b, session_dir());
    sb_str(&b, ",\"frames\":"); sb_u32(&b, (u32)g_frames_stored);
    sb_str(&b, ",\"kb\":"); sb_u32(&b, session_bytes_kb());
    sb_str(&b, ",\"marks\":"); sb_u32(&b, (u32)g_mark_n);
    sb_str(&b, ",\"syncs\":"); sb_u32(&b, (u32)g_sync_n);
    sb_str(&b, ",\"listener\":"); sb_jsonz(&b, listener_status());
    sb_str(&b, ",\"program_pid\":"); sb_u32(&b, (u32)listener_pid());
    sb_str(&b, ",\"screen\":"); sb_jsonz(&b, screen_on ? capture_error() : "off");
    sb_str(&b, session_write_failed() ? ",\"write_failed\":true" : ",\"write_failed\":false");
    sb_str(&b, "}\n");
    copy_str(out, b.p ? b.p : "{}\n", (u32)cap);
    sb_free(&b);
}

static void ctl_handle(int c)
{
    char in[600], out[2400];
    ssize_t n;
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 1000) <= 0 || (n = read(c, in, sizeof in - 1)) <= 0) { close(c); return; }
    in[n] = 0;
    while (n && (in[n - 1] == '\n' || in[n - 1] == '\r')) in[--n] = 0;
    if (!strcmp(in, "start")) start_rec();
    else if (!strcmp(in, "stop")) stop_rec();
    else if (!strncmp(in, "mark", 4)) {
        int k = do_mark(in[4] == ' ' ? in + 5 : "");
        if (!k) { snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"not recording\"}\n"); goto send; }
    } else if (!strcmp(in, "sync")) {
        if (!do_sync()) { snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"not recording\"}\n"); goto send; }
    } else if (!strcmp(in, "quit")) quit = 1;
    else if (strcmp(in, "status")) { snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"unknown command\"}\n"); goto send; }
    json_status(out, sizeof out);
send:
    if (write(c, out, strlen(out)) < 0) {}
    close(c);
}

static int ctl_client(int argc, char **argv)
{
    struct sockaddr_un a;
    char cmd[600], buf[4000];
    int fd, i;
    ssize_t n;
    size_t o = 0;
    if (!ctl_path[0]) default_ctl_path(ctl_path, sizeof ctl_path);
    cmd[0] = 0;
    for (i = 0; i < argc; i++) o += (size_t)snprintf(cmd + o, sizeof cmd - o, "%s%s", i ? " " : "", argv[i]);
    if (!argc) { copy_str(cmd, "status", sizeof cmd); }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", ctl_path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "tick ctl: no running tick at %s\n", ctl_path);
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) return 1;
    n = read(fd, buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; fputs(buf, stdout); }
    close(fd);
    return n > 0 && strstr(buf, "\"ok\":true") ? 0 : 1;
}

/* ---------------- main ---------------- */

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    int i, run = 0, start = 0, no_tui = 0, until_exit = 0, started_prog = 0;
    char **prog = 0, err[400];
    ssize_t n;

    g_cfg.interval_ms = 500; g_cfg.full_every = 60; g_cfg.tile = 32;
    g_cfg.cursor = 0; g_cfg.titles = 1; g_cfg.mask_self = 1;
    n = readlink("/proc/self/exe", g_exe_dir, sizeof g_exe_dir - 1);
    if (n > 0) {
        g_exe_dir[n] = 0;
        while (n > 0 && g_exe_dir[n - 1] != '/') n--;
        g_exe_dir[n] = 0;
    } else copy_str(g_exe_dir, "./", sizeof g_exe_dir);

    i = 1;
    if (argc > 1 && !strcmp(argv[1], "ctl")) {
        for (i = 2; i < argc && !strcmp(argv[i], "--control") && i + 1 < argc; i += 2)
            copy_str(ctl_path, argv[i + 1], sizeof ctl_path);
        return ctl_client(argc - i, argv + i);
    }
    if (argc > 1 && !strcmp(argv[1], "run")) { run = 1; i = 2; }
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dir") && i + 1 < argc) copy_str(g_cfg.dir, argv[++i], sizeof g_cfg.dir);
        else if (!strcmp(a, "--interval") && i + 1 < argc) g_cfg.interval_ms = (u32)atoi(argv[++i]);
        else if (!strcmp(a, "--full-every") && i + 1 < argc) g_cfg.full_every = (u32)atoi(argv[++i]);
        else if (!strcmp(a, "--control") && i + 1 < argc) copy_str(ctl_path, argv[++i], sizeof ctl_path);
        else if (!strcmp(a, "--no-screen")) screen_on = 0;
        else if (!strcmp(a, "--no-titles")) g_cfg.titles = 0;
        else if (!strcmp(a, "--start")) start = 1;
        else if (!strcmp(a, "--no-tui")) no_tui = 1;
        else if (!strcmp(a, "--until-exit")) until_exit = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-') { fprintf(stderr, "tick: unknown option %s\n", a); usage(); return 2; }
        else break;
    }
    if (run) {
        if (i >= argc) { usage(); return 2; }
        prog = argv + i;
    } else if (i < argc) { usage(); return 2; }
    if (g_cfg.interval_ms < 100) g_cfg.interval_ms = 100;
    if (g_cfg.full_every < 1) g_cfg.full_every = 1;

    clock_init();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    {
        const char *l = getenv("LC_ALL"), *c = getenv("LC_CTYPE"), *g = getenv("LANG");
        const char *loc = l && *l ? l : c && *c ? c : g ? g : "";
        utf8 = strstr(loc, "UTF-8") || strstr(loc, "utf8") || strstr(loc, "UTF8");
    }
    tui = !no_tui && isatty(0) && isatty(1);
    if (tui) {
        const char *w = getenv("WINDOWID");     /* our terminal window: painted grey in screenshots */
        if (w) g_mask_window = strtoul(w, 0, 10);
    }
    g_cfg.mask_self = g_mask_window != 0;
    if (!ctl_path[0]) default_ctl_path(ctl_path, sizeof ctl_path);
    ctl_fd = ctl_listen();
    if (ctl_fd < 0 && !tui) fprintf(stderr, "tick: control socket %s not available\n", ctl_path);

    if (start || run) {
        if (!start_rec()) { fprintf(stderr, "tick: %s\n", msg); return 1; }
    }
    if (run) {
        if (listener_run(prog, tui, err, sizeof err)) started_prog = 1;
        else if (tui) show(err);
        else fprintf(stderr, "tick: %s\n", err);
    }
    if (tui) tty_raw();
    while (!quit) {
        struct pollfd p[2];
        int np = 0;
        if (tui) { p[np].fd = 0; p[np].events = POLLIN; np++; }
        if (ctl_fd >= 0) { p[np].fd = ctl_fd; p[np].events = POLLIN; np++; }
        if (tui) draw();
        if (poll(p, (nfds_t)np, 100) > 0) {
            int k;
            for (k = 0; k < np; k++) {
                if (!(p[k].revents & POLLIN)) continue;
                if (p[k].fd == 0) {
                    unsigned char c[16];
                    ssize_t r = read(0, c, sizeof c), j;
                    for (j = 0; j < r; j++) key(c[j]);
                    if (r == 0) quit = 1;
                } else {
                    int c = accept4(ctl_fd, 0, 0, SOCK_CLOEXEC);
                    if (c >= 0) ctl_handle(c);
                }
            }
        }
        session_flush_if_due();
        if (until_exit && started_prog && !listener_running()) quit = 1;
    }
    listener_detach_all();
    stop_rec();
    tty_restore();
    if (ctl_fd >= 0) { close(ctl_fd); unlink(ctl_path); }
    if (session_dir()[0]) printf("tick: session in %s\n", session_dir());
    return 0;
}
