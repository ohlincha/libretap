/* demo-instrument (Linux): the pretend instrument for trying Tick, on the host of a test VM (or any Linux PC).
 * DemoMeter.exe (in the VM, started from Tick) talks to it over the network, or over the VM's COM port when that
 * port is connected to this program. Same commands as DemoInstrument.exe:
 *   *IDN? -> DEMO-METER,2.1   INT n -> OK   GAIN n -> OK   MEAS? -> M 0.5418 V      (lines end in CR)
 * Listens on:
 *   TCP 5025                      network instrument   (DemoMeter: Network, address of this computer)
 *   TCP 5026                      serial line for a VM COM port in TCP mode (VirtualBox: Port Mode "TCP",
 *                                 "Connect to existing" on, address 127.0.0.1:5026; QEMU: -serial tcp:127.0.0.1:5026)
 *   /tmp/demo-instrument-com      the same as a Unix socket (VirtualBox: Port Mode "Host Pipe", "Connect to
 *                                 existing pipe/socket" on, that path)
 *   a pseudo-terminal             for programs on this computer (its path is printed)
 * and, with --vm-com HOST:PORT, connects to a VM's COM port in TCP server mode (VirtualBox: Port Mode "TCP",
 * "Connect to existing" off, port e.g. 5027), reconnecting when the VM restarts.
 * Build: cc -O2 -o demo-instrument instrument_linux.c -lm        Stop: Ctrl-C. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NET_PORT 5025
#define COM_PORT 5026
#define COM_PATH "/tmp/demo-instrument-com"
#define MAXC 16

struct conn { int fd; int kind; char buf[256]; int got; int ms, gain; };   /* kind: 0 network, 1 serial line */
static struct conn cs[MAXC];

static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }

static void answer(struct conn *c, const char *cmd, char *ans, size_t cap)
{
    if (!strncmp(cmd, "*IDN?", 5)) snprintf(ans, cap, "DEMO-METER,2.1\r\n");
    else if (!strncmp(cmd, "INT ", 4)) {
        int v = atoi(cmd + 4);
        if (v >= 10 && v <= 10000) { c->ms = v; snprintf(ans, cap, "OK\r\n"); } else snprintf(ans, cap, "ERR range\r\n");
    } else if (!strncmp(cmd, "GAIN ", 5)) {
        int v = atoi(cmd + 5);
        if (v == 1 || v == 2 || v == 4) { c->gain = v; snprintf(ans, cap, "OK\r\n"); } else snprintf(ans, cap, "ERR range\r\n");
    } else if (!strncmp(cmd, "MEAS?", 5)) {
        double v = 0.5 + 0.2 * sin(now() / 7.0) * c->ms / 1000.0 * c->gain;
        snprintf(ans, cap, "M %.4f V\r\n", v);
    } else snprintf(ans, cap, "ERR unknown command\r\n");
}

static int listen_tcp(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)) {
        fprintf(stderr, "port %d: %s (is demo-instrument already running?)\n", port, strerror(errno));
        exit(1);
    }
    return fd;
}

static int listen_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    unlink(path);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    return fd;
}

static int add(int fd, int kind, const char *what)
{
    for (int i = 0; i < MAXC; i++)
        if (cs[i].fd < 0) {
            cs[i] = (struct conn){fd, kind, {0}, 0, 500, 1};
            printf("%s connected\n", what);
            return i;
        }
    close(fd);
    return -1;
}

static void handle(struct conn *c)
{
    int n = (int)read(c->fd, c->buf + c->got, sizeof c->buf - 1 - c->got), start = 0;
    if (n <= 0) {
        if (c->kind != 2) { printf("%s disconnected\n", c->kind ? "serial line" : "network client"); close(c->fd); c->fd = -1; }
        return;
    }
    c->got += n;
    for (int i = 0; i < c->got; i++)
        if (c->buf[i] == '\r' || c->buf[i] == '\n') {
            c->buf[i] = 0;
            if (i > start) {
                char ans[64];
                answer(c, c->buf + start, ans, sizeof ans);
                if (write(c->fd, ans, strlen(ans)) < 0) {}
                printf("%-7s %-10s -> %.*s\n", c->kind == 0 ? "net" : "serial", c->buf + start, (int)strcspn(ans, "\r\n"), ans);
            }
            start = i + 1;
        }
    memmove(c->buf, c->buf + start, c->got - start);
    c->got -= start;
    if (c->got >= (int)sizeof c->buf - 1) c->got = 0;
}

static char vm_host[64];
static int vm_port, vm_idx = -1;
static double vm_next;

static void vm_try(void)   /* connect to the VM's COM port (TCP server in the VM software) */
{
    struct sockaddr_in a;
    int fd;
    if (!vm_port || (vm_idx >= 0 && cs[vm_idx].fd >= 0) || now() < vm_next) return;
    vm_next = now() + 2;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(vm_port);
    inet_pton(AF_INET, vm_host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return; }
    vm_idx = add(fd, 1, "VM COM port");
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--vm-com")) {
            char *c = strrchr(argv[i + 1], ':');
            if (!c) { fprintf(stderr, "--vm-com HOST:PORT\n"); return 2; }
            snprintf(vm_host, sizeof vm_host, "%.*s", (int)(c - argv[i + 1]), argv[i + 1]);
            vm_port = atoi(c + 1);
        }
    setvbuf(stdout, 0, _IOLBF, 0);
    for (int i = 0; i < MAXC; i++) cs[i].fd = -1;
    int net = listen_tcp(NET_PORT), com = listen_tcp(COM_PORT), pipe_fd = listen_unix(COM_PATH);
    /* a pseudo-terminal: a serial line for programs on this computer */
    int pty = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty >= 0 && !grantpt(pty) && !unlockpt(pty)) {
        struct termios t;
        int slave = open(ptsname(pty), O_RDWR | O_NOCTTY);   /* keeps the line up while no one has it open */
        if (slave >= 0 && !tcgetattr(slave, &t)) { cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t); }
        add(pty, 2, "pseudo-terminal");
    }
    printf("demo-instrument: a pretend instrument for Tick tests. Ctrl-C stops it.\n");
    printf("  network:      TCP port %d (DemoMeter: Network, this computer's address; from a VirtualBox NAT VM: 10.0.2.2)\n", NET_PORT);
    printf("  serial line:  TCP port %d, Unix socket %s (a VM's COM port), pseudo-terminal %s\n", COM_PORT, COM_PATH,
           pty >= 0 ? ptsname(pty) : "(none)");
    if (vm_port) printf("  VM COM port:  connecting to %s:%d (retrying every 2 s)\n", vm_host, vm_port);
    for (;;) {
        struct pollfd p[MAXC + 3];
        vm_try();
        int n = 0, map[MAXC];
        p[n++] = (struct pollfd){net, POLLIN, 0};
        p[n++] = (struct pollfd){com, POLLIN, 0};
        p[n++] = (struct pollfd){pipe_fd, pipe_fd >= 0 ? POLLIN : 0, 0};
        for (int i = 0; i < MAXC; i++) if (cs[i].fd >= 0) { map[n - 3] = i; p[n++] = (struct pollfd){cs[i].fd, POLLIN, 0}; }
        if (poll(p, n, vm_port ? 1000 : -1) < 0) { if (errno == EINTR) continue; perror("poll"); return 1; }
        if (p[0].revents & POLLIN) add(accept(net, 0, 0), 0, "network client");
        if (p[1].revents & POLLIN) add(accept(com, 0, 0), 1, "serial line (TCP)");
        if (pipe_fd >= 0 && (p[2].revents & POLLIN)) add(accept(pipe_fd, 0, 0), 1, "serial line (Unix socket)");
        for (int k = 3; k < n; k++)
            if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) handle(&cs[map[k - 3]]);
    }
}
