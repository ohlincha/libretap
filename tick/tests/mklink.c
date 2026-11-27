/* mklink.exe - test helper: makes a real shortcut with the system's IShellLink (as Explorer
 * and installers do), to check that Tick reads shortcuts the way Windows writes them.
 *   mklink LNKFILE TARGET [ARGS] [STARTIN] [COMMENT]
 * Test tool only, not shipped. */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    IShellLinkA *sl;
    IPersistFile *pf;
    WCHAR w[MAX_PATH];
    HRESULT hr;
    if (argc < 3) { printf("usage: mklink LNK TARGET [ARGS] [STARTIN] [COMMENT]\n"); return 2; }
    CoInitialize(0);
    hr = CoCreateInstance(&CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl);
    if (FAILED(hr)) { printf("no IShellLink (%lx)\n", (unsigned long)hr); return 1; }
    IShellLinkA_SetPath(sl, argv[2]);
    if (argc > 3) IShellLinkA_SetArguments(sl, argv[3]);
    if (argc > 4) IShellLinkA_SetWorkingDirectory(sl, argv[4]);
    if (argc > 5) IShellLinkA_SetDescription(sl, argv[5]);
    hr = IShellLinkA_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (FAILED(hr)) return 1;
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    hr = IPersistFile_Save(pf, w, TRUE);
    printf(SUCCEEDED(hr) ? "shortcut %s -> %s\n" : "could not save %s\n", argv[1], argv[2]);
    IPersistFile_Release(pf);
    IShellLinkA_Release(sl);
    return SUCCEEDED(hr) ? 0 : 1;
}
