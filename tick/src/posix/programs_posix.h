/* programs_posix.h - see programs_posix.c */
#ifndef TICK_PROGRAMS_POSIX_H
#define TICK_PROGRAMS_POSIX_H

typedef struct {
    int running;                /* 1: running now */
    int pid;
    int uses;                   /* running: 2 uses a device, 1 a network connection, 0 has a window */
    int terminal;               /* the menu entry wants a terminal */
    char name[256];             /* "VendorSoft" */
    char label[400];            /* what the list shows */
    char detail[1200];          /* the command */
    char exe[1024];             /* the program file (full path, "" if unknown) */
    char cwd[1024];             /* working folder ("" = Tick's) */
    char **argv;                /* command (malloc'ed) */
    char **envp;                /* running: its environment (malloc'ed), else NULL */
} lprog;

int lprogs_list(lprog **out, const int *win_pids, char (*win_titles)[160], int nwin);
void lprogs_free(lprog *l, int n);
int lprogs_app_for(const lprog *l, int n, const char *exe);
int lprog_running(int pid, lprog *p);
int lprog_resolve(const char *what, lprog *p, char *err, unsigned errcap);   /* file, .desktop, menu name */
int lprog_find_running(const char *exe, int *copies);   /* pid of a running program from that file, 0 if none */
int lprog_close(int pid);                      /* SIGTERM (only a process Tick may see, see lprog_allowed) */
int lprog_allowed(int pid);                    /* 0 outside $TICK_TEST_ROOT_PID's descendants (tests) */
int lprog_alive(int pid);
lprog lprog_copy(const lprog *p);
void lprog_free_strings(char **v);

#endif
