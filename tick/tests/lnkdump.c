/* lnkdump.exe - test helper: prints what Tick's own shortcut reader (src/lnk.c) finds in a .lnk,
 * to compare with what the system wrote.   lnkdump LNKFILE   Test tool only. */
#include <windows.h>
#include <stdio.h>
#include "../src/lnk.h"

static void w2a(const u16 *w, u32 n, char *out, u32 cap)
{
    int k = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)w, (int)n, out, (int)cap - 1, 0, 0);
    out[k > 0 ? k : 0] = 0;
}

int main(int argc, char **argv)
{
    static unsigned char buf[1 << 20];
    FILE *f;
    size_t n;
    lnk_info o;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) return 2;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (!lnk_parse(buf, (u32)n, &o, w2a)) { printf("not a shortcut\n"); return 1; }
    printf("target=[%s]\nargs=[%s]\nworkdir=[%s]\nname=[%s]\nfrom=%d env=%d\n", o.target, o.args, o.workdir, o.name, o.from,
           o.target_has_env);
    return 0;
}
