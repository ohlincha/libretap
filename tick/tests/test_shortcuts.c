/* test_shortcuts.c - native tests of the shortcut readers: Windows .lnk files built byte by
 * byte here as Windows 95 (ANSI), XP..11 (Unicode, item list with long names), network and
 * %ProgramFiles% shortcuts write them, damaged copies of each; and Linux .desktop files
 * (escapes, quoting, field codes, finding by id and by menu name).
 * Usage: test_shortcuts SCRATCHDIR */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/lnk.h"
#include "../src/posix/desktop.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKS(a, b) do { if (strcmp((a), (b))) { printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

/* UTF-16 -> UTF-8 (what the tests compare against) */
static void w2u(const u16 *w, u32 n, char *out, u32 cap)
{
    sbuf b;
    sb_init(&b, 64);
    sb_utf16(&b, w, n);
    snprintf(out, cap, "%s", b.p ? b.p : "");
    sb_free(&b);
}

/* ---- a little byte builder ---- */
typedef struct { u8 b[8192]; u32 n; } buf;
static void b8(buf *x, u32 v) { x->b[x->n++] = (u8)v; }
static void b16(buf *x, u32 v) { b8(x, v); b8(x, v >> 8); }
static void b32(buf *x, u32 v) { b16(x, v); b16(x, v >> 16); }
static void bstr(buf *x, const char *s) { while (*s) b8(x, (u8)*s++); b8(x, 0); }
static void bmem(buf *x, const void *p, u32 n) { memcpy(x->b + x->n, p, n); x->n += n; }
/* UTF-8 text as UTF-16 code units (BMP only), without terminator; returns units */
static u32 bwide(buf *x, const char *s)
{
    u32 n = 0;
    const u8 *p = (const u8 *)s;
    while (*p) {
        u32 c = *p++;
        if (c >= 0xc0 && c < 0xe0) c = (c & 0x1f) << 6 | (*p++ & 0x3f);
        else if (c >= 0xe0) { c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f); p += 2; }
        b16(x, c);
        n++;
    }
    return n;
}
static void set32(buf *x, u32 at, u32 v) { x->b[at] = (u8)v; x->b[at + 1] = (u8)(v >> 8); x->b[at + 2] = (u8)(v >> 16); x->b[at + 3] = (u8)(v >> 24); }
static void set16(buf *x, u32 at, u32 v) { x->b[at] = (u8)v; x->b[at + 1] = (u8)(v >> 8); }

static void header(buf *x, u32 flags)
{
    static const u8 clsid[16] = {0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    x->n = 0;
    b32(x, 0x4C);
    bmem(x, clsid, 16);
    b32(x, flags);
    while (x->n < 0x4C) b8(x, 0);
}

static void sdata_ansi(buf *x, const char *s) { b16(x, (u32)strlen(s)); bmem(x, s, (u32)strlen(s)); }
static void sdata_wide(buf *x, const char *s)
{
    u32 at = x->n, n;
    b16(x, 0);
    n = bwide(x, s);
    set16(x, at, n);
}

/* LinkInfo with a local path (ANSI, and Unicode when wide != NULL) */
static void link_info_local(buf *x, const char *ansi, const char *wide)
{
    u32 start = x->n, hsize = wide ? 0x24 : 0x1C, vol, lbp, cps, lbpu = 0, cpsu = 0;
    b32(x, 0); b32(x, hsize); b32(x, 1);
    b32(x, 0); b32(x, 0); b32(x, 0); b32(x, 0);
    if (wide) { b32(x, 0); b32(x, 0); }
    vol = x->n - start;                             /* VolumeID: size, type, serial, label offset, label */
    b32(x, 0x11); b32(x, 3); b32(x, 0x1234abcd); b32(x, 0x10); b8(x, 0);
    lbp = x->n - start; bstr(x, ansi);
    cps = x->n - start; b8(x, 0);
    if (wide) {
        if (x->n & 1) b8(x, 0);
        lbpu = x->n - start; bwide(x, wide); b16(x, 0);
        cpsu = x->n - start; b16(x, 0);
    }
    set32(x, start, x->n - start);
    set32(x, start + 12, vol); set32(x, start + 16, lbp); set32(x, start + 24, cps);
    if (wide) { set32(x, start + 28, lbpu); set32(x, start + 32, cpsu); }
}

static void test_lnk_win95(void)
{
    buf x;
    lnk_info o;
    header(&x, 0x2 | 0x4 | 0x10 | 0x20);            /* LinkInfo, Name, WorkingDir, Arguments; ANSI */
    link_info_local(&x, "C:\\PROGRA~1\\VENDOR\\ACQ.EXE", 0);
    sdata_ansi(&x, "Acquisition");
    sdata_ansi(&x, "C:\\PROGRA~1\\VENDOR");
    sdata_ansi(&x, "/port COM1 \"my file.cfg\"");
    b32(&x, 0);                                     /* end of ExtraData */
    CHECK(lnk_parse(x.b, x.n, &o, w2u));
    CHECKS(o.target, "C:\\PROGRA~1\\VENDOR\\ACQ.EXE");
    CHECKS(o.args, "/port COM1 \"my file.cfg\"");
    CHECKS(o.workdir, "C:\\PROGRA~1\\VENDOR");
    CHECKS(o.name, "Acquisition");
    CHECK(o.from == 1 && !o.target_has_env);
}

/* item list: root (My Computer), volume, folders and file with 0xBEEF0004 long names */
static void item_file(buf *x, int folder, const char *shortn, const char *longn, u32 pre_name_field)
{
    u32 start = x->n, ext, n;
    b16(x, 0);
    b8(x, folder ? 0x31 : 0x32);
    b8(x, 0);
    b32(x, folder ? 0 : 123456);
    b32(x, 0x5b2c4a10);
    b16(x, folder ? 0x10 : 0x20);
    bstr(x, shortn);
    if ((x->n - start) & 1) b8(x, 0);               /* 2-aligned within the item */
    ext = x->n;
    b16(x, 0); b16(x, 3); b32(x, 0xBEEF0004u);
    b32(x, 0x5b2c4a10); b32(x, 0x5b2c4a10);
    b16(x, pre_name_field);                         /* a field before the name (0x14 or a value >= 0x20) */
    n = bwide(x, longn); (void)n;
    b16(x, 0);
    b16(x, ext - start);                            /* offset of the extension block */
    set16(x, ext, x->n - ext);
    set16(x, start, x->n - start);
}

static void id_list(buf *x, u32 pre)
{
    static const u8 mycomp[16] = {0xe0, 0x4f, 0xd0, 0x20, 0xea, 0x3a, 0x69, 0x10, 0xa2, 0xd8, 0x08, 0x00, 0x2b, 0x30, 0x30, 0x9d};
    u32 start = x->n, it;
    b16(x, 0);
    b16(x, 20); b8(x, 0x1f); b8(x, 0x50); bmem(x, mycomp, 16);
    it = x->n; b16(x, 0); b8(x, 0x2f); bstr(x, "C:\\"); while (x->n - it < 25) b8(x, 0); set16(x, it, x->n - it);
    item_file(x, 1, "PROGRA~1", "Program Files", pre);
    item_file(x, 1, "VENDOR~1", "Vendor Soft", pre);
    item_file(x, 0, "ACQUIS~1.EXE", "Acquisition Ström.exe", pre);
    b16(x, 0);                                      /* terminal item */
    set16(x, start, x->n - start - 2);
}

static void test_lnk_xp_unicode(void)
{
    buf x;
    lnk_info o;
    header(&x, 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x80);
    id_list(&x, 0x14);
    link_info_local(&x, "C:\\Program Files\\Vendor Soft\\Acquisition Str?m.exe",
                    "C:\\Program Files\\Vendor Soft\\Acquisition Ström.exe");
    sdata_wide(&x, "..\\..\\..\\Program Files\\Vendor Soft\\Acquisition Ström.exe");
    sdata_wide(&x, "C:\\Program Files\\Vendor Soft");
    sdata_wide(&x, "-instrument 2");
    b32(&x, 0);
    CHECK(lnk_parse(x.b, x.n, &o, w2u));
    CHECKS(o.target, "C:\\Program Files\\Vendor Soft\\Acquisition Ström.exe");   /* the Unicode path wins */
    CHECKS(o.args, "-instrument 2");
    CHECKS(o.workdir, "C:\\Program Files\\Vendor Soft");
    CHECK(o.relative[0] == '.');
    CHECK(o.from == 1);
}

static void test_lnk_idlist_only(u32 pre)
{
    buf x;
    lnk_info o;
    header(&x, 0x1 | 0x100);                        /* item list only (ForceNoLinkInfo) */
    id_list(&x, pre);
    b32(&x, 0);
    CHECK(lnk_parse(x.b, x.n, &o, w2u));
    CHECKS(o.target, "C:\\Program Files\\Vendor Soft\\Acquisition Ström.exe");
    CHECK(o.from == 3);
}

static void test_lnk_network(void)
{
    buf x;
    lnk_info o;
    u32 start, cn;
    header(&x, 0x2);
    start = x.n;
    b32(&x, 0); b32(&x, 0x1C); b32(&x, 2);
    b32(&x, 0); b32(&x, 0); b32(&x, 0); b32(&x, 0);
    cn = x.n - start;
    b32(&x, 0x14 + 16); b32(&x, 2); b32(&x, 0x14); b32(&x, 0); b32(&x, 0x20000);
    bstr(&x, "\\\\labserver\\apps");
    while ((x.n - start) % 4) b8(&x, 0);
    set32(&x, start + 24, x.n - start);
    bstr(&x, "vendor\\acq.exe");
    set32(&x, start, x.n - start);
    set32(&x, start + 20, cn);
    b32(&x, 0);
    CHECK(lnk_parse(x.b, x.n, &o, w2u));
    CHECKS(o.target, "\\\\labserver\\apps\\vendor\\acq.exe");
}

static void test_lnk_env(void)
{
    buf x;
    lnk_info o;
    u32 at, k;
    header(&x, 0x200 | 0x80);                       /* HasExpString, Unicode */
    at = x.n;
    b32(&x, 0x314); b32(&x, 0xA0000001u);
    for (k = 0; k < 260; k++) b8(&x, 0);
    memcpy(x.b + at + 8, "%ProgramFiles%\\Vendor\\acq.exe", 29);
    for (k = 0; k < 520; k++) b8(&x, 0);
    {
        buf w;
        w.n = 0;
        bwide(&w, "%ProgramFiles%\\Vendor\\acq.exe");
        memcpy(x.b + at + 268, w.b, w.n);
    }
    b32(&x, 0);
    CHECK(lnk_parse(x.b, x.n, &o, w2u));
    CHECKS(o.target, "%ProgramFiles%\\Vendor\\acq.exe");
    CHECK(o.target_has_env && o.from == 2);
}

static void test_lnk_damaged(void)
{
    buf x;
    lnk_info o;
    u32 cut, i;
    header(&x, 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x80);
    id_list(&x, 0x14);
    link_info_local(&x, "C:\\A.EXE", "C:\\A.EXE");
    sdata_wide(&x, "a.exe");
    sdata_wide(&x, "C:\\");
    sdata_wide(&x, "x");
    b32(&x, 0);
    for (cut = 0; cut <= x.n; cut++) lnk_parse(x.b, cut, &o, w2u);       /* every truncation: no crash */
    for (i = 0x4C; i < x.n; i++) {                                         /* every byte flipped */
        u8 keep = x.b[i];
        x.b[i] ^= 0xff;
        lnk_parse(x.b, x.n, &o, w2u);
        x.b[i] = keep;
    }
    CHECK(!lnk_parse((const u8 *)"not a shortcut at all, just text in a file............................", 70, &o, w2u));
}

/* ---- .desktop ---- */

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void test_desktop(const char *scratch)
{
    desktop_entry d, *all = 0;
    char **v, dir[1024], p[1200], env[2200];
    int n, i, found;
    CHECK(desktop_parse("[Desktop Entry]\r\nType=Application\r\nName=Vendor Acquire\r\n"
                        "Exec=\"/opt/Vendor Soft/bin/acq\" --port /dev/ttyUSB0 \"a \\\\\"quoted\\\\\" arg\" %U\r\n"
                        "Path=/opt/Vendor Soft\r\nTerminal=false\r\n[Desktop Action New]\r\nExec=other\r\n", &d));
    CHECKS(d.name, "Vendor Acquire");
    CHECKS(d.path, "/opt/Vendor Soft");
    v = desktop_argv(&d);
    CHECK(v != 0);
    if (v) {
        for (n = 0; v[n]; n++) ;
        CHECK(n == 4);
        CHECKS(v[0], "/opt/Vendor Soft/bin/acq");
        CHECKS(v[1], "--port");
        CHECKS(v[2], "/dev/ttyUSB0");
        if (n > 3) CHECKS(v[3], "a \"quoted\" arg");
        desktop_free_argv(v);
    }
    CHECK(desktop_parse("[Desktop Entry]\nType=Application\nName=Pct\nExec=calc --rate 50%% -f %f\n", &d));
    v = desktop_argv(&d);
    CHECK(v && !strcmp(v[2], "50%") && !strcmp(v[3], "-f") && !v[4]);
    desktop_free_argv(v);
    CHECK(!desktop_parse("[Desktop Entry]\nType=Link\nName=Web\nURL=http://x\n", &d));
    CHECK(!desktop_parse("[Desktop Entry]\nType=Application\nName=NoExec\n", &d));
    CHECK(desktop_parse("[Desktop Entry]\nType=Application\nName=Bad\nExec=\"unclosed\n", &d));
    CHECK(desktop_argv(&d) == 0);
    CHECK(desktop_parse("[Desktop Entry]\nType=Application\nName=Hid\nExec=x\nNoDisplay=true\n", &d) && d.hidden);

    /* a menu: the user's folder shadows the system one; sub-folders give ids with '-' */
    snprintf(dir, sizeof dir, "%s/home", scratch);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/applications", dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys", scratch); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/applications", scratch); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/applications/vendor", scratch); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/applications/vendor/acquire.desktop", scratch);
    write_file(p, "[Desktop Entry]\nType=Application\nName=Acquire (system)\nExec=/usr/bin/acq\n");
    snprintf(p, sizeof p, "%s/sys/applications/org.example.Viewer.desktop", scratch);
    write_file(p, "[Desktop Entry]\nType=Application\nName=Spectrum Viewer\nExec=viewer %F\n");
    snprintf(p, sizeof p, "%s/sys/applications/hidden.desktop", scratch);
    write_file(p, "[Desktop Entry]\nType=Application\nName=Helper\nExec=helper\nNoDisplay=true\n");
    snprintf(p, sizeof p, "%s/home/applications/vendor-acquire.desktop", scratch);
    write_file(p, "[Desktop Entry]\nType=Application\nName=Acquire\nExec=/home/user/acq-fixed\n");
    snprintf(env, sizeof env, "%s/home", scratch); setenv("XDG_DATA_HOME", env, 1);
    snprintf(env, sizeof env, "%s/sys", scratch); setenv("XDG_DATA_DIRS", env, 1);
    CHECK(desktop_find("vendor-acquire", &d) && !strcmp(d.exec, "/home/user/acq-fixed"));
    CHECK(desktop_find("vendor-acquire.desktop", &d) && !strcmp(d.exec, "/home/user/acq-fixed"));
    CHECK(desktop_find("spectrum viewer", &d) && !strcmp(d.id, "org.example.Viewer"));
    CHECK(desktop_find("Viewer", &d) && !strcmp(d.name, "Spectrum Viewer"));
    CHECK(desktop_find(p, &d) && !strcmp(d.name, "Acquire"));
    CHECK(!desktop_find("Helper", &d));
    CHECK(!desktop_find("nothing-like-this", &d));
    n = desktop_list(&all);
    CHECK(n == 2);
    for (i = 0, found = 0; i < n; i++) found += !strcmp(all[i].name, "Acquire") || !strcmp(all[i].name, "Spectrum Viewer");
    CHECK(found == 2);
    free(all);
}

int main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : "/tmp";
    test_lnk_win95();
    test_lnk_xp_unicode();
    test_lnk_idlist_only(0x14);
    test_lnk_idlist_only(0x2E);                     /* a field before the name that looks like a character */
    test_lnk_network();
    test_lnk_env();
    test_lnk_damaged();
    test_desktop(scratch);
    if (fails) { printf("%d shortcut test(s) FAILED\n", fails); return 1; }
    printf("shortcut tests passed\n");
    return 0;
}
