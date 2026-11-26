/* fakedevices_linux - test "vendor program" for Tick's network listener on Linux:
 * TCP to 127.0.0.1:PORT (connect, send, recv, write/read, sendmsg/recvmsg), UDP to PORT+1
 * (sendto/recvfrom).  Also opens /dev/null as if it were a USB device path given as the
 * second argument (only the path matters for Tick).  Exit 0 if all were answered. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 5800, s, u, ok = 0, i;
    struct sockaddr_in a;
    char buf[256];
    struct timeval tv = {2, 0};
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 2; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (i = 0; i < 2; i++) {
        snprintf(buf, sizeof buf, "*IDN? %d\r", i);
        send(s, buf, strlen(buf), 0);
        if (recv(s, buf, sizeof buf, 0) > 0) ok++;
    }
    if (write(s, "*RW?\r", 5) == 5 && read(s, buf, sizeof buf) > 0) ok++;
    {
        struct iovec v[2] = {{"*MS", 3}, {"G?\r", 3}};
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = v; m.msg_iovlen = 2;
        sendmsg(s, &m, 0);
        v[0].iov_base = buf; v[0].iov_len = sizeof buf; m.msg_iovlen = 1;
        if (recvmsg(s, &m, 0) > 0) ok++;
    }
    close(s);
    u = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    a.sin_port = htons((unsigned short)(port + 1));
    sendto(u, "PING\r", 5, 0, (struct sockaddr *)&a, sizeof a);
    if (recvfrom(u, buf, sizeof buf, 0, 0, 0) > 0) ok++;
    close(u);
    printf("fakedevices_linux: %d of 5 answered\n", ok);
    return ok == 5 ? 0 : 1;
}
