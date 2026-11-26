/* plat_posix.c - Linux (POSIX) side of the platform layer (core.h) */
#define _GNU_SOURCE
#include "../core.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

void ansi_to_utf8(sbuf *b, const char *s) { sb_str(b, s); }     /* Linux text is UTF-8 already */

void *pf_create(const char *path, int must_be_new)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (must_be_new ? O_EXCL : O_TRUNC), 0644);
    return fd < 0 ? 0 : (void *)(long)(fd + 1);
}

int pf_write(void *f, const void *p, u32 n)
{
    int fd = (int)(long)f - 1;
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        c += w;
        n -= (u32)w;
    }
    return 1;
}

void pf_flush(void *f) { fdatasync((int)(long)f - 1); }
void pf_close(void *f) { close((int)(long)f - 1); }

int p_mkdir(const char *path)
{
    struct stat st;
    mkdir(path, 0755);
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int p_mkdir_new(const char *path)
{
    if (mkdir(path, 0755) == 0) return 1;
    return errno == EEXIST ? 0 : -1;
}

void p_rmdir(const char *path) { rmdir(path); }
void p_unlink(const char *path) { unlink(path); }

u32 p_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)((u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000);
}

void p_hostname(char *out, u32 cap)
{
    struct utsname u;
    if (uname(&u) == 0) copy_str(out, u.nodename, cap);
    else copy_str(out, "", cap);
}

void p_os(sbuf *b)
{
    struct utsname u;
    if (uname(&u) != 0) { sb_str(b, "Linux"); return; }
    sb_str(b, u.sysname); sb_putc(b, ' ');
    sb_str(b, u.release); sb_putc(b, ' ');
    sb_str(b, u.machine);
}

static pthread_mutex_t mu;
static pthread_once_t mu_once = PTHREAD_ONCE_INIT;

static void mu_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mu, &a);
}

void p_lock(void) { pthread_once(&mu_once, mu_init); pthread_mutex_lock(&mu); }
void p_unlock(void) { pthread_mutex_unlock(&mu); }

int p_documents(char *out, u32 cap)
{
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    copy_str(out, h, cap);
    return 1;
}

int p_temp(char *out, u32 cap)
{
    const char *t = getenv("TMPDIR");
    copy_str(out, t && *t ? t : "/tmp", cap);
    return 1;
}
