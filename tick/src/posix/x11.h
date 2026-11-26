/* x11.h - minimal X11 protocol client (see x11.c) */
#ifndef TICK_X11_H
#define TICK_X11_H
#include "../rt.h"

typedef struct {
    int fd;
    u32 rid, root, root_visual;
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

#endif
