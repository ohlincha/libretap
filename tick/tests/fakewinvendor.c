/* fakewinvendor.exe - a stand-in vendor program WITH a window ("Fake VendorSoft"), for testing
 * Tick's program list, restart and Pick window.  Logs its command line and working folder to
 * vendor-start.log next to itself, then every second asks the instrument on its COM port.
 *   fakewinvendor [COM1]
 * Closes on WM_CLOSE like any program.  Test tool only, not shipped. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE port = INVALID_HANDLE_VALUE;
static int n;

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TIMER && port != INVALID_HANDLE_VALUE) {
        char q[32], a[64];
        DWORD k = 0;
        sprintf(q, "*IDN? %d\r", n++);
        WriteFile(port, q, (DWORD)strlen(q), &k, 0);
        ReadFile(port, a, sizeof a - 1, &k, 0);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE p, LPSTR cmd, int show)
{
    char me[MAX_PATH], log[MAX_PATH], cwd[MAX_PATH], com[64] = "COM1";
    FILE *f;
    WNDCLASSA wc;
    MSG msg;
    char *s;
    (void)p;
    GetModuleFileNameA(0, me, MAX_PATH);
    strcpy(log, me);
    s = strrchr(log, '\\');
    strcpy(s ? s + 1 : log, "vendor-start.log");
    GetCurrentDirectoryA(MAX_PATH, cwd);
    f = fopen(log, "a");
    if (f) { fprintf(f, "cmdline=[%s] cwd=[%s]\n", GetCommandLineA(), cwd); fclose(f); }
    if (cmd && *cmd) { sscanf(cmd, "%60s", com); }
    {
        char dev[80];
        DCB d;
        COMMTIMEOUTS t = {50, 0, 300, 0, 300};
        sprintf(dev, "\\\\.\\%s", com);
        port = CreateFileA(dev, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
        if (port != INVALID_HANDLE_VALUE) {
            memset(&d, 0, sizeof d);
            d.DCBlength = sizeof d;
            GetCommState(port, &d);
            d.BaudRate = 9600; d.ByteSize = 8; d.Parity = NOPARITY; d.StopBits = ONESTOPBIT; d.fBinary = 1;
            SetCommState(port, &d);
            SetCommTimeouts(port, &t);
        }
    }
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wp;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "FakeVendorSoft";
    RegisterClassA(&wc);
    CreateWindowExA(0, "FakeVendorSoft", "Fake VendorSoft", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 360, 200, 0, 0, hi, 0);
    SetTimer(FindWindowA("FakeVendorSoft", 0), 1, 1000, 0);
    while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (port != INVALID_HANDLE_VALUE) CloseHandle(port);
    (void)show;
    return 0;
}
