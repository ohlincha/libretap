/* fakevendor_linux - stand-in Linux "vendor program" for testing tick run:
 * opens a serial port (9600 8N1 raw, DTR on), sends "*IDN? n\r" and reads the
 * answer, a few times (once with writev).  Exit code 0 if all were answered.
 *   fakevendor_linux PORT [rounds] [delay_ms] */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *port = argc > 1 ? argv[1] : "/dev/ttyS0";
    int rounds = argc > 2 ? atoi(argv[2]) : 3, delay = argc > 3 ? atoi(argv[3]) : 300, i, ok = 0, bits = TIOCM_DTR;
    struct termios t;
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror(port); return 2; }
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B9600);
    cfsetospeed(&t, B9600);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
    ioctl(fd, TIOCMBIS, &bits);
    tcflush(fd, TCIOFLUSH);
    for (i = 0; i < rounds; i++) {
        char cmd[32], ans[128];
        int got = 0;
        snprintf(cmd, sizeof cmd, "*IDN? %d\r", i);
        if (i == 1) {
            struct iovec v[2] = {{cmd, 4}, {cmd + 4, strlen(cmd) - 4}};
            if (writev(fd, v, 2) < 0) perror("writev");
        } else if (write(fd, cmd, strlen(cmd)) < 0) perror("write");
        while (got < (int)sizeof ans - 1) {
            struct pollfd p = {fd, POLLIN, 0};
            ssize_t n;
            if (poll(&p, 1, 1000) <= 0) break;
            n = read(fd, ans + got, sizeof ans - 1 - got);
            if (n <= 0) break;
            got += (int)n;
            if (ans[got - 1] == '\n') break;
        }
        ans[got] = 0;
        if (got) ok++;
        printf("%s -> %s", cmd, got ? ans : "(no answer)\n");
        fflush(stdout);
        {
            struct timespec ts = {delay / 1000, (long)(delay % 1000) * 1000000};
            nanosleep(&ts, 0);
        }
    }
    bits = TIOCM_DTR;
    ioctl(fd, TIOCMBIC, &bits);
    close(fd);
    printf("fakevendor_linux: %s\n", ok == rounds ? "all answered" : "some unanswered");
    {
        const char *log = getenv("FAKEVENDOR_LOG");
        FILE *f = log ? fopen(log, "a") : 0;
        if (f) { fprintf(f, "%d rounds: %s\n", rounds, ok == rounds ? "all answered" : "some unanswered"); fclose(f); }
    }
    return ok == rounds ? 0 : 1;
}
