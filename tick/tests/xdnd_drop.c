/* xdnd_drop.c - test helper: drags files onto a window the way a file manager does (XDND v5)
 *   xdnd_drop WINDOW URI-LIST-TEXT [X Y]      (X Y: pointer position on the screen; default 100 100)
 * Prints "status accept=N" and "finished accepted=N". Uses Tick's own X11 client (x11.c). */
#include "../src/posix/x11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 g32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static x11 X;

static void client_message(u32 to, u32 type, u32 l0, u32 l1, u32 l2, u32 l3, u32 l4)
{
    u8 r[44];
    memset(r, 0, sizeof r);
    r[0] = 25; p16(r + 2, 11); p32(r + 4, to);
    r[12] = 33; r[13] = 32; p32(r + 16, to); p32(r + 20, type);
    p32(r + 24, l0); p32(r + 28, l1); p32(r + 32, l2); p32(r + 36, l3); p32(r + 40, l4);
    x11_send(&X, r, 44);
}

static const char *g_data;
static u32 g_sel;

/* answer a request for the dragged data (a target may ask before the drop, as Wine does) */
static void serve(const u8 *ev)
{
    u32 req = g32(ev + 12), prop = g32(ev + 24), target_atom = g32(ev + 20), n = (u32)strlen(g_data);
    u8 *c = calloc(1, 24 + n + 4), sn[44];
    if (!prop) prop = target_atom;                      /* obsolete requestors */
    c[0] = 18; p16(c + 2, (u16)(6 + (n + 3) / 4)); p32(c + 4, req); p32(c + 8, prop);
    p32(c + 12, target_atom); c[16] = 8; p32(c + 20, n);   /* ChangeProperty */
    memcpy(c + 24, g_data, n);
    x11_send(&X, c, 24 + (n + 3) / 4 * 4);
    free(c);
    memset(sn, 0, sizeof sn);
    sn[0] = 25; p16(sn + 2, 11); p32(sn + 4, req);
    sn[12] = 31; p32(sn + 16, g32(ev + 4)); p32(sn + 20, req); p32(sn + 24, g_sel);
    p32(sn + 28, target_atom); p32(sn + 32, prop);     /* SelectionNotify */
    x11_send(&X, sn, 44);
}

/* waits for a ClientMessage of that type (10 s), serving data requests meanwhile */
static int wait_for(u32 msgtype, u8 *ev)
{
    int i;
    for (i = 0; i < 100; i++) {
        int k = x11_next_event(&X, ev, 100);
        if (k < 0) return 0;
        if (k > 0 && (ev[0] & 0x7f) == 30) { serve(ev); i = 0; continue; }
        if (k > 0 && (ev[0] & 0x7f) == 33 && g32(ev + 8) == msgtype) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char err[300];
    u8 r[64], ev[32];
    u32 target, src, enter, pos, status, drop, finished, sel, copy, uri;
    const char *data;
    u32 px = 100, py = 100;
    if (argc < 3) { fprintf(stderr, "usage: xdnd_drop WINDOW URI-LIST\n"); return 2; }
    target = (u32)strtoul(argv[1], 0, 0);
    data = argv[2];
    if (argc > 4) { px = (u32)atoi(argv[3]); py = (u32)atoi(argv[4]); }
    if (!x11_open(&X, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    enter = x11_atom(&X, "XdndEnter"); pos = x11_atom(&X, "XdndPosition"); status = x11_atom(&X, "XdndStatus");
    drop = x11_atom(&X, "XdndDrop"); finished = x11_atom(&X, "XdndFinished"); sel = x11_atom(&X, "XdndSelection");
    copy = x11_atom(&X, "XdndActionCopy"); uri = x11_atom(&X, "text/uri-list");
    src = x11_new_id(&X);
    memset(r, 0, sizeof r);
    r[0] = 1; r[1] = 0; p16(r + 2, 8); p32(r + 4, src); p32(r + 8, X.root);   /* CreateWindow, InputOnly */
    p16(r + 16, 1); p16(r + 18, 1); p16(r + 22, 2);
    x11_send(&X, r, 32);
    memset(r, 0, sizeof r);
    r[0] = 22; p16(r + 2, 4); p32(r + 4, src); p32(r + 8, sel);               /* SetSelectionOwner */
    x11_send(&X, r, 16);
    {   /* XDND version: 5 (GTK, Qt); XDND_VERSION=N to try another */
        const char *v = getenv("XDND_VERSION");
        client_message(target, enter, src, (u32)(v ? atoi(v) : 5) << 24, uri, 0, 0);
    }
    client_message(target, pos, src, 0, px << 16 | py, 0, copy);
    g_data = data;
    g_sel = sel;
    if (!wait_for(status, ev)) { printf("no status\n"); return 1; }
    printf("status accept=%u\n", g32(ev + 16) & 1);
    client_message(target, drop, src, 0, 0, 0, 0);
    if (!wait_for(finished, ev)) { printf("no answer to the drop\n"); return 1; }
    printf("finished accepted=%u\n", g32(ev + 16) & 1);
    return 0;
}
