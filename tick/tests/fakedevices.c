/* fakedevices.exe - test "vendor program" for Tick's network and USB listener.
 *   fakedevices net PORT   TCP to 127.0.0.1:PORT: send/recv, WSASend + overlapped WSARecv;
 *                          UDP to 127.0.0.1:PORT+1: sendto/recvfrom
 *   fakedevices usb        device handle \\.\NUL: WriteFile, ReadFile, DeviceIoControl;
 *                          hid.dll HidD_SetFeature/GetFeature and winusb.dll WinUsb_* through
 *                          GetProcAddress (they fail without a real device; Tick records the calls)
 * Writes one result line to fakedevices.log next to the exe.  Test tool only. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void logline(const char *s)
{
    char p[MAX_PATH];
    FILE *f;
    GetModuleFileNameA(0, p, MAX_PATH);
    strcpy(strrchr(p, '\\') + 1, "fakedevices.log");
    if ((f = fopen(p, "a"))) { fprintf(f, "%s\n", s); fclose(f); }
    printf("%s\n", s);
}

static int net(int port)
{
    WSADATA w;
    SOCKET s, u;
    struct sockaddr_in a;
    char buf[256];
    int ok = 0, n, i;
    WSAStartup(MAKEWORD(2, 2), &w);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { logline("net: connect failed"); return 1; }
    for (i = 0; i < 3; i++) {
        sprintf(buf, "*IDN? %d\r", i);
        send(s, buf, (int)strlen(buf), 0);
        n = recv(s, buf, sizeof buf - 1, 0);
        if (n > 0) ok++;
    }
    {   /* WSASend + overlapped WSARecv */
        WSABUF b;
        DWORD sent = 0, got = 0, fl = 0;
        WSAOVERLAPPED ov;
        strcpy(buf, "*WSA?\r");
        b.buf = buf; b.len = (ULONG)strlen(buf);
        WSASend(s, &b, 1, &sent, 0, 0, 0);
        memset(&ov, 0, sizeof ov);
        ov.hEvent = WSACreateEvent();
        b.buf = buf; b.len = sizeof buf;
        if (WSARecv(s, &b, 1, &got, &fl, &ov, 0) == 0 ||
            (WSAGetLastError() == WSA_IO_PENDING && WSAGetOverlappedResult(s, &ov, &got, TRUE, &fl))) {
            if (got) ok++;
        }
        WSACloseEvent(ov.hEvent);
    }
    closesocket(s);
    u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a.sin_port = htons((u_short)(port + 1));
    sendto(u, "PING\r", 5, 0, (struct sockaddr *)&a, sizeof a);
    {
        struct sockaddr_in from;
        int fl = sizeof from;
        DWORD tmo = 2000;
        setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
        n = recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) ok++;
    }
    closesocket(u);
    sprintf(buf, "net: %d of 5 answered", ok);
    logline(buf);
    return ok == 5 ? 0 : 1;
}

typedef BOOLEAN (WINAPI *HIDF)(HANDLE, PVOID, ULONG);
typedef BOOL (WINAPI *WUINIT)(HANDLE, PVOID *);

static int usb(void)
{
    HANDLE h = CreateFileA("\\\\.\\NUL", GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    DWORD n = 0;
    char buf[64] = "\x01\x02\x03\x04usb-out";
    HMODULE hid = LoadLibraryA("hid.dll"), wu = LoadLibraryA("winusb.dll");
    char m[200];
    if (h == INVALID_HANDLE_VALUE) { logline("usb: cannot open \\\\.\\NUL"); return 1; }
    WriteFile(h, buf, 11, &n, 0);
    ReadFile(h, buf, sizeof buf, &n, 0);
    DeviceIoControl(h, 0x00220004, buf, 4, buf, sizeof buf, &n, 0);
    if (hid) {
        HIDF set = (HIDF)(void *)GetProcAddress(hid, "HidD_SetFeature");
        HIDF get = (HIDF)(void *)GetProcAddress(hid, "HidD_GetFeature");
        unsigned char rep[9] = {0, 0xa5, 0x5a};
        if (set) set(h, rep, sizeof rep);
        if (get) get(h, rep, sizeof rep);
    }
    if (wu && !getenv("TICK_TEST_NO_WINUSB")) {     /* Wine's winusb.dll aborts in WinUsb_Initialize */
        WUINIT init = (WUINIT)(void *)GetProcAddress(wu, "WinUsb_Initialize");
        PVOID ih = 0;
        if (init) init(h, &ih);
    }
    CloseHandle(h);
    sprintf(m, "usb: done (hid.dll %s, winusb.dll %s)", hid ? "loaded" : "missing", wu ? "loaded" : "missing");
    logline(m);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "net")) return net(atoi(argv[2]));
    if (argc > 1 && !strcmp(argv[1], "usb")) return usb();
    printf("usage: fakedevices net PORT | usb\n");
    return 2;
}
