/* tick.h - shared declarations of Tick.exe (Windows only) */
#ifndef TICK_H
#define TICK_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"

#include "core.h"

#define TICK_HAS_WIN 1
extern HWND g_hwnd;
extern int g_win9x;
extern DWORD g_winver;                  /* GetVersion() */

/* Windows-only clock helpers (clock.c) */
u64 clock_qpc_to_us(u64 qpc);
u64 clock_ft_to_us(u64 ft);
void clock_local(u64 us, SYSTEMTIME *st);
void process_name(DWORD pid, char *out);         /* exe name of a process, "" if unknown */

/* ---- screen capture (capture.c) ---- */
extern volatile DWORD g_pick_pid;
int capture_start(void);
void capture_stop(void);

/* ---- program listener (launch.c) ---- */
int launch_program(const char *exe, const char *args, const char *workdir, char *err, u32 errcap);   /* workdir NULL: the program's folder */
int attach_process(DWORD pid, const char *name, char *err, u32 errcap);
int listener_running(void);
void listener_session_changed(void);    /* recording started/stopped */
void listener_detach_all(void);         /* Tick is closing */
const char *listener_status(void);      /* short text for the window */
DWORD listener_pid(void);               /* process listened to, 0 if none */
const char *listener_program(void);     /* its exe name */

/* ---- choosing the program (programs.c) ---- */
void programs_dialog(HINSTANCE hi);     /* "Start a program..." */
void programs_open_file(const char *path);  /* .exe or .lnk from Browse, a drop or the command line */
void programs_picked(DWORD pid);        /* the window picked belongs to pid */
void programs_timer(void);              /* every 50 ms: a restart in progress */
void programs_accept_drops(HWND w);
void programs_drop(HANDLE h);         /* HDROP */

/* ---- tick.c, for programs.c ---- */
int tick_start_recording(void);
void tick_show_msg(const char *m);
void tick_pick_window(int for_program); /* 1: then offer to listen to it; 0: screenshots of it only */

#define WM_TICK_LISTENER (WM_APP + 1)   /* listener state changed */
#define WM_TICK_ERROR    (WM_APP + 2)   /* lParam: static error text */


#endif
