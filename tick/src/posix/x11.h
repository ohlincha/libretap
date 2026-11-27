/* x11.h - minimal X11 protocol client (see x11.c) */
#ifndef TICK_X11_H
#define TICK_X11_H
#include "../rt.h"

#define X11_EVQ 64

typedef struct {
    int fd;
    u32 rid, rid_mask, id_next, seq, root, root_visual;
    u8 min_kc, max_kc;
    u8 evq[X11_EVQ][32];
    int evh, evt;
    u32 w, h, depth, bpp, pad;
    u32 rmask, gmask, bmask;
    int lsb;
    int nfmt;
    u8 fmt_depth[8], fmt_bpp[8], fmt_pad[8];
    u8 *buf;
    u32 bufcap;
    u32 a_active, a_name, a_wmname;
} x11;

int x11_open(x11 *x, char *err, u32 errcap);       /* connects to $DISPLAY */
void x11_close(x11 *x);
int x11_get_image(x11 *x, u8 *bgrx);               /* whole root window, w*h*4 bytes */
int x11_active_title(x11 *x, char *out, u32 cap);  /* -1: no window manager info */
int x11_window_rect(x11 *x, u32 win, int *rx, int *ry, int *rw, int *rh);

/* for a window of Tick's own (gui_x11.c) and picking windows */
int x11_send(x11 *x, const void *req, u32 n);       /* one request; counts the sequence */
u8 *x11_reply(x11 *x, u32 *extra);                  /* reply to the last request, 0 on its error */
u32 x11_new_id(x11 *x);
u32 x11_atom(x11 *x, const char *name);             /* made if missing */
int x11_prop(x11 *x, u32 win, u32 property, void *out, u32 max, u32 *type);
int x11_next_event(x11 *x, u8 *ev, int timeout_ms); /* 1 event, 0 none, -1 connection lost */
/* the program of the window the operator clicks (crosshair pointer); 0 if none in time */
int x11_pick_pid(x11 *x, int timeout_ms, char *title, u32 tcap);
int x11_client_windows(x11 *x, int *pids, char (*titles)[160], int max);

#endif
