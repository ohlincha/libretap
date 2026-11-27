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
/* the same in a working folder and with an environment (NULL: Tick's own) */
int listener_run_in(char **argv, const char *cwd, char **envp, int quiet_output, char *err, u32 errcap);
int listener_running(void);
int listener_pid(void);
void listener_session_changed(void);
void listener_detach_all(void);
const char *listener_status(void);

/* the app (tick_linux.c): what the terminal clock and the window both do */
#include "programs_posix.h"
int app_choose(const lprog *p);         /* 1 started or refused (message), 2 asks first (app_question) */
extern int g_screen_on;
int app_start(void);
void app_stop(void);
int app_mark(const char *note);
int app_sync(void);
void app_show(const char *m);
const char *app_message(void);          /* "" when none */
int app_flash(void);                    /* sync number while the clock flashes, else 0 */
u32 app_rec_seconds(void);
const char *app_question(void);         /* "Restart X under Tick...?" waiting for yes/no, else NULL */
void app_answer(int yes);
int app_pick(void);                     /* waits for a click on a window (20 s) */
int app_open(const char *what);         /* a program, .desktop file or menu name */
void app_poll(void);
const char *app_ini_path(void);

/* the window (gui_x11.c) */
int gui_open(char *err, u32 errcap);
int gui_fd(void);
u32 gui_window(void);
void gui_events(void);
void gui_draw(void);
int gui_closed(void);
void gui_close(void);

#endif
