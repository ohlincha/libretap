/* lnk.c - see lnk.h.  Layout from Microsoft's [MS-SHLLINK] specification:
 *   ShellLinkHeader (0x4C bytes) - LinkTargetIDList - LinkInfo - StringData - ExtraData
 * The target path is taken, in this order of trust, from
 *   1. LinkInfo: LocalBasePath (+ CommonPathSuffix), Unicode variant when present,
 *      or a network share (\\server\share + suffix);
 *   2. the EnvironmentVariableDataBlock ("%ProgramFiles%\..."): caller expands it;
 *   3. the item list (My Computer - drive - folders - file), long names from the
 *      0xBEEF0004 extension block where there is one, else the 8.3 names;
 *   4. the relative path (relative to the shortcut's folder; the caller joins).
 * Every offset is checked against the buffer: a damaged file gives "" fields,
 * never a crash. */
#include "lnk.h"

static u32 g16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 g32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void put(char *d, u32 cap, const char *s, u32 n)
{
    u32 i = 0;
    while (i < n && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void cat(char *d, u32 cap, const char *s)
{
    u32 i = 0, k = 0;
    while (d[i] && i + 1 < cap) i++;
    while (s[k] && i + 1 < cap) d[i++] = s[k++];
    d[i] = 0;
}

/* NUL-terminated 8-bit string at off (within [off, end)) */
static void ansi_at(const u8 *b, u32 off, u32 end, char *out, u32 cap)
{
    u32 n = 0;
    out[0] = 0;
    if (off >= end) return;
    while (off + n < end && b[off + n]) n++;
    put(out, cap, (const char *)b + off, n);
}

/* UTF-16 string of n units (or up to NUL if n == 0xffffffff) at off */
static void wide_at(const u8 *b, u32 off, u32 end, u32 n, char *out, u32 cap, lnk_w2a w2a)
{
    u16 tmp[LNK_TEXT];
    u32 i = 0;
    out[0] = 0;
    while (i < LNK_TEXT - 1 && off + i * 2 + 1 < end && (n == 0xffffffffu ? 1 : i < n)) {
        u16 c = (u16)g16(b + off + i * 2);
        if (n == 0xffffffffu && !c) break;
        tmp[i++] = c;
    }
    tmp[i] = 0;
    w2a(tmp, i, out, cap);
}

static void link_info(const u8 *b, u32 n, u32 off, lnk_info *o, lnk_w2a w2a)
{
    u32 size, hsize, flags, end;
    char base[LNK_TEXT], suffix[LNK_TEXT];
    if (off + 28 > n) return;
    size = g32(b + off);
    hsize = g32(b + off + 4);
    flags = g32(b + off + 8);
    if (size < 28 || off + size > n) return;
    end = off + size;
    base[0] = suffix[0] = 0;
    if (flags & 1) {                                /* VolumeIDAndLocalBasePath */
        if (hsize >= 0x24 && g32(b + off + 28)) wide_at(b, off + g32(b + off + 28), end, 0xffffffffu, base, sizeof base, w2a);
        if (!base[0]) ansi_at(b, off + g32(b + off + 16), end, base, sizeof base);
    } else if (flags & 2) {                         /* CommonNetworkRelativeLink: \\server\share */
        u32 cn = off + g32(b + off + 20);
        if (cn + 20 <= end) ansi_at(b, cn + g32(b + cn + 8), end, base, sizeof base);
    }
    if (hsize >= 0x24 && off + 36 <= end && g32(b + off + 32))
        wide_at(b, off + g32(b + off + 32), end, 0xffffffffu, suffix, sizeof suffix, w2a);
    if (!suffix[0] && g32(b + off + 24)) ansi_at(b, off + g32(b + off + 24), end, suffix, sizeof suffix);
    if (!base[0]) return;
    put(o->target, sizeof o->target, base, LNK_TEXT);
    if (suffix[0]) {
        u32 l = 0;
        while (o->target[l]) l++;
        if (l && o->target[l - 1] != '\\') cat(o->target, sizeof o->target, "\\");
        cat(o->target, sizeof o->target, suffix);
    }
    o->from = 1;
}

/* long name in a file entry's 0xBEEF0004 extension block: the UTF-16 string that
 * ends just before the block's last two bytes (its own offset field) */
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static int long_name(const u8 *it, u32 isz, u32 name_end, const char *shortn, char *out, u32 cap, lnk_w2a w2a)
{
    u32 e = (name_end + 1) & ~1u;                   /* extension blocks are 2-aligned */
    while (e + 8 <= isz) {
        u32 esz = g16(it + e), sig = g32(it + e + 4);
        if (esz < 8 || e + esz > isz) return 0;
        if (sig == 0xBEEF0004u && esz >= 22) {
            u32 stop = esz - 2, k, start;
            if (stop < 4 || g16(it + e + stop - 2)) return 0;        /* the terminating 0x0000 */
            k = stop - 2;
            while (k >= 20 && g16(it + e + k - 2) >= 0x20) k -= 2;   /* back to the string's start */
            start = k;
            /* a size field just before the name can look like a character: the long name starts with the
               same letter as the 8.3 name, so skip up to two units that do not */
            {
                int t;
                for (t = 0; t < 2 && start + 2 < stop - 2 && shortn[0] && g16(it + e + start) < 0x80 &&
                            up((char)g16(it + e + start)) != up(shortn[0]); t++) start += 2;
            }
            if (start >= stop - 2) return 0;
            wide_at(it, e + start, e + stop, (stop - 2 - start) / 2, out, cap, w2a);
            return out[0] != 0;
        }
        e += esz;
    }
    return 0;
}

static void id_list(const u8 *b, u32 n, u32 off, u32 len, lnk_info *o, lnk_w2a w2a)
{
    char path[LNK_TEXT], part[LNK_TEXT];
    u32 p = off, end = off + len;
    int have_drive = 0;
    if (end > n) return;
    path[0] = 0;
    while (p + 2 <= end) {
        u32 isz = g16(b + p);
        const u8 *it = b + p;
        u8 type;
        if (!isz) break;
        if (isz < 3 || p + isz > end) return;
        type = it[2];
        if ((type & 0x70) == 0x20 && isz >= 6) {    /* volume: "C:\" */
            ansi_at(it, 3, isz, part, sizeof part);
            if (part[0] && part[1] == ':') {
                put(path, sizeof path, part, LNK_TEXT);
                have_drive = 1;
            }
        } else if ((type & 0x70) == 0x30 && isz > 14 && have_drive) {   /* file or folder */
            u32 ne = 14;
            int uni = (type & 0x04) != 0;
            part[0] = 0;
            if (uni) {
                while (ne + 1 < isz && g16(it + ne)) ne += 2;
                wide_at(it, 14, isz, (ne - 14) / 2, part, sizeof part, w2a);
                ne += 2;
            } else {
                while (ne < isz && it[ne]) ne++;
                ansi_at(it, 14, isz, part, sizeof part);
                ne++;
            }
            {
                char lng[LNK_TEXT];
                if (long_name(it, isz, ne, part, lng, sizeof lng, w2a)) put(part, sizeof part, lng, LNK_TEXT);
            }
            if (!part[0]) return;
            {
                u32 l = 0;
                while (path[l]) l++;
                if (l && path[l - 1] != '\\') cat(path, sizeof path, "\\");
            }
            cat(path, sizeof path, part);
        }
        p += isz;
    }
    if (have_drive && path[3]) { put(o->target, sizeof o->target, path, LNK_TEXT); o->from = 3; }
}

int lnk_parse(const u8 *b, u32 n, lnk_info *o, lnk_w2a w2a)
{
    static const u8 clsid[16] = {0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    u32 flags, p = 0x4C, idl_off = 0, idl_len = 0, li_off = 0;
    int k;
    memset(o, 0, sizeof *o);
    if (n < 0x4C || g32(b) != 0x4C || memcmp(b + 4, clsid, 16)) return 0;
    flags = g32(b + 0x14);
    if (flags & 1) {                                /* LinkTargetIDList */
        if (p + 2 > n) return 0;
        idl_len = g16(b + p);
        idl_off = p + 2;
        p += 2 + idl_len;
    }
    if (flags & 2) {                                /* LinkInfo */
        if (p + 4 > n) return 0;
        li_off = p;
        p += g32(b + p);
    }
    /* StringData: NAME, RELATIVE_PATH, WORKING_DIR, ARGUMENTS, ICON_LOCATION */
    for (k = 0; k < 5; k++) {
        static const u32 bit[5] = {0x4, 0x8, 0x10, 0x20, 0x40};
        char *dst[5];
        u32 cnt, bytes;
        dst[0] = o->name; dst[1] = o->relative; dst[2] = o->workdir; dst[3] = o->args; dst[4] = 0;
        if (!(flags & bit[k])) continue;
        if (p + 2 > n) break;
        cnt = g16(b + p);
        bytes = (flags & 0x80) ? cnt * 2 : cnt;
        if (p + 2 + bytes > n) break;
        if (dst[k]) {
            if (flags & 0x80) wide_at(b, p + 2, p + 2 + bytes, cnt, dst[k], LNK_TEXT, w2a);
            else put(dst[k], LNK_TEXT, (const char *)b + p + 2, cnt);
        }
        p += 2 + bytes;
    }
    if (li_off && !(flags & 0x100)) link_info(b, n, li_off, o, w2a);
    /* ExtraData blocks: the environment-variable target */
    if (!o->target[0]) {
        u32 q = p;
        while (q + 8 <= n) {
            u32 sz = g32(b + q), sig;
            if (sz < 8 || q + sz > n) break;
            sig = g32(b + q + 4);
            if (sig == 0xA0000001u && sz >= 0x314) {
                wide_at(b, q + 268, q + 268 + 520, 0xffffffffu, o->target, sizeof o->target, w2a);
                if (!o->target[0]) ansi_at(b, q + 8, q + 268, o->target, sizeof o->target);
                if (o->target[0]) { o->target_has_env = 1; o->from = 2; }
                break;
            }
            q += sz;
        }
    }
    if (!o->target[0] && idl_off) id_list(b, n, idl_off, idl_len, o, w2a);
    if (!o->target[0] && o->relative[0]) o->from = 4;
    return o->target[0] || o->relative[0] || o->args[0];
}
