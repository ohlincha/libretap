/* tickx.h - Linux Tick: declarations shared by its files */
#ifndef TICKX_H
#define TICKX_H
#include "../core.h"

/* screen (capture_x11.c) */
int capture_start(void);                /* 0 if there is no usable X display (reason in capture_error) */
void capture_stop(void);
const char *capture_error(void);
extern unsigned long g_mask_window;     /* X window to paint grey (Tick's own terminal), 0 = none */

/* listener (listener_posix.c) */
int listener_run(char **argv, int quiet_output, char *err, u32 errcap);
int listener_running(void);
int listener_pid(void);
void listener_session_changed(void);
void listener_detach_all(void);
const char *listener_status(void);

#endif
