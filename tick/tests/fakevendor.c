/* fakevendor.exe - a stand-in "vendor program" for testing Tick's listener.
 * Opens a COM port like old instrument software does (9600 8N1, DTR on),
 * sends "*IDN?\r" and reads the answer, a few times; once with overlapped
 * I/O and once through a function pointer from GetProcAddress (late binding).
 *   fakevendor [COM1] [rounds] [delay_ms]
 * Exit code 0 if every round got an answer.  Test tool only, not shipped. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef BOOL (WINAPI *WRITEFILE)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

static int read_answer(HANDLE h, int overlapped, char *buf, int cap)
{
    DWORD n = 0;
    int got = 0, tries;
    for (tries = 0; tries < 20 && got < cap - 1; tries++) {
        if (overlapped) {
            OVERLAPPED ov;
            memset(&ov, 0, sizeof ov);
            ov.hEvent = CreateEventA(0, TRUE, FALSE, 0);
            if (!ReadFile(h, buf + got, cap - 1 - got, &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(h, &ov, &n, TRUE)) n = 0;
            }
            CloseHandle(ov.hEvent);
        } else if (!ReadFile(h, buf + got, cap - 1 - got, &n, 0)) n = 0;
        got += (int)n;
        if (got && buf[got - 1] == '\n') break;
    }
    buf[got] = 0;
    return got;
}

static int session(const char *port, int rounds, int delay, int overlapped)
{
    char path[64], ans[128];
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    WRITEFILE late = (WRITEFILE)(void *)GetProcAddress(LoadLibraryA("kernel32.dll"), "WriteFile");
    int i, ok = 0;
    snprintf(path, sizeof path, "\\\\.\\%s", port);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, overlapped ? FILE_FLAG_OVERLAPPED : 0, 0);
    if (h == INVALID_HANDLE_VALUE) { printf("cannot open %s (%lu)\n", path, GetLastError()); return 0; }
    SetupComm(h, 4096, 4096);
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 9600; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = 1; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) printf("SetCommState failed (%lu)\n", GetLastError());
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = 50; to.ReadTotalTimeoutConstant = 200; to.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &to);
    EscapeCommFunction(h, SETDTR);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    for (i = 0; i < rounds; i++) {
        char cmd[32];
        DWORD n = 0;
        snprintf(cmd, sizeof cmd, "*IDN? %d\r", i);
        if (overlapped) {
            OVERLAPPED ov;
            memset(&ov, 0, sizeof ov);
            ov.hEvent = CreateEventA(0, TRUE, FALSE, 0);
            if (!WriteFile(h, cmd, (DWORD)strlen(cmd), &n, &ov) && GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(h, &ov, &n, TRUE);
            CloseHandle(ov.hEvent);
        } else if (i == 1 && late) late(h, cmd, (DWORD)strlen(cmd), &n, 0);
        else WriteFile(h, cmd, (DWORD)strlen(cmd), &n, 0);
        if (read_answer(h, overlapped, ans, sizeof ans) > 0) { ok++; printf("%s -> %s", cmd, ans); }
        else printf("%s -> (no answer)\n", cmd);
        Sleep(delay);
    }
    EscapeCommFunction(h, CLRDTR);
    CloseHandle(h);
    return ok == rounds;
}

int main(int argc, char **argv)
{
    const char *port = argc > 1 ? argv[1] : "COM1";
    int rounds = argc > 2 ? atoi(argv[2]) : 3, delay = argc > 3 ? atoi(argv[3]) : 300;
    int a = session(port, rounds, delay, 0);
    int b = session(port, 2, delay, 1);
    printf("fakevendor: %s\n", a && b ? "all answered" : "some unanswered");
    fflush(stdout);
    {   /* also into fakevendor.log next to the exe: under Tick there is no console */
        char p[MAX_PATH];
        FILE *f;
        GetModuleFileNameA(0, p, MAX_PATH);
        strcpy(strrchr(p, '\\') + 1, "fakevendor.log");
        if ((f = fopen(p, "a")) != NULL) {
            fprintf(f, "%s %d rounds: %s\n", port, rounds, a && b ? "all answered" : "some unanswered");
            fclose(f);
        }
    }
    return a && b ? 0 : 1;
}
