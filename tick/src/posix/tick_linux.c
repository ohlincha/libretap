/* tick_linux.c - Tick for Linux: terminal UI (big clock, s/m/y/q keys),
 * control socket for agents and scripts, `tick run PROGRAM` with the serial
 * listener.  Same session format as Tick for Windows (docs/FORMAT.md). */
#define _GNU_SOURCE
#include "tickx.h"
#include "desktop.h"
#include "programs_posix.h"
#include "x11.h"
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

int g_screen_on = 1;
static int tui, gui, quit, utf8;
static struct termios saved_tty;
static int tty_saved;
static char note[256], msg[300];
static int note_mode;
/* terminal screens: 0 clock, 1 program list, 2 settings, 3 typing a program or folder */
static int mode;
static lprog *plist;
static int pn, psel, ptop;
static char typed[TICK_PATH];
static int typing_for;                  /* 1 program, 2 sessions folder */
static u32 msg_until, flash_until, rec_start;
static int flash_n;
static char ctl_path[TICK_PATH];
static int ctl_fd = -1;

static void usage(void)
{
    puts("Tick " TICK_VERSION " - records the screen and a program's ports for LibreTap\n"
         "\n"
         "  tick                                window (started from a desktop or file manager), or\n"
         "                                      terminal clock (started in a terminal; keys s m y p o q)\n"
         "  tick run [options] [--] PROGRAM [ARGS]\n"
         "                                      record and start PROGRAM listening to its ports; PROGRAM may\n"
         "                                      also be a .desktop file or an applications-menu name\n"
         "  tick ctl COMMAND [NOTE]             control a running tick: status start stop mark sync quit programs\n"
         "\n"
         "options: --gui (window), --no-tui, --dir DIR, --interval MS (500), --full-every N (60),\n"
         "         --no-screen, --no-titles, --start, --until-exit (with run: quit when the program ends),\n"
         "         --no-serial, --no-net, --no-usb, --control PATH\n"
         "settings: tick.ini next to tick if that folder is writable, else ~/.config/tick/tick.ini");
}

static int desktop_is_command(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        size_t l = strcspn(path, ":");
        char c[TICK_PATH];
        snprintf(c, sizeof c, "%.*s/%s", (int)l, path, name);
        if (access(c, X_OK) == 0) return 1;
        path += l + (path[l] == ':');
    }
    return 0;
}

/* ---------------- settings (tick.ini) ---------------- */

static char ini_path[TICK_PATH];

static void ini_choose(void)
{
    char p[TICK_PATH];
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    snprintf(p, sizeof p, "%stick.ini", g_exe_dir);
    if (access(p, F_OK) == 0 || access(g_exe_dir, W_OK) == 0) { copy_str(ini_path, p, sizeof ini_path); return; }
    if (xc && *xc) snprintf(ini_path, sizeof ini_path, "%s/tick/tick.ini", xc);
    else snprintf(ini_path, sizeof ini_path, "%s/.config/tick/tick.ini", home ? home : "/tmp");
}

void cfg_load(void)
{
    FILE *f;
    char line[TICK_PATH + 40];
    g_cfg.interval_ms = 500; g_cfg.full_every = 60; g_cfg.tile = 32;
    g_cfg.cursor = 0; g_cfg.titles = 1; g_cfg.mask_self = 1;
    g_cfg.rec_serial = g_cfg.rec_net = g_cfg.rec_usb = 1;
    g_cfg.dir[0] = 0;
    ini_choose();
    f = fopen(ini_path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        if (!eq || line[0] == '[' || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "interval_ms")) g_cfg.interval_ms = (u32)atoi(v);
        else if (!strcmp(line, "full_every")) g_cfg.full_every = (u32)atoi(v);
        else if (!strcmp(line, "window_titles")) g_cfg.titles = atoi(v) != 0;
        else if (!strcmp(line, "record_screen")) g_screen_on = atoi(v) != 0;
        else if (!strcmp(line, "record_serial")) g_cfg.rec_serial = atoi(v) != 0;
        else if (!strcmp(line, "record_network")) g_cfg.rec_net = atoi(v) != 0;
        else if (!strcmp(line, "record_usb")) g_cfg.rec_usb = atoi(v) != 0;
        else if (!strcmp(line, "sessions_folder")) copy_str(g_cfg.dir, v, sizeof g_cfg.dir);
    }
    fclose(f);
    if (g_cfg.interval_ms < 100) g_cfg.interval_ms = 100;
    if (g_cfg.interval_ms > 600000) g_cfg.interval_ms = 600000;
    if (g_cfg.full_every < 1) g_cfg.full_every = 1;
}

/* what tick.ini said, and the values after the command line: a value given on the command line
 * is saved only if the operator changed it in Settings */
static tick_cfg ini_cfg, cli_cfg;
static int ini_screen, cli_screen;

static void cfg_mark_cli(void) { cli_cfg = g_cfg; cli_screen = g_screen_on; }

int cfg_save(void)
{
    FILE *f;
    tick_cfg c = g_cfg;
    int scr = g_screen_on;
#define KEEP(field) if (c.field == cli_cfg.field && cli_cfg.field != ini_cfg.field) c.field = ini_cfg.field
    KEEP(interval_ms); KEEP(full_every); KEEP(titles); KEEP(rec_serial); KEEP(rec_net); KEEP(rec_usb);
#undef KEEP
    if (!strcmp(c.dir, cli_cfg.dir) && strcmp(cli_cfg.dir, ini_cfg.dir)) copy_str(c.dir, ini_cfg.dir, sizeof c.dir);
    if (scr == cli_screen && cli_screen != ini_screen) scr = ini_screen;
    char d[TICK_PATH];
    char *slash;
    copy_str(d, ini_path, sizeof d);
    slash = strrchr(d, '/');
    if (slash) {                                    /* ~/.config/tick */
        char *q;
        *slash = 0;
        for (q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
        mkdir(d, 0755);
    }
    f = fopen(ini_path, "w");
    if (!f) return 0;
    fprintf(f, "[tick]\ninterval_ms=%u\nfull_every=%u\nwindow_titles=%d\nrecord_screen=%d\nrecord_serial=%d\n"
               "record_network=%d\nrecord_usb=%d\nsessions_folder=%s\n",
            c.interval_ms, c.full_every, c.titles, scr, c.rec_serial, c.rec_net, c.rec_usb, c.dir);
    if (fclose(f) != 0) return 0;
    ini_cfg = c;
    ini_screen = scr;
    cfg_mark_cli();
    return 1;
}

const char *app_ini_path(void) { return ini_path; }

/* sockaddr for a control socket path; paths longer than sun_path (108 bytes) go
 * through /proc/self/fd/N/name with N an O_PATH handle of the folder */
static int ctl_addr(struct sockaddr_un *a, const char *path, int *dirfd)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    *dirfd = -1;
    if (strlen(path) < sizeof a->sun_path) { memcpy(a->sun_path, path, strlen(path) + 1); return 1; }
    {
        char dir[TICK_PATH];
        const char *slash = strrchr(path, '/');
        if (!slash) return 0;
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
        *dirfd = open(dir[0] ? dir : "/", O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (*dirfd < 0) return 0;
        if ((size_t)snprintf(a->sun_path, sizeof a->sun_path, "/proc/self/fd/%d/%s", *dirfd, slash + 1) >= sizeof a->sun_path)
            return 0;
    }
    return 1;
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
    if (g_screen_on && !capture_start()) show(capture_error());
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

/* ---------------- choosing and restarting the program ---------------- */

static lprog pending;                   /* a running program waiting for the operator's yes */
static int pending_pid;
static char question[1600];
static lprog rs;                        /* a restart in progress */
static int rs_pid;
static u32 rs_deadline, rs_waited;

int app_start(void) { return start_rec(); }
void app_stop(void) { stop_rec(); }
int app_mark(const char *note) { return do_mark(note); }
int app_sync(void) { return do_sync(); }
void app_show(const char *m) { show(m); }
const char *app_message(void) { return msg_until && p_ms() < msg_until ? msg : ""; }
int app_flash(void) { return flash_until && p_ms() < flash_until ? flash_n : 0; }
u32 app_rec_seconds(void) { return session_active() ? (p_ms() - rec_start) / 1000 : 0; }
const char *app_question(void) { return pending_pid ? question : 0; }

static void start_prog(const lprog *p)
{
    char err[400];
    if (listener_running()) { show("Tick is already listening to a program."); return; }
    if (!start_rec()) return;
    if (!listener_run_in(p->argv, p->cwd[0] ? p->cwd : 0, p->envp, tui || gui, err, sizeof err)) show(err);
}

/* 1: started or refused (message shown); 2: a question for the operator (app_question) */
int app_choose(const lprog *p)
{
    int copies = 0, pid = p->running ? p->pid : lprog_find_running(p->exe, &copies);
    if (listener_running()) { show("Tick is already listening to a program."); return 1; }
    if (p->running && !lprog_allowed(pid)) { show("Tick may not use that program."); return 1; }
    if (copies > 1) {   /* which one is meant is unclear: never guess which to close */
        char m[400];
        snprintf(m, sizeof m, "%d copies of %s are running. Close them, then choose it again.", copies, p->name);
        show(m);
        return 1;
    }
    if (!pid) { start_prog(p); return 1; }
    lprog_free_strings(pending.argv);
    lprog_free_strings(pending.envp);
    memset(&pending, 0, sizeof pending);
    if (!lprog_running(pid, &pending)) { start_prog(p); return 1; }
    pending_pid = pid;
    snprintf(question, sizeof question, "Restart %s under Tick to record its ports? It will be closed and started again.\n%s",
             pending.name, pending.detail);
    return 2;
}

void app_answer(int yes)
{
    if (!pending_pid) return;
    if (yes) {
        lprog_free_strings(rs.argv);
        lprog_free_strings(rs.envp);
        rs = pending;
        memset(&pending, 0, sizeof pending);
        rs_pid = pending_pid;
        rs_deadline = p_ms() + 20000;
        rs_waited = 0;
        if (!lprog_close(rs_pid)) { show("Tick may not close that program."); rs_pid = 0; }
        else show("Closing the program...");
    }
    pending_pid = 0;
}

void app_poll(void)
{
    if (!rs_pid) return;
    if (!lprog_alive(rs_pid)) {
        rs_pid = 0;
        start_prog(&rs);
        return;
    }
    if ((int)(p_ms() - rs_deadline) > 0) {
        char m[300];
        rs_waited++;
        if (rs_waited > 6) { show("Not restarted: the program did not close."); rs_pid = 0; return; }
        snprintf(m, sizeof m, "%s has not closed (is it asking a question?). Close it; Tick then starts it.", rs.name);
        show(m);
        msg_until = p_ms() + 20000;
        rs_deadline = p_ms() + 20000;
    }
}

int app_pick(void)
{
    x11 x;
    char err[300], title[160];
    int pid;
    lprog p;
    if (!x11_open(&x, err, sizeof err)) { show("No display: Pick window needs X11."); return 0; }
    pid = x11_pick_pid(&x, 20000, title, sizeof title);
    x11_close(&x);
    if (!pid || pid == getpid()) { show("No window picked."); return 0; }
    if (!lprog_running(pid, &p)) { show("Tick cannot see that window's program (another user's?)."); return 0; }
    app_choose(&p);
    lprog_free_strings(p.argv);
    lprog_free_strings(p.envp);
    return 1;
}

/* a program, .desktop file or menu name (Browse, a file given on the command line) */
int app_open(const char *what)
{
    lprog p;
    char err[400];
    int r;
    if (!lprog_resolve(what, &p, err, sizeof err)) { show(err); return 0; }
    r = app_choose(&p);
    lprog_free_strings(p.argv);
    return r;
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
    if (mode == 1) {                                /* the program list */
        int rows = 10, k;
        {
            struct winsize ws;
            if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 16) rows = ws.ws_row - 13;
        }
        PUT("  Which program talks to the instrument?\033[K\n");
        if (psel < ptop) ptop = psel;
        if (psel >= ptop + rows) ptop = psel - rows + 1;
        for (k = ptop; k < pn && k < ptop + rows; k++)
            PUT("%s%s%s\033[0m\033[K\n", k == psel ? "\033[7m> " : "  ", fit(plist[k].label, line, sizeof line, 4),
                k == psel ? " " : "");
        if (!pn) PUT("  (no programs found)\033[K\n");
        PUT("  %s\033[K\n", pn ? fit(plist[psel].detail, line, sizeof line, 2) : "");
        PUT("\033[K\n  [Enter] start   [w] pick window   [f] type a program or file   [Esc] back\033[K\n");
    } else if (mode == 2) {                         /* settings */
        PUT("  Settings (saved in %s)\033[K\n", fit(ini_path, line, sizeof line, 22));
        PUT("  [1] record the screen      %s\033[K\n", g_screen_on ? "on" : "off");
        PUT("  [2] serial ports           %s\033[K\n", g_cfg.rec_serial ? "on" : "off");
        PUT("  [3] network                %s\033[K\n", g_cfg.rec_net ? "on" : "off");
        PUT("  [4] USB                    %s\033[K\n", g_cfg.rec_usb ? "on" : "off");
        PUT("  [5] window titles          %s\033[K\n", g_cfg.titles ? "on" : "off");
        PUT("  [-/+] screenshot every     %u ms\033[K\n", g_cfg.interval_ms);
        PUT("  [d] sessions folder        %s\033[K\n", g_cfg.dir[0] ? fit(g_cfg.dir, line, sizeof line, 29) : "(TickSessions next to tick)");
        PUT("\033[K\n  [Esc] save and back\033[K\n");
    } else if (mode == 3) {
        PUT("  %s\033[K\n", typing_for == 1 ? "Program, .desktop file or menu name (Enter = start, Esc = back):"
                                              : "Sessions folder (Enter = set, Esc = back):");
        PUT("  %s_\033[K\n", fit(typed, line, sizeof line, 3));
    } else {
        const char *q = app_question();
        if (q) {
            const char *nl = strchr(q, '\n');
            char first[700];
            snprintf(first, sizeof first, "%.*s", nl ? (int)(nl - q) : (int)strlen(q), q);
            PUT("  \033[1m%s\033[0m\033[K\n", fit(first, line, sizeof line, 2));
            if (nl) PUT("  %s\033[K\n", fit(nl + 1, line, sizeof line, 2));
            PUT("  [y] yes   [n] no\033[K\n");
        }
        else if (msg_until && p_ms() < msg_until) PUT("  \033[34m%s\033[0m\033[K\n", fit(msg, line, sizeof line, 2));
        else if (rec) {
            u32 sec = (p_ms() - rec_start) / 1000, kb = session_bytes_kb();
            PUT("  Recording %02u:%02u:%02u - %ld screenshots, %u %s, %ld marks\033[K\n", sec / 3600, sec / 60 % 60, sec % 60,
                g_frames_stored, kb < 1024 ? kb : kb / 1024, kb < 1024 ? "kB" : "MB", g_mark_n);
        } else PUT("  Not recording.\033[K\n");
        if (!q) {
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
            } else PUT("  Ports too? [p] start the program from Tick.\033[K\n\033[K\n");
        }
        if (note_mode) PUT("\033[K\n  Note for mark (Enter = mark, Esc = cancel): %s_\033[K\n", note);
        else PUT("\033[K\n  [s] %s   [m] mark   [y] sync   [p] program   [o] settings   [q] quit\033[K\n", rec ? "stop" : "start");
    }
    PUT("\033[J");
#undef PUT
    if (write(1, out, o) < 0) {}
}

/* a read from the terminal: arrow keys (ESC [ A/B, also ESC O A/B) move in the list */
static void key(int c);
static void keys(const unsigned char *c, ssize_t n)
{
    ssize_t j;
    for (j = 0; j < n; j++) {
        if (c[j] == 27 && j + 2 < n && (c[j + 1] == '[' || c[j + 1] == 'O')) {
            if (mode == 1 && c[j + 2] == 'A' && psel > 0) psel--;
            if (mode == 1 && c[j + 2] == 'B' && psel + 1 < pn) psel++;
            if (mode == 1 && c[j + 2] == '5' && psel > 0) psel = psel > 10 ? psel - 10 : 0;
            if (mode == 1 && c[j + 2] == '6' && pn) psel = psel + 10 < pn ? psel + 10 : pn - 1;
            j += 2;
            while (j + 1 < n && c[j] >= '0' && c[j] <= '9') j++;      /* ESC [ 5 ~ */
            continue;
        }
        key(c[j]);
    }
}

static void open_list(void)
{
    x11 x;
    char err[300], titles[64][160];
    int pids[64], nw = 0;
    if (plist) lprogs_free(plist, pn);
    if (x11_open(&x, err, sizeof err)) { nw = x11_client_windows(&x, pids, titles, 64); x11_close(&x); }
    pn = lprogs_list(&plist, pids, titles, nw);
    psel = ptop = 0;
    mode = 1;
}

static void key(int c)
{
    if (mode == 3) {
        size_t n = strlen(typed);
        if (c == '\r' || c == '\n') {
            if (typing_for == 1) { mode = 0; app_open(typed); }
            else { copy_str(g_cfg.dir, typed, sizeof g_cfg.dir); mode = 2; }
        } else if (c == 27) mode = typing_for == 1 ? 1 : 2;
        else if ((c == 127 || c == 8) && n) typed[n - 1] = 0;
        else if (c >= 32 && n + 1 < sizeof typed) { typed[n] = (char)c; typed[n + 1] = 0; }
        return;
    }
    if (mode == 1) {
        if (c == 27) { mode = 0; return; }
        if ((c == '\r' || c == '\n') && pn) { lprog p = lprog_copy(&plist[psel]); mode = 0; app_choose(&p); lprog_free_strings(p.argv); lprog_free_strings(p.envp); }
        else if (c == 'w' || c == 'W') { mode = 0; show("Click the program's window (20 s)."); draw(); app_pick(); }
        else if (c == 'f' || c == 'F') { typed[0] = 0; typing_for = 1; mode = 3; }
        else if (c == 'k' && psel > 0) psel--;
        else if (c == 'j' && psel + 1 < pn) psel++;
        return;
    }
    if (mode == 2) {
        if (c == 27) { mode = 0; if (!cfg_save()) show("Settings could not be saved."); else show("Settings saved."); }
        else if (c == '1') g_screen_on = !g_screen_on;
        else if (c == '2') g_cfg.rec_serial = !g_cfg.rec_serial;
        else if (c == '3') g_cfg.rec_net = !g_cfg.rec_net;
        else if (c == '4') g_cfg.rec_usb = !g_cfg.rec_usb;
        else if (c == '5') g_cfg.titles = !g_cfg.titles;
        else if (c == '+' && g_cfg.interval_ms < 600000) g_cfg.interval_ms += g_cfg.interval_ms < 1000 ? 100 : 500;
        else if (c == '-' && g_cfg.interval_ms > 100) g_cfg.interval_ms -= g_cfg.interval_ms <= 1000 ? 100 : 500;
        else if (c == 'd' || c == 'D') { copy_str(typed, g_cfg.dir, sizeof typed); typing_for = 2; mode = 3; }
        return;
    }
    if (app_question()) {
        if (c == 'y' || c == 'Y') app_answer(1);
        else if (c == 'n' || c == 'N' || c == 27) app_answer(0);
        return;
    }
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
    case 'p': case 'P':
        if (listener_running()) show("Tick is already listening to a program.");
        else open_list();
        break;
    case 'o': case 'O': mode = 2; break;
    case 'q': case 'Q': case 3: quit = 1; break;
    }
}

/* ---------------- control socket ---------------- */

static int ctl_listen(void)
{
    struct sockaddr_un a;
    int dfd, fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (!ctl_addr(&a, ctl_path, &dfd)) { close(fd); return -1; }
    {   /* a stale socket of a Tick that did not exit cleanly */
        int t = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (t >= 0 && connect(t, (struct sockaddr *)&a, sizeof a) < 0) unlink(ctl_path);
        if (t >= 0) close(t);
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) { close(fd); if (dfd >= 0) close(dfd); return -1; }
    if (dfd >= 0) close(dfd);
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
    sb_str(&b, ",\"screen\":"); sb_jsonz(&b, g_screen_on ? capture_error() : "off");
    sb_str(&b, session_write_failed() ? ",\"write_failed\":true" : ",\"write_failed\":false");
    sb_str(&b, ",\"question\":"); sb_jsonz(&b, app_question() ? app_question() : "");
    sb_str(&b, ",\"message\":"); sb_jsonz(&b, app_message());
    sb_str(&b, "}\n");
    copy_str(out, b.p ? b.p : "{}\n", (u32)cap);
    sb_free(&b);
}

static void ctl_handle(int c)
{
    char in[600], out[16000];
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
    } else if (!strcmp(in, "programs")) {     /* what the program list shows, for scripts */
        lprog *l;
        int k, cnt = lprogs_list(&l, 0, 0, 0);
        sbuf b;
        sb_init(&b, 4096);
        sb_str(&b, "{\"ok\":true,\"programs\":[");
        for (k = 0; k < cnt && b.len < sizeof out - 300; k++) {
            if (k) sb_putc(&b, ',');
            sb_str(&b, "{\"label\":"); sb_jsonz(&b, l[k].label);
            sb_str(&b, ",\"command\":"); sb_jsonz(&b, l[k].detail);
            sb_str(&b, l[k].running ? ",\"running\":true}" : ",\"running\":false}");
        }
        sb_str(&b, "]}\n");
        copy_str(out, b.p, sizeof out);
        sb_free(&b);
        lprogs_free(l, cnt);
        goto send;
    } else if (!strcmp(in, "yes") || !strcmp(in, "no")) {   /* answer the window's question (tests, agents) */
        app_answer(!strcmp(in, "yes"));
    } else if (!strncmp(in, "open ", 5)) {     /* start a program like Browse does */
        app_open(in + 5);
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
    char cmd[600], buf[17000];
    int fd, i;
    ssize_t n;
    size_t o = 0;
    if (!ctl_path[0]) default_ctl_path(ctl_path, sizeof ctl_path);
    cmd[0] = 0;
    for (i = 0; i < argc; i++) o += (size_t)snprintf(cmd + o, sizeof cmd - o, "%s%s", i ? " " : "", argv[i]);
    if (!argc) { copy_str(cmd, "status", sizeof cmd); }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    {
        int dfd;
        if (!ctl_addr(&a, ctl_path, &dfd)) { fprintf(stderr, "tick ctl: bad path %s\n", ctl_path); return 1; }
        if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); fd = -1; }
        if (dfd >= 0) close(dfd);
    }
    if (fd < 0) {
        fprintf(stderr, "tick ctl: no running tick at %s\n", ctl_path);
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) return 1;
    {
        size_t got = 0;
        while (got < sizeof buf - 1 && (n = read(fd, buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)n;
        buf[got] = 0;
        n = (ssize_t)got;
    }
    if (n > 0) fputs(buf, stdout);
    close(fd);
    return n > 0 && strstr(buf, "\"ok\":true") ? 0 : 1;
}

/* ---------------- main ---------------- */

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    int i, run = 0, start = 0, no_tui = 0, until_exit = 0, started_prog = 0, want_gui = 0;
    char **prog = 0, err[400];
    ssize_t n;

    n = readlink("/proc/self/exe", g_exe_dir, sizeof g_exe_dir - 1);
    if (n > 0) {
        g_exe_dir[n] = 0;
        while (n > 0 && g_exe_dir[n - 1] != '/') n--;
        g_exe_dir[n] = 0;
    } else copy_str(g_exe_dir, "./", sizeof g_exe_dir);
    cfg_load();
    ini_cfg = g_cfg;
    ini_screen = g_screen_on;

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
        else if (!strcmp(a, "--no-screen")) g_screen_on = 0;
        else if (!strcmp(a, "--no-titles")) g_cfg.titles = 0;
        else if (!strcmp(a, "--start")) start = 1;
        else if (!strcmp(a, "--no-tui")) no_tui = 1;
        else if (!strcmp(a, "--gui")) want_gui = 1;
        else if (!strcmp(a, "--until-exit")) until_exit = 1;
        else if (!strcmp(a, "--no-serial")) g_cfg.rec_serial = 0;
        else if (!strcmp(a, "--no-net")) g_cfg.rec_net = 0;
        else if (!strcmp(a, "--no-usb")) g_cfg.rec_usb = 0;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-') { fprintf(stderr, "tick: unknown option %s\n", a); usage(); return 2; }
        else break;
    }
    if (!run && i < argc) run = 2;      /* a file given to tick (opened with it from a file manager): as run */
    if (run) {
        if (i >= argc) { usage(); return 2; }
        prog = argv + i;
    }
    if (g_cfg.interval_ms < 100) g_cfg.interval_ms = 100;
    if (g_cfg.full_every < 1) g_cfg.full_every = 1;
    cfg_mark_cli();

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
    /* the window when started from a desktop or file manager (no terminal), or with --gui */
    gui = want_gui || (!no_tui && !isatty(0) && getenv("DISPLAY") && *getenv("DISPLAY"));
    if (gui && !gui_open(err, sizeof err)) {
        if (want_gui) { fprintf(stderr, "tick: %s\n", err); return 1; }
        gui = 0;
    }
    tui = !gui && !no_tui && isatty(0) && isatty(1);
    if (gui) g_mask_window = gui_window();
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
        lprog lp;
        int resolved = 0;
        size_t l0 = strlen(prog[0]);
        /* a .desktop file, or a name that is no program on the PATH but an applications-menu entry */
        if (!prog[1] && ((l0 > 8 && !strcmp(prog[0] + l0 - 8, ".desktop")) || (!strchr(prog[0], '/') && !desktop_is_command(prog[0])))) {
            if (lprog_resolve(prog[0], &lp, err, sizeof err)) resolved = 1;
            else { if (tui || gui) show(err); else fprintf(stderr, "tick: %s\n", err); prog = 0; }
        }
        if (resolved) {
            if (listener_run_in(lp.argv, lp.cwd[0] ? lp.cwd : 0, 0, tui || gui, err, sizeof err)) started_prog = 1;
            else if (tui || gui) show(err);
            else fprintf(stderr, "tick: %s\n", err);
            lprog_free_strings(lp.argv);
        } else if (prog) {
            if (listener_run(prog, tui || gui, err, sizeof err)) started_prog = 1;
            else if (tui || gui) show(err);
            else fprintf(stderr, "tick: %s\n", err);
        }
    }
    if (tui) tty_raw();
    while (!quit) {
        struct pollfd p[3];
        int np = 0;
        if (tui) { p[np].fd = 0; p[np].events = POLLIN; np++; }
        if (ctl_fd >= 0) { p[np].fd = ctl_fd; p[np].events = POLLIN; np++; }
        if (gui) { p[np].fd = gui_fd(); p[np].events = POLLIN; np++; }
        if (tui) draw();
        if (gui) { gui_events(); gui_draw(); if (gui_closed()) quit = 1; }
        app_poll();
        if (poll(p, (nfds_t)np, 100) > 0) {
            int k;
            for (k = 0; k < np; k++) {
                if (!(p[k].revents & POLLIN)) continue;
                if (gui && p[k].fd == gui_fd()) continue;       /* handled by gui_events() */
                if (p[k].fd == 0) {
                    unsigned char c[16];
                    ssize_t r = read(0, c, sizeof c);
                    if (r > 0) keys(c, r);
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
    if (gui) gui_close();
    if (ctl_fd >= 0) { close(ctl_fd); unlink(ctl_path); }
    if (session_dir()[0]) printf("tick: session in %s\n", session_dir());
    return 0;
}
