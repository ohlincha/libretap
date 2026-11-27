/* desktop.c - see desktop.h */
#define _GNU_SOURCE
#include "desktop.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>

static void unescape_value(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            out[o++] = *s == 's' ? ' ' : *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
        } else out[o++] = *s;
    }
    out[o] = 0;
}

static int truth(const char *v) { return !strcmp(v, "true") || !strcmp(v, "1"); }

int desktop_parse(const char *text, desktop_entry *d)
{
    const char *p = text;
    int in_group = 0, app = 0;
    char line[4096];
    memset(d, 0, sizeof *d);
    while (*p) {
        size_t n = strcspn(p, "\n");
        char *k, *v, *eq;
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p += n + (p[n] == '\n');
        { size_t l = strlen(line); if (l && line[l - 1] == '\r') line[l - 1] = 0; }
        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (!*k || *k == '#') continue;
        if (*k == '[') { in_group = !strncmp(k, "[Desktop Entry]", 15); continue; }
        if (!in_group || !(eq = strchr(k, '='))) continue;
        v = eq + 1;
        while (eq > k && (eq[-1] == ' ' || eq[-1] == '\t')) eq--;
        *eq = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcmp(k, "Name")) unescape_value(v, d->name, sizeof d->name);
        else if (!strcmp(k, "Exec")) unescape_value(v, d->exec, sizeof d->exec);
        else if (!strcmp(k, "Path")) unescape_value(v, d->path, sizeof d->path);
        else if (!strcmp(k, "Type")) app = !strcmp(v, "Application");
        else if (!strcmp(k, "Terminal")) d->terminal = truth(v);
        else if (!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) d->hidden |= truth(v);
    }
    return app && d->exec[0];
}

int desktop_load(const char *file, desktop_entry *d)
{
    FILE *f = fopen(file, "rb");
    char *buf;
    size_t n;
    int ok;
    const char *b;
    if (!f) return 0;
    buf = malloc(256 * 1024);
    if (!buf) { fclose(f); return 0; }
    n = fread(buf, 1, 256 * 1024 - 1, f);
    fclose(f);
    buf[n] = 0;
    ok = desktop_parse(buf, d);
    free(buf);
    snprintf(d->file, sizeof d->file, "%s", file);
    b = strrchr(file, '/');
    snprintf(d->id, sizeof d->id, "%s", b ? b + 1 : file);
    if (strlen(d->id) > 8 && !strcmp(d->id + strlen(d->id) - 8, ".desktop")) d->id[strlen(d->id) - 8] = 0;
    return ok;
}

void desktop_free_argv(char **argv)
{
    int i;
    if (!argv) return;
    for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

/* Exec rules: arguments separated by spaces; "..." quotes an argument, in which \" \` \$ \\
 * stand for the character; %f %F %u %U (files to open), %i %c %k and the deprecated codes
 * expand to nothing here (Tick starts the program without a file); %% is a percent sign. */
char **desktop_argv(const desktop_entry *d)
{
    const char *s = d->exec;
    char **v = calloc(64, sizeof *v), cur[2048];
    int n = 0, bad = 0;
    if (!v) return 0;
    while (*s && n < 62) {
        size_t o = 0;
        int quoted = 0, any = 0, only_code = 1;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        while (*s && (quoted || (*s != ' ' && *s != '\t')) && o + 1 < sizeof cur) {
            if (*s == '"') { quoted = !quoted; s++; any = 1; only_code = 0; continue; }
            if (quoted && *s == '\\' && s[1]) { cur[o++] = s[1]; s += 2; only_code = 0; continue; }
            if (*s == '%' && s[1]) {
                if (s[1] == '%') { cur[o++] = '%'; only_code = 0; }
                s += 2;                             /* field code: nothing */
                continue;
            }
            cur[o++] = *s++;
            only_code = 0;
        }
        if (quoted) bad = 1;
        cur[o] = 0;
        if (o || (any && !only_code)) v[n++] = strdup(cur);
    }
    v[n] = 0;
    if (bad || !n) { desktop_free_argv(v); return 0; }
    return v;
}

/* applications folders in order of precedence */
static int app_dirs(char dirs[][1024], int max)
{
    const char *h = getenv("XDG_DATA_HOME"), *home = getenv("HOME"), *dd = getenv("XDG_DATA_DIRS");
    int n = 0;
    if (h && *h) snprintf(dirs[n++], 1024, "%s/applications", h);
    else if (home) snprintf(dirs[n++], 1024, "%s/.local/share/applications", home);
    if (!dd || !*dd) dd = "/usr/local/share:/usr/share";
    while (*dd && n < max) {
        size_t l = strcspn(dd, ":");
        if (l) {
            snprintf(dirs[n], 1024, "%.*s%sapplications", (int)l, dd, dd[l - 1] == '/' ? "" : "/");
            n++;
        }
        dd += l + (dd[l] == ':');
    }
    return n;
}

typedef struct { desktop_entry *e; int n, cap; } elist;

static int seen(const elist *l, const char *id)
{
    int i;
    for (i = 0; i < l->n; i++) if (!strcmp(l->e[i].id, id)) return 1;
    return 0;
}

static void scan(const char *dir, const char *prefix, elist *l, int depth)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    if (!dp) return;
    while ((de = readdir(dp))) {
        char path[2048], id[512];
        size_t nl = strlen(de->d_name);
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) {
                char pre[512];
                snprintf(pre, sizeof pre, "%s%s-", prefix, de->d_name);
                scan(path, pre, l, depth + 1);
            }
            continue;
        }
        if (nl < 9 || strcmp(de->d_name + nl - 8, ".desktop")) continue;
        snprintf(id, sizeof id, "%s%.*s", prefix, (int)(nl - 8), de->d_name);
        if (seen(l, id)) continue;                  /* an earlier folder has it (the user's own copy) */
        if (l->n == l->cap) {
            desktop_entry *ne = realloc(l->e, (size_t)(l->cap ? l->cap * 2 : 64) * sizeof *ne);
            if (!ne) break;
            l->e = ne;
            l->cap = l->cap ? l->cap * 2 : 64;
        }
        {
            desktop_entry *e = &l->e[l->n];
            int ok = desktop_load(path, e);
            snprintf(e->id, sizeof e->id, "%.255s", id);
            if (ok || e->hidden) l->n++;            /* hidden ones are kept so they shadow later copies */
            if (!ok) e->hidden = 1;
        }
    }
    closedir(dp);
}

static void collect(elist *l)
{
    char dirs[16][1024];
    int n = app_dirs(dirs, 16), i;
    for (i = 0; i < n; i++) scan(dirs[i], "", l, 0);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const desktop_entry *)a)->name, ((const desktop_entry *)b)->name);
}

int desktop_list(desktop_entry **out)
{
    elist l = {0, 0, 0};
    int i, k = 0;
    collect(&l);
    for (i = 0; i < l.n; i++) if (!l.e[i].hidden && l.e[i].name[0]) l.e[k++] = l.e[i];
    if (k) qsort(l.e, (size_t)k, sizeof *l.e, by_name);
    *out = l.e;
    return k;
}

int desktop_find(const char *what, desktop_entry *d)
{
    elist l = {0, 0, 0};
    char want[512];
    size_t wl;
    int i, found = 0;
    struct stat st;
    if (strchr(what, '/') && stat(what, &st) == 0 && S_ISREG(st.st_mode)) return desktop_load(what, d);
    snprintf(want, sizeof want, "%s", what);
    wl = strlen(want);
    if (wl > 8 && !strcmp(want + wl - 8, ".desktop")) want[wl - 8] = 0;
    collect(&l);
    for (i = 0; i < l.n && !found; i++)             /* the id first ... */
        if (!strcmp(l.e[i].id, want) && l.e[i].exec[0]) { *d = l.e[i]; found = 1; }
    for (i = 0; i < l.n && !found; i++)             /* ... then the name the menu shows */
        if (!strcasecmp(l.e[i].name, want) && l.e[i].exec[0] && !l.e[i].hidden) { *d = l.e[i]; found = 1; }
    for (i = 0; i < l.n && !found; i++) {           /* ... then the id's last part (org.vendor.Soft -> soft) */
        const char *dot = strrchr(l.e[i].id, '.');
        if (dot && !strcasecmp(dot + 1, want) && l.e[i].exec[0] && !l.e[i].hidden) { *d = l.e[i]; found = 1; }
    }
    free(l.e);
    return found;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int uri_list_first_path(const char *list, char *out, unsigned cap)
{
    const char *l = list;
    while (*l) {
        const char *e = l + strcspn(l, "\r\n"), *u = l, *path;
        unsigned o = 0;
        if (*l == '#' || e == l) { l = e + strspn(e, "\r\n"); continue; }      /* comment, empty line */
        if (strncasecmp(u, "file:", 5)) return 0;                              /* the first item is not a file */
        u += 5;
        if (u[0] == '/' && u[1] == '/') {                                       /* file://host/path */
            const char *h = u + 2, *slash = h;
            char host[256], me[256];
            while (slash < e && *slash != '/') slash++;
            if (slash >= e) return 0;
            snprintf(host, sizeof host, "%.*s", (int)(slash - h), h);
            me[0] = 0;
            gethostname(me, sizeof me - 1);
            me[sizeof me - 1] = 0;
            if (host[0] && strcasecmp(host, "localhost") && strcasecmp(host, me)) return 0;   /* another computer's file */
            path = slash;
        } else if (u[0] == '/') {
            path = u;                                                          /* file:/path */
        } else {
            return 0;
        }
        for (; path < e; path++) {
            char c = *path;
            if (c == '%') {
                int a = path + 2 < e ? hexv(path[1]) : -1, b = a >= 0 ? hexv(path[2]) : -1;
                if (a < 0 || b < 0 || (a == 0 && b == 0)) return 0;             /* malformed, or a NUL byte */
                c = (char)(a * 16 + b);
                path += 2;
            }
            if (o + 1 >= cap) return 0;
            out[o++] = c;
        }
        out[o] = 0;
        return o > 0;
    }
    return 0;
}
