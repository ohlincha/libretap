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
#define IDC_PROG  108
#define IDC_PLIST 1020
#define IDC_PPICK 1022

static HWND wait_dialog(const char *title)
{
    int i;
    for (i = 0; i < 100; i++) {
        HWND d = FindWindowA("#32770", title);
        if (d) return d;
        Sleep(100);
    }
    return 0;
}

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
    } else if (!strcmp(argv[1], "programs")) {   /* open Start a program, list it; with TEXT: choose that entry */
        HWND d, lb;
        int i, k, pick = -1;
        PostMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_PROG, BN_CLICKED), (LPARAM)GetDlgItem(w, IDC_PROG));
        if (!(d = wait_dialog("Start a program"))) { printf("no Start a program dialog\n"); return 1; }
        lb = GetDlgItem(d, IDC_PLIST);
        k = (int)SendMessageA(lb, LB_GETCOUNT, 0, 0);
        for (i = 0; i < k; i++) {
            char t[400];
            WCHAR wt[400];
            wt[0] = 0;
            SendMessageW(lb, LB_GETTEXT, i, (LPARAM)wt);   /* the wide form survives Wine's cross-process copy */
            WideCharToMultiByte(CP_ACP, 0, wt, -1, t, sizeof t, 0, 0);
            printf("  [%d] %s\n", i, t);
            if (argc > 2 && pick < 0 && strstr(t, argv[2])) pick = i;
        }
        if (argc > 2 && !strcmp(argv[2], "--pick")) {
            PostMessageA(d, WM_COMMAND, MAKEWPARAM(IDC_PPICK, BN_CLICKED), 0);
            printf("Pick window pressed\n");
        } else if (pick >= 0) {
            char det[600];
            SendMessageA(lb, LB_SETCURSEL, pick, 0);
            SendMessageA(d, WM_COMMAND, MAKEWPARAM(IDC_PLIST, LBN_SELCHANGE), (LPARAM)lb);
            GetDlgItemTextA(d, 1021, det, sizeof det);
            printf("chosen [%d], details: %s\n", pick, det);
            PostMessageA(d, WM_COMMAND, IDOK, 0);
        } else PostMessageA(d, WM_COMMAND, IDCANCEL, 0);
    } else if (!strcmp(argv[1], "answer")) {      /* answer Tick's next question: yes|no|ok|cancel */
        HWND d = wait_dialog("Tick");
        char text[1000] = "";
        if (!d) { printf("no question\n"); return 1; }
        {   /* the message box text is its static control 0xFFFF */
            HWND s = GetDlgItem(d, 0xFFFF);
            if (s) GetWindowTextA(s, text, sizeof text);
        }
        printf("question: %s\n", text);
        PostMessageA(d, WM_COMMAND, !strcmp(argv[2], "yes") ? IDYES : !strcmp(argv[2], "no") ? IDNO :
                                    !strcmp(argv[2], "ok") ? IDOK : IDCANCEL, 0);
    } else if (!strcmp(argv[1], "activate")) {    /* bring a window to the front (by title) */
        HWND t = FindWindowA(0, argv[2]);
        if (!t) { printf("window %s not found\n", argv[2]); return 1; }
        SetForegroundWindow(t);
        printf("activated %s\n", argv[2]);
    } else if (!strcmp(argv[1], "status")) {
        char t[400];
        int id;
        for (id = 105; id <= 107; id++) { GetWindowTextA(GetDlgItem(w, id), t, sizeof t); printf("  %s\n", t); }
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
