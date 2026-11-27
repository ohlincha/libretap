/* DemoInstrument.exe: a pretend instrument for trying Tick. Start it first, then DemoMeter.exe.
 * Answers on the network (TCP port 5025, this computer) and on COM2 if that port exists (for a VM whose COM1 and
 * COM2 are connected to each other). Commands, one per line ending in CR:
 *   *IDN?      -> DEMO-METER,2.1
 *   INT n      -> OK          integration time in ms
 *   GAIN n     -> OK          1, 2 or 4
 *   MEAS?      -> M 0.5418 V  a slowly varying reading
 * No C runtime; Windows 95 (with Winsock 2) / 98 / 2000 / XP and later. */
#include "demo_common.h"

static HANDLE out_h;
static CRITICAL_SECTION out_cs;

static void say(const char *s)
{
    DWORD n;
    EnterCriticalSection(&out_cs);
    WriteFile(out_h, s, (DWORD)d_len(s), &n, 0);
    WriteFile(out_h, "\r\n", 2, &n, 0);
    LeaveCriticalSection(&out_cs);
}

struct state { int ms, gain; };

/* one command line -> answer (with CR LF) */
static void answer(struct state *st, const char *cmd, char *ans)
{
    if (d_starts(cmd, "*IDN?")) {
        lstrcpyA(ans, "DEMO-METER,2.1\r\n");
    } else if (d_starts(cmd, "INT ")) {
        int v = d_atoi(cmd + 4);
        if (v >= 10 && v <= 10000) { st->ms = v; lstrcpyA(ans, "OK\r\n"); } else lstrcpyA(ans, "ERR range\r\n");
    } else if (d_starts(cmd, "GAIN ")) {
        int v = d_atoi(cmd + 5);
        if (v == 1 || v == 2 || v == 4) { st->gain = v; lstrcpyA(ans, "OK\r\n"); } else lstrcpyA(ans, "ERR range\r\n");
    } else if (d_starts(cmd, "MEAS?")) {
        double t = GetTickCount() / 1000.0;
        double v = 0.5 + 0.2 * d_sin(t / 7.0) * st->ms / 1000.0 * st->gain;
        char num[32];
        d_volts(num, v);
        wsprintfA(ans, "M %s V\r\n", num);
    } else {
        lstrcpyA(ans, "ERR unknown command\r\n");
    }
}

static void log_line(const char *where, const char *cmd, const char *ans)
{
    char line[300], a[100];
    int i;
    lstrcpynA(a, ans, sizeof a);
    for (i = 0; a[i]; i++) if (a[i] == '\r' || a[i] == '\n') a[i] = 0;
    wsprintfA(line, "%s  %s  ->  %s", where, cmd, a);
    say(line);
}

/* ---- network */
static DWORD WINAPI tcp_thread(LPVOID unused)
{
    SOCKET ls, c;
    struct sockaddr_in a;
    (void)unused;
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { say("Network: cannot create a socket."); return 0; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(DEMO_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        say("Network: port 5025 is in use (is DemoInstrument already running?).");
        return 0;
    }
    say("Network: listening on port 5025.");
    for (;;) {
        struct state st = {500, 1};
        char buf[256], ans[128];
        int got = 0;
        c = accept(ls, 0, 0);
        if (c == INVALID_SOCKET) continue;
        say("Network: DemoMeter connected.");
        for (;;) {
            int n = recv(c, buf + got, (int)sizeof buf - 1 - got, 0), i, start = 0;
            if (n <= 0) break;
            got += n;
            buf[got] = 0;
            for (i = 0; i < got; i++)
                if (buf[i] == '\r' || buf[i] == '\n') {
                    buf[i] = 0;
                    if (i > start) {
                        answer(&st, buf + start, ans);
                        send(c, ans, d_len(ans), 0);
                        log_line("net ", buf + start, ans);
                    }
                    start = i + 1;
                }
            memcpy(buf, buf + start, (unsigned)(got - start));
            got -= start;
            if (got >= (int)sizeof buf - 1) got = 0;
        }
        closesocket(c);
        say("Network: DemoMeter disconnected.");
    }
}

/* ---- serial (COM2, 9600 8N1) */
static DWORD WINAPI serial_thread(LPVOID name)
{
    char path[32], buf[256], ans[128], msg[80];
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    struct state st = {500, 1};
    int got = 0;
    wsprintfA(path, "\\\\.\\%s", (char *)name);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfA(msg, "Serial: no %s here (network only).", (char *)name);
        say(msg);
        return 0;
    }
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 9600; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = 1; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = 0; dcb.fOutxDsrFlow = 0; dcb.fOutX = 0; dcb.fInX = 0;
    SetCommState(h, &dcb);
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = 50; to.ReadTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &to);
    wsprintfA(msg, "Serial: answering on %s, 9600 baud 8N1.", (char *)name);
    say(msg);
    for (;;) {
        DWORD n = 0, w;
        int i, start = 0;
        if (!ReadFile(h, buf + got, (DWORD)(sizeof buf - 1 - got), &n, 0)) { Sleep(200); continue; }
        if (!n) continue;
        got += (int)n;
        buf[got] = 0;
        for (i = 0; i < got; i++)
            if (buf[i] == '\r' || buf[i] == '\n') {
                buf[i] = 0;
                if (i > start) {
                    answer(&st, buf + start, ans);
                    WriteFile(h, ans, (DWORD)d_len(ans), &w, 0);
                    log_line((char *)name, buf + start, ans);
                }
                start = i + 1;
            }
        memcpy(buf, buf + start, (unsigned)(got - start));
        got -= start;
        if (got >= (int)sizeof buf - 1) got = 0;
    }
}

void __stdcall InstrumentMain(void)
{
    WSADATA w;
    DWORD id;
    static char com2[] = "COM2";
    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    InitializeCriticalSection(&out_cs);
    SetConsoleTitleA("DemoInstrument - pretend instrument for Tick tests");
    say("DemoInstrument: a pretend instrument. Leave this window open, then start DemoMeter.exe (from Tick).");
    say("Close this window to stop it.");
    say("");
    if (WSAStartup(MAKEWORD(2, 0), &w) == 0) CreateThread(0, 0, tcp_thread, 0, 0, &id);
    else say("Network: Winsock 2 is not available.");
    CreateThread(0, 0, serial_thread, com2, 0, &id);
    for (;;) Sleep(1000);
}
