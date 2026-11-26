/* tick.h - shared declarations of Tick.exe (Windows only) */
#ifndef TICK_H
#define TICK_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"

#define TICK_VERSION "0.1.0"
#define TICK_NAME    "Tick " TICK_VERSION

/* ---- settings (Tick.ini next to Tick.exe, section [tick]) ---- */
typedef struct {
    u32 interval_ms;        /* screenshot period, default 500 */
    u32 full_every;         /* full picture every Nth stored screenshot, default 60 */
    u32 tile;               /* diff tile size, default 32 */
    int cursor;             /* draw the mouse pointer into screenshots */
    int titles;             /* record the active window's title */
    int mask_self;          /* paint Tick's own window grey in screenshots */
    int topmost;            /* keep Tick above other windows */
    int copy_settings;      /* copy the started program's settings files into files/ */
    char dir[MAX_PATH];     /* sessions folder; "" = TickSessions next to Tick.exe */
} tick_cfg;

extern tick_cfg g_cfg;
extern HWND g_hwnd;
extern char g_exe_dir[MAX_PATH];        /* with trailing backslash */
extern int g_win9x;
extern DWORD g_winver;                  /* GetVersion() */

void cfg_load(void);
int cfg_save(void);                     /* 0 if Tick.ini could not be written */

/* ---- clock (clock.c) ---- */
void clock_init(void);
u64 clock_now_us(void);                 /* Unix time, microseconds */
u64 clock_qpc_to_us(u64 qpc);
u64 clock_ft_to_us(u64 ft);
void clock_local(u64 us, SYSTEMTIME *st);
int clock_utc_offset_min(void);
void clock_iso(sbuf *b, u64 us);        /* "2026-11-30T10:15:00.123+01:00" */

/* ---- session (session.c) ---- */
int session_start(char *err, u32 errcap);
void session_stop(void);
int session_active(void);
const char *session_dir(void);          /* with trailing backslash */
const char *session_root(void);         /* the folder that holds the sessions */
/* event line builder: ev_begin writes {"t":..,"kind":".."; the caller adds
 * ,"key":value pairs; ev_end closes, writes and flushes (thread safe). */
void ev_begin(sbuf *b, u64 t, const char *kind);
void ev_end(sbuf *b);
void ev_status(const char *kind, const char *text);    /* kind "status" or "error" */
/* register a source (JSON object text); rewrites session.json once */
void session_add_source(const char *id, const char *json_object);
int session_has_source(const char *id);
int session_write_file(const char *rel, const void *p, u32 n);
void session_mkdir(const char *rel);
u32 session_bytes_kb(void);
void session_flush_if_due(void);
int session_write_failed(void);
void session_set_note(const char *note);
extern volatile LONG g_frames_stored, g_mark_n, g_sync_n;

/* ---- screen capture (capture.c) ---- */
int capture_start(void);
void capture_stop(void);

/* ---- program listener (launch.c) ---- */
int launch_program(const char *exe, const char *args, char *err, u32 errcap);
int attach_process(DWORD pid, const char *name, char *err, u32 errcap);
int listener_running(void);
void listener_session_changed(void);    /* recording started/stopped */
void listener_detach_all(void);         /* Tick is closing */
const char *listener_status(void);      /* short text for the window */

#define WM_TICK_LISTENER (WM_APP + 1)   /* listener state changed */
#define WM_TICK_ERROR    (WM_APP + 2)   /* lParam: static error text */

/* ---- helpers ---- */
void ansi_to_utf8(sbuf *b, const char *s);
void copy_str(char *d, const char *s, u32 cap);

#endif
