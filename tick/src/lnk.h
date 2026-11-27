/* lnk.h - reading Windows shortcut files (.lnk, MS-SHLLINK) without COM, so Tick
 * can start a program from its Start menu or desktop shortcut on Windows 95..11.
 * Portable (no Windows calls): the Unicode strings in a shortcut are turned
 * into the caller's 8-bit text by a function the caller passes (Windows: the
 * system code page; tests: UTF-8). */
#ifndef TICK_LNK_H
#define TICK_LNK_H
#include "rt.h"

#define LNK_TEXT 520

typedef void (*lnk_w2a)(const u16 *w, u32 n, char *out, u32 cap);

typedef struct {
    char target[LNK_TEXT];      /* the program the shortcut starts ("" if none found) */
    char args[LNK_TEXT];        /* its arguments */
    char workdir[LNK_TEXT];     /* "Start in" folder ("" = none given) */
    char name[LNK_TEXT];        /* the shortcut's description (comment), often empty */
    char relative[LNK_TEXT];    /* target relative to the shortcut's folder, if given */
    int target_has_env;         /* target came from an %ENVIRONMENT% path: expand it before use */
    int from;                   /* where the target came from: 1 LinkInfo, 2 env block, 3 item list, 4 relative */
} lnk_info;

/* 1 if buf is a shortcut and something useful was read; 0 if it is not a shortcut */
int lnk_parse(const u8 *buf, u32 n, lnk_info *out, lnk_w2a w2a);

#endif
