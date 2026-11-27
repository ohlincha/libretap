/* core.h - what Tick's portable parts share on every platform (Windows, Linux):
 * settings, clock, session writer, event lines.  Platform code: plat.h. */
#ifndef TICK_CORE_H
#define TICK_CORE_H
#include "rt.h"

#ifdef _WIN32
#define TICK_PATH 260           /* MAX_PATH */
#define PSEP "\\"
#else
#define TICK_PATH 1024
#define PSEP "/"
#endif

#define TICK_VERSION "0.4.1"
#ifdef TICK_SCREEN_ONLY
#define TICK_NAME    "TickEyes " TICK_VERSION
#else
#define TICK_NAME    "Tick " TICK_VERSION
#endif

/* ---- settings (Tick.ini next to Tick.exe, section [tick]) ---- */
typedef struct {
    u32 interval_ms;        /* screenshot period, default 500 */
    u32 full_every;         /* full picture every Nth stored screenshot, default 60 */
    u32 tile;               /* diff tile size, default 32 */
    int cursor;             /* draw the mouse pointer into screenshots */
    int titles;             /* record the active window's title */
    int mask_self;          /* paint Tick's own window grey in screenshots */
    int topmost;            /* keep Tick above other windows */
    int program_only;       /* screenshots show only the vendor program's windows */
    int copy_settings;      /* copy the started program's settings files into files/ */
    int rec_serial, rec_net, rec_usb;   /* kinds the listener records (all on by default) */
    char dir[TICK_PATH];     /* sessions folder; "" = TickSessions next to Tick.exe */
} tick_cfg;

extern tick_cfg g_cfg;
extern char g_exe_dir[TICK_PATH];       /* folder of the Tick program, with trailing separator */

void cfg_load(void);
int cfg_save(void);                     /* 0 if Tick.ini could not be written */

/* ---- clock (clock.c) ---- */
void clock_init(void);
u64 clock_now_us(void);                 /* Unix time, microseconds */
typedef struct { int year, mon, day, hour, min, sec, ms, wday; } tick_tm;
void clock_tm(u64 us, tick_tm *t);      /* local time */
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
extern volatile long g_frames_stored, g_mark_n, g_sync_n;

void session_set_capture(const char *process);   /* program-only mode: whose windows */

/* ---- helpers ---- */
void ansi_to_utf8(sbuf *b, const char *s);      /* text in the system's code page -> UTF-8 */
void copy_str(char *d, const char *s, u32 cap);
void cat_str(char *d, const char *s, u32 cap);

/* ---- platform layer (plat_win.c / posix/plat_posix.c) ---- */
void *pf_create(const char *path, int must_be_new);    /* NULL on failure */
int pf_write(void *f, const void *p, u32 n);
void pf_flush(void *f);
void pf_close(void *f);
int p_mkdir(const char *path);          /* 1 if the folder exists afterwards */
int p_mkdir_new(const char *path);      /* 1 created, 0 exists already, -1 failed */
void p_rmdir(const char *path);
void p_unlink(const char *path);
u32 p_ms(void);                         /* monotonic milliseconds */
void p_hostname(char *out, u32 cap);
void p_os(sbuf *b);                     /* "Windows 5.0 build 2195", "Linux 6.12.1 x86_64" */
void p_lock(void);
void p_unlock(void);
int p_documents(char *out, u32 cap);    /* user's documents (Windows) or home (Linux) folder */
int p_temp(char *out, u32 cap);

#endif
