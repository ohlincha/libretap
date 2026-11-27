/* DemoMeter.exe: pretend vendor software for trying Tick. Start DemoInstrument.exe first, then start this
 * from Tick ("Start a program..."). It connects over the network (this computer, port 5025) or a COM port,
 * asks the instrument who it is, sends the settings when you change them, and reads a value every 0.5 s.
 * No C runtime; Windows 95 (with Winsock 2) / 98 / 2000 / XP and later. */
#include "demo_common.h"

enum { ID_CONN = 100, ID_CONNECT, ID_INT, ID_GAIN, ID_READING, ID_POLLS, ID_STATUS, ID_TIMER = 1 };

static HWND wnd, conn_box, int_box, gain_box, reading_lbl, polls_lbl, status_lbl, connect_btn;
static SOCKET sock = INVALID_SOCKET;
static HANDLE com = INVALID_HANDLE_VALUE;
static int polls;
static const int int_values[] = {100, 250, 500, 1000};
static const int gain_values[] = {1, 2, 4};

static void status(const char *s) { SetWindowTextA(status_lbl, s); }
static int connected(void) { return sock != INVALID_SOCKET || com != INVALID_HANDLE_VALUE; }

static void disconnect(void)
{
    KillTimer(wnd, ID_TIMER);
    if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    if (com != INVALID_HANDLE_VALUE) { CloseHandle(com); com = INVALID_HANDLE_VALUE; }
    SetWindowTextA(connect_btn, "Connect");
}

/* send one command, read one answer line (without CR LF); 0 on no answer */
static int ask(const char *cmd, char *ans, int cap)
{
    char line[64];
    int got = 0;
    DWORD n;
    wsprintfA(line, "%s\r", cmd);
    ans[0] = 0;
    if (sock != INVALID_SOCKET) {
        if (send(sock, line, d_len(line), 0) <= 0) return 0;
        while (got < cap - 1) {   /* whole answers: one recv usually holds the line */
            int r = recv(sock, ans + got, cap - 1 - got, 0);
            if (r <= 0) return 0;
            got += r;
            if (ans[got - 1] == '\n') break;
        }
    } else if (com != INVALID_HANDLE_VALUE) {
        if (!WriteFile(com, line, (DWORD)d_len(line), &n, 0)) return 0;
        while (got < cap - 1) {   /* ends when the line is complete or after a pause (timeouts) */
            if (!ReadFile(com, ans + got, (DWORD)(cap - 1 - got), &n, 0) || n == 0) break;
            got += (int)n;
            if (ans[got - 1] == '\n') break;
        }
        if (!got) return 0;
    } else {
        return 0;
    }
    ans[got] = 0;
    while (got && (ans[got - 1] == '\r' || ans[got - 1] == '\n')) ans[--got] = 0;
    return 1;
}

static void send_settings(void)
{
    char cmd[32], ans[64];
    int i = (int)SendMessageA(int_box, CB_GETCURSEL, 0, 0), g = (int)SendMessageA(gain_box, CB_GETCURSEL, 0, 0);
    if (!connected()) return;
    wsprintfA(cmd, "INT %d", int_values[i < 0 ? 2 : i]);
    ask(cmd, ans, sizeof ans);
    wsprintfA(cmd, "GAIN %d", gain_values[g < 0 ? 0 : g]);
    ask(cmd, ans, sizeof ans);
}

static void do_connect(void)
{
    char ans[64], msg[128];
    int k = (int)SendMessageA(conn_box, CB_GETCURSEL, 0, 0);
    if (connected()) { disconnect(); status("Disconnected."); return; }
    if (k <= 0) {   /* network */
        struct sockaddr_in a;
        DWORD tmo = 2000;
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(DEMO_PORT);
        a.sin_addr.s_addr = htonl(0x7f000001);   /* 127.0.0.1 */
        if (sock == INVALID_SOCKET || connect(sock, (struct sockaddr *)&a, sizeof a) != 0) {
            disconnect();
            status("No instrument on the network. Start DemoInstrument.exe first.");
            return;
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    } else {        /* COM1..COM4 */
        char path[32];
        DCB dcb;
        COMMTIMEOUTS to;
        wsprintfA(path, "\\\\.\\COM%d", k);
        com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
        if (com == INVALID_HANDLE_VALUE) {
            wsprintfA(msg, "Cannot open COM%d (missing or in use).", k);
            status(msg);
            return;
        }
        memset(&dcb, 0, sizeof dcb);
        dcb.DCBlength = sizeof dcb;
        GetCommState(com, &dcb);
        dcb.BaudRate = 9600; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = 1; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = 0; dcb.fOutxDsrFlow = 0; dcb.fOutX = 0; dcb.fInX = 0;
        SetCommState(com, &dcb);
        memset(&to, 0, sizeof to);
        to.ReadIntervalTimeout = 50; to.ReadTotalTimeoutConstant = 300;
        SetCommTimeouts(com, &to);
    }
    if (!ask("*IDN?", ans, sizeof ans) || !d_starts(ans, "DEMO-METER")) {
        disconnect();
        status(k <= 0 ? "The instrument did not answer." : "No answer on that COM port (is DemoInstrument on the other end?).");
        return;
    }
    wsprintfA(msg, "Connected: %s", ans);
    status(msg);
    SetWindowTextA(connect_btn, "Disconnect");
    send_settings();
    polls = 0;
    SetTimer(wnd, ID_TIMER, 500, 0);
}

static void poll(void)
{
    char ans[64], txt[64];
    if (!ask("MEAS?", ans, sizeof ans)) {
        disconnect();
        status("Lost the instrument.");
        return;
    }
    if (d_starts(ans, "M ")) SetWindowTextA(reading_lbl, ans + 2);
    wsprintfA(txt, "%d", ++polls);
    SetWindowTextA(polls_lbl, txt);
}

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(0), 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
    return c;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CONNECT) do_connect();
        else if ((LOWORD(wp) == ID_INT || LOWORD(wp) == ID_GAIN) && HIWORD(wp) == CBN_SELCHANGE) send_settings();
        return 0;
    case WM_TIMER:
        if (wp == ID_TIMER) poll();
        if (wp == 2) { KillTimer(h, 2); SendMessageA(gain_box, CB_SETCURSEL, 2, 0); send_settings(); }
        return 0;
    case WM_DESTROY:
        disconnect();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

void __stdcall MeterMain(void)
{
    WNDCLASSA wc;
    MSG msg;
    WSADATA w;
    int i;
    char t[32];
    HINSTANCE hi = GetModuleHandleA(0);
    WSAStartup(MAKEWORD(2, 0), &w);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.hIcon = LoadIconA(0, (LPCSTR)IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "DemoMeter";
    RegisterClassA(&wc);
    wnd = CreateWindowExA(0, "DemoMeter", "DEMO-METER Control 2.1", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 420, 270, 0, 0, hi, 0);
    add("STATIC", "Connection", 0, 16, 18, 110, 20, 0);
    conn_box = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 14, 170, 200, ID_CONN);
    SendMessageA(conn_box, CB_ADDSTRING, 0, (LPARAM)"Network (this computer)");
    for (i = 1; i <= 4; i++) { wsprintfA(t, "COM%d, 9600 baud", i); SendMessageA(conn_box, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(conn_box, CB_SETCURSEL, 0, 0);
    connect_btn = add("BUTTON", "Connect", BS_PUSHBUTTON | WS_TABSTOP, 310, 13, 85, 25, ID_CONNECT);
    add("STATIC", "Integration time", 0, 16, 58, 110, 20, 0);
    int_box = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 54, 120, 200, ID_INT);
    for (i = 0; i < 4; i++) { wsprintfA(t, "%d ms", int_values[i]); SendMessageA(int_box, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(int_box, CB_SETCURSEL, 2, 0);
    add("STATIC", "Gain", 0, 16, 92, 110, 20, 0);
    gain_box = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 130, 88, 120, 200, ID_GAIN);
    for (i = 0; i < 3; i++) { wsprintfA(t, "x%d", gain_values[i]); SendMessageA(gain_box, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(gain_box, CB_SETCURSEL, 0, 0);
    add("STATIC", "Reading", 0, 16, 126, 110, 20, 0);
    reading_lbl = add("STATIC", "-", SS_SUNKEN, 130, 124, 120, 22, ID_READING);
    add("STATIC", "Polls", 0, 16, 158, 110, 20, 0);
    polls_lbl = add("STATIC", "0", SS_SUNKEN, 130, 156, 120, 22, ID_POLLS);
    status_lbl = add("STATIC", "Start DemoInstrument.exe, then press Connect.", 0, 16, 196, 380, 20, ID_STATUS);
    ShowWindow(wnd, SW_SHOW);
    {   /* for scripts: /connect connects at once, /gain4 then changes the gain after 3 s */
        const char *cl = GetCommandLineA();
        for (i = 0; cl[i]; i++) {
            if (d_starts(cl + i, "/connect")) do_connect();
            if (d_starts(cl + i, "/gain4")) SetTimer(wnd, 2, 3000, 0);
        }
    }
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        if (IsDialogMessageA(wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    WSACleanup();
    ExitProcess(0);
}
