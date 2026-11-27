/* programs_posix.c - choosing the program Tick listens to on Linux: the applications menu
 * (.desktop files), programs running now that use a serial port, USB device or network
 * connection (from /proc), programs with a window (from the window manager), and restarting a
 * running program under Tick (closed with SIGTERM, as a desktop's close does; never killed). */
#define _GNU_SOURCE
#include "programs_posix.h"
#include "desktop.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char **dup_argv(char **v)
{
    int n = 0, i;
    char **o;
    if (!v) return 0;
    while (v[n]) n++;
    o = calloc((size_t)n + 1, sizeof *o);
    if (!o) return 0;
    for (i = 0; i < n; i++) o[i] = strdup(v[i]);
    return o;
}

void lprog_free_strings(char **v)
{
    int i;
    if (!v) return;
    for (i = 0; v[i]; i++) free(v[i]);
    free(v);
}

void lprogs_free(lprog *l, int n)
{
    int i;
    for (i = 0; i < n; i++) { lprog_free_strings(l[i].argv); lprog_free_strings(l[i].envp); }
    free(l);
}

/* NUL-separated file (cmdline, environ) -> NULL-terminated list */
static char **read_nul_list(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf, **v;
    size_t n, i, k = 0, cnt = 0;
    if (!f) return 0;
    buf = malloc(1 << 20);
    if (!buf) { fclose(f); return 0; }
    n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    if (!n) { free(buf); return 0; }
    if (buf[n - 1]) buf[n++] = 0;
    for (i = 0; i < n; i++) if (!buf[i]) cnt++;
    v = calloc(cnt + 1, sizeof *v);
    if (!v) { free(buf); return 0; }
    for (i = 0; i < n && k < cnt; i += strlen(buf + i) + 1) v[k++] = strdup(buf + i);
    free(buf);
    return v;
}

static int read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = 0; return 0; }
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return out[0] != 0;
}

/* sockets with a connection to another computer: inode -> "192.168.0.20:5000" */
typedef struct { unsigned long ino; char peer[64]; } conn;

static int tcp_conns(conn *c, int max)
{
    static const char *files[2] = {"/proc/net/tcp", "/proc/net/tcp6"};
    int k = 0, f;
    for (f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        char line[512];
        if (!fp) continue;
        if (!fgets(line, sizeof line, fp)) { fclose(fp); continue; }
        while (k < max && fgets(line, sizeof line, fp)) {
            char rem[64];
            unsigned st, port;
            unsigned long ino;
            char *colon;
            if (sscanf(line, "%*d: %*s %63s %x %*s %*s %*s %*d %*d %lu", rem, &st, &ino) != 3) continue;
            if (st != 1 || !ino) continue;          /* ESTABLISHED only */
            colon = strchr(rem, ':');
            if (!colon) continue;
            port = (unsigned)strtoul(colon + 1, 0, 16);
            *colon = 0;
            if (strlen(rem) == 8) {
                unsigned a = (unsigned)strtoul(rem, 0, 16);
                struct in_addr in;
                in.s_addr = a;
                if ((ntohl(a) >> 24) == 127) continue;  /* this computer itself */
                snprintf(c[k].peer, sizeof c[k].peer, "%s:%u", inet_ntoa(in), port);
            } else {
                if (!strcmp(rem, "00000000000000000000000001000000") ||
                    !strncmp(rem, "0000000000000000FFFF00000100007F", 32)) continue;
                snprintf(c[k].peer, sizeof c[k].peer, "IPv6 port %u", port);
            }
            if (port == 443 || port == 53) continue;  /* web pages and name lookups: not instruments */
            c[k].ino = ino;
            k++;
        }
        fclose(fp);
    }
    return k;
}

/* what a process uses: 2 a device (serial, USB), 1 a network connection, 0 neither */
static int uses_of(int pid, const conn *c, int nc, char *what, size_t cap)
{
    char d[64];
    DIR *dp;
    struct dirent *de;
    int best = 0;
    what[0] = 0;
    snprintf(d, sizeof d, "/proc/%d/fd", pid);
    dp = opendir(d);
    if (!dp) return 0;
    while ((de = readdir(dp))) {
        char l[300], t[300];
        ssize_t n;
        if (de->d_name[0] == '.') continue;
        snprintf(l, sizeof l, "%s/%s", d, de->d_name);
        n = readlink(l, t, sizeof t - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (!strncmp(t, "/dev/ttyUSB", 11) || !strncmp(t, "/dev/ttyACM", 11) || !strncmp(t, "/dev/ttyS", 9) ||
            !strncmp(t, "/dev/serial/", 12) || !strncmp(t, "/dev/bus/usb/", 13) || !strncmp(t, "/dev/hidraw", 11) ||
            !strncmp(t, "/dev/usbtmc", 11) || !strncmp(t, "/dev/usb/", 9) || !strncmp(t, "/dev/ttyAMA", 11)) {
            if (best < 2) snprintf(what, cap, "%s", t);
            best = 2;
        } else if (best < 1 && !strncmp(t, "socket:[", 8)) {
            unsigned long ino = strtoul(t + 8, 0, 10);
            int i;
            for (i = 0; i < nc; i++)
                if (c[i].ino == ino) { snprintf(what, cap, "network %s", c[i].peer); best = 1; break; }
        }
    }
    closedir(dp);
    return best;
}

static void base_name(const char *p, char *out, size_t cap)
{
    const char *b = strrchr(p, '/');
    snprintf(out, cap, "%s", b ? b + 1 : p);
}

int lprog_running(int pid, lprog *p)
{
    char path[64], exe[1024], name[256];
    ssize_t n;
    memset(p, 0, sizeof *p);
    p->running = 1;
    p->pid = pid;
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    p->argv = read_nul_list(path);
    if (!p->argv) return 0;
    snprintf(path, sizeof path, "/proc/%d/environ", pid);
    p->envp = read_nul_list(path);
    snprintf(path, sizeof path, "/proc/%d/cwd", pid);
    n = readlink(path, p->cwd, sizeof p->cwd - 1);
    p->cwd[n > 0 ? n : 0] = 0;
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    n = readlink(path, exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    snprintf(p->exe, sizeof p->exe, "%s", exe);
    base_name(exe[0] ? exe : p->argv[0], name, sizeof name);
    {   /* a script: its own name, not the interpreter's (python3 /usr/bin/solaar -> solaar) */
        static const char *interp[] = {"python", "perl", "ruby", "node", "java", "tclsh", "wish", "mono", 0};
        int k, j;
        for (k = 0; interp[k]; k++)
            if (!strncmp(name, interp[k], strlen(interp[k]))) {
                for (j = 1; p->argv[j]; j++)
                    if (p->argv[j][0] != '-') { base_name(p->argv[j], name, sizeof name); break; }
                break;
            }
    }
    snprintf(p->name, sizeof p->name, "%s", name);
    {
        int i;
        size_t o = 0;
        for (i = 0; p->argv[i] && o < sizeof p->detail - 2; i++)
            o += (size_t)snprintf(p->detail + o, sizeof p->detail - o, "%s%s", i ? " " : "", p->argv[i]);
    }
    return 1;
}

/* a program file given by path or name on the PATH -> its full path ("" if none) */
static void which(const char *prog, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    out[0] = 0;
    if (strchr(prog, '/')) { if (!realpath(prog, out)) out[0] = 0; return; }
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        size_t l = strcspn(path, ":");
        char c[2048];
        snprintf(c, sizeof c, "%.*s/%s", (int)l, path, prog);
        if (access(c, X_OK) == 0) { if (!realpath(c, out)) snprintf(out, cap, "%s", c); return; }
        path += l + (path[l] == ':');
    }
}

int lprog_resolve(const char *what, lprog *p, char *err, unsigned errcap)
{
    desktop_entry d;
    struct stat st;
    size_t l = strlen(what);
    memset(p, 0, sizeof *p);
    if ((l > 8 && !strcmp(what + l - 8, ".desktop")) || (!strchr(what, '/') && desktop_find(what, &d))) {
        if (l > 8 && !strcmp(what + l - 8, ".desktop") && !desktop_find(what, &d)) {
            snprintf(err, errcap, "Not an application: %s", what);
            return 0;
        }
        p->argv = desktop_argv(&d);
        if (!p->argv) { snprintf(err, errcap, "%s: no command to run.", d.name); return 0; }
        snprintf(p->cwd, sizeof p->cwd, "%s", d.path);
        snprintf(p->name, sizeof p->name, "%s", d.name);
        snprintf(p->label, sizeof p->label, "%s", d.name);
        snprintf(p->detail, sizeof p->detail, "%s", d.exec);
        which(p->argv[0], p->exe, sizeof p->exe);
        p->terminal = d.terminal;
        return 1;
    }
    which(what, p->exe, sizeof p->exe);
    if (!p->exe[0] || stat(p->exe, &st) < 0 || !S_ISREG(st.st_mode) || access(p->exe, X_OK) < 0) {
        snprintf(err, errcap, "Not a program: %s", what);
        return 0;
    }
    {
        char *v[2];
        v[0] = (char *)what; v[1] = 0;
        p->argv = dup_argv(v);
    }
    base_name(p->exe, p->name, sizeof p->name);
    snprintf(p->label, sizeof p->label, "%s", p->name);
    snprintf(p->detail, sizeof p->detail, "%s", p->exe);
    return 1;
}

/* programs that run other programs: their file says nothing about which program it is, so a
 * running one is never taken for the menu entry (sh -c "...", python3 script, env, flatpak, wine) */
static int generic(const char *exe)
{
    static const char *g[] = {"sh", "bash", "dash", "zsh", "ksh", "csh", "tcsh", "fish", "busybox", "env", "python", "perl", "ruby",
                              "node", "java", "mono", "wine", "wine64", "wine-preloader", "wine64-preloader", "flatpak",
                              "flatpak-spawn", "snap", "bwrap", "xdg-open", "gio", "gtk-launch", "exo-open", "kioclient5",
                              "tclsh", "wish", "lua", "php", "pkexec", "sudo", "nohup", "setsid", "nice", "ionice", "timeout", 0};
    const char *b = strrchr(exe, '/');
    char base[256];
    int i;
    snprintf(base, sizeof base, "%s", b ? b + 1 : exe);
    for (i = 0; g[i]; i++) {
        size_t l = strlen(g[i]);
        if (!strncmp(base, g[i], l) && (!base[l] || (base[l] >= '0' && base[l] <= '9') || base[l] == '.')) return 1;
    }
    return 0;
}

int lprog_find_running(const char *exe)
{
    DIR *dp;
    struct dirent *de;
    int found = 0;
    if (!exe[0] || generic(exe)) return 0;
    dp = opendir("/proc");
    if (!dp) return 0;
    while ((de = readdir(dp)) && !found) {
        char l[64], t[1024];
        ssize_t n;
        int pid = atoi(de->d_name);
        if (pid <= 1 || pid == getpid()) continue;
        {
            struct stat st;
            snprintf(l, sizeof l, "/proc/%d", pid);
            if (stat(l, &st) < 0 || st.st_uid != getuid()) continue;   /* only this user's programs */
        }
        snprintf(l, sizeof l, "/proc/%d/exe", pid);
        n = readlink(l, t, sizeof t - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (!strcmp(t, exe)) found = pid;
    }
    closedir(dp);
    return found;
}

int lprog_close(int pid) { return kill(pid, SIGTERM) == 0; }

int lprog_alive(int pid)
{
    char path[64], st[300];
    if (kill(pid, 0) < 0 && errno == ESRCH) return 0;
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_line(path, st, sizeof st)) {           /* a zombie has ended */
        char *r = strrchr(st, ')');
        if (r && r[1] == ' ' && r[2] == 'Z') return 0;
    }
    return 1;
}

static int by_label(const void *a, const void *b)
{
    return strcasecmp(((const lprog *)a)->label, ((const lprog *)b)->label);
}

int lprogs_list(lprog **out, const int *win_pids, char (*win_titles)[160], int nwin)
{
    lprog *l = calloc(1024, sizeof *l);
    int n = 0, run_end, i, k;
    conn *c = calloc(512, sizeof *c);
    int nc = c ? tcp_conns(c, 512) : 0;
    DIR *dp;
    struct dirent *de;
    desktop_entry *apps = 0;
    int napps;
    *out = l;
    if (!l) { free(c); return 0; }
    /* running programs of this user that use a device or a network connection, or have a window */
    dp = opendir("/proc");
    while (dp && (de = readdir(dp)) && n < 200) {
        int pid = atoi(de->d_name), use, w = -1;
        char what[300], path[64];
        struct stat st;
        if (pid <= 1 || pid == getpid() || pid == getppid()) continue;
        snprintf(path, sizeof path, "/proc/%d", pid);
        if (stat(path, &st) < 0 || st.st_uid != getuid()) continue;
        {   /* web browsers and remote shells are not instrument software */
            static const char *skip[] = {"chrome", "chromium", "firefox", "firefox-esr", "thunderbird", "ssh", "sshd",
                                         "Xorg", "Xwayland", "pipewire", "pulseaudio", "systemd", "dbus-daemon", 0};
            char comm[64];
            int z, skipit = 0;
            snprintf(path, sizeof path, "/proc/%d/comm", pid);
            read_line(path, comm, sizeof comm);
            for (z = 0; skip[z]; z++) if (!strcmp(comm, skip[z])) skipit = 1;
            if (skipit) continue;
        }
        use = uses_of(pid, c, nc, what, sizeof what);
        for (k = 0; k < nwin; k++) if (win_pids[k] == pid) { w = k; break; }
        if (!use && w < 0) continue;
        if (!lprog_running(pid, &l[n])) continue;
        l[n].uses = use;
        if (use) snprintf(l[n].label, sizeof l[n].label, "Running: %s (%s)", l[n].name, what);
        else snprintf(l[n].label, sizeof l[n].label, "Running: %s (%s)", win_titles[w], l[n].name);
        n++;
    }
    if (dp) closedir(dp);
    free(c);
    /* devices first, then network, then windows */
    for (i = 1; i < n; i++) {
        lprog t = l[i];
        int j = i;
        while (j > 0 && (l[j - 1].uses < t.uses || (l[j - 1].uses == t.uses && strcasecmp(l[j - 1].label, t.label) > 0))) {
            l[j] = l[j - 1];
            j--;
        }
        l[j] = t;
    }
    run_end = n;
    napps = desktop_list(&apps);
    for (i = 0; i < napps && n < 1024; i++) {
        lprog *p = &l[n];
        memset(p, 0, sizeof *p);
        p->argv = desktop_argv(&apps[i]);
        if (!p->argv) continue;
        snprintf(p->label, sizeof p->label, "%s", apps[i].name);
        snprintf(p->name, sizeof p->name, "%s", apps[i].name);
        snprintf(p->detail, sizeof p->detail, "%s", apps[i].exec);
        snprintf(p->cwd, sizeof p->cwd, "%s", apps[i].path);
        which(p->argv[0], p->exe, sizeof p->exe);
        p->terminal = apps[i].terminal;
        n++;
    }
    free(apps);
    if (n > run_end) qsort(l + run_end, (size_t)(n - run_end), sizeof *l, by_label);
    return n;
}

/* the application entry that starts exe (its menu command and folder), or -1 */
int lprogs_app_for(const lprog *l, int n, const char *exe)
{
    int i;
    for (i = 0; i < n; i++) if (!l[i].running && l[i].exe[0] && !strcmp(l[i].exe, exe)) return i;
    return -1;
}

lprog lprog_copy(const lprog *p)
{
    lprog c = *p;
    c.argv = dup_argv(p->argv);
    c.envp = dup_argv(p->envp);
    return c;
}
