/* tickctl.exe - test helper: drives a running Tick like an operator would.
 *   tickctl start|stop|mark [note]|sync|close|advanced
 * "close" also answers Tick's "Stop recording and close Tick?" with Yes.
 * Built with the normal mingw C runtime; it is a test tool, not shipped. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define IDC_START 101
#define IDC_MARK  102
#define IDC_SYNC  103
#define IDC_NOTE  104

int main(int argc, char **argv)
{
    HWND w = FindWindowA("TickRecorder", NULL);
    if (argc < 2) { printf("usage: tickctl start|stop|mark [note]|sync|close\n"); return 2; }
    if (!w) { printf("Tick window not found\n"); return 1; }
    if (!strcmp(argv[1], "start") || !strcmp(argv[1], "stop")) {
        char t[32];
        GetWindowTextA(GetDlgItem(w, IDC_START), t, sizeof t);
        if ((!strcmp(argv[1], "start")) == (!strcmp(t, "Start")))
            SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_START, BN_CLICKED), (LPARAM)GetDlgItem(w, IDC_START));
    } else if (!strcmp(argv[1], "mark")) {
        if (argc > 2) SetWindowTextA(GetDlgItem(w, IDC_NOTE), argv[2]);
        SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_MARK, BN_CLICKED), (LPARAM)GetDlgItem(w, IDC_MARK));
    } else if (!strcmp(argv[1], "enter")) {   /* type a note and press Enter in the note field */
        HWND e = GetDlgItem(w, IDC_NOTE);
        if (argc > 2) SetWindowTextA(e, argv[2]);
        SendMessageA(e, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageA(e, WM_CHAR, '\r', 0);
    } else if (!strcmp(argv[1], "sync")) {
        SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_SYNC, BN_CLICKED), (LPARAM)GetDlgItem(w, IDC_SYNC));
    } else if (!strcmp(argv[1], "close")) {
        int i;
        PostMessageA(w, WM_CLOSE, 0, 0);
        for (i = 0; i < 50; i++) {
            HWND d;
            Sleep(100);
            d = FindWindowA("#32770", "Tick");
            if (d) {
                PostMessageA(d, WM_COMMAND, IDYES, 0);
                printf("answered Yes to: Tick question\n");
            }
            if (!IsWindow(w)) break;
        }
        printf(IsWindow(w) ? "Tick still open\n" : "Tick closed\n");
    } else {
        printf("unknown command\n");
        return 2;
    }
    return 0;
}
