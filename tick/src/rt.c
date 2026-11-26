/* rt.c - see rt.h */
#include "rt.h"

#ifdef _WIN32
#include <windows.h>

/* The compiler may emit calls to these four even with -ffreestanding.
 * Written with string instructions so clang cannot turn them back into
 * calls to themselves. */
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n)
{
    void *r = d;
    if ((const u8 *)d <= (const u8 *)s || (const u8 *)d >= (const u8 *)s + n) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d = (u8 *)d + n - 1;
        s = (const u8 *)s + n - 1;
        __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    /* compare 4 bytes at a time first: this is the screen diff hot path */
    while (n >= 4 && *(const u32 *)x == *(const u32 *)y) { x += 4; y += 4; n -= 4; }
    while (n) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++; n--;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

void *rt_alloc(u32 n)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1);
}

void rt_free(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}
#endif

u64 rt_udiv64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0;
    int i;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    if ((d >> 32) == 0 && (n >> 32) == 0) {
        u32 nn = (u32)n, dd = (u32)d;
        if (rem) *rem = nn % dd;
        return nn / dd;
    }
    {
        u64 r = 0;
        for (i = 63; i >= 0; i--) {
            r = (r << 1) | ((n >> i) & 1);
            if (r >= d) { r -= d; q |= (u64)1 << i; }
        }
        if (rem) *rem = r;
    }
    return q;
}

void sb_init(sbuf *b, u32 cap)
{
    b->len = 0;
    b->oom = 0;
    b->cap = cap ? cap : 64;
    b->p = rt_alloc(b->cap + 1);
    if (!b->p) { b->cap = 0; b->oom = 1; }
}

void sb_free(sbuf *b)
{
    rt_free(b->p);
    b->p = 0;
    b->len = b->cap = 0;
}

void sb_reset(sbuf *b)
{
    b->len = 0;
    if (b->p) b->p[0] = 0;
}

void sb_reserve(sbuf *b, u32 extra)
{
    u32 need = b->len + extra, ncap;
    char *np;
    if (need <= b->cap) return;
    ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    np = rt_alloc(ncap + 1);
    if (!np) { b->oom = 1; return; }
    if (b->p) { memcpy(np, b->p, b->len); rt_free(b->p); }
    b->p = np;
    b->cap = ncap;
}

void sb_mem(sbuf *b, const void *p, u32 n)
{
    sb_reserve(b, n);
    if (b->len + n > b->cap) return;
    memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_putc(sbuf *b, char c)
{
    if (b->len + 1 > b->cap) sb_reserve(b, 1);
    if (b->len + 1 > b->cap) return;
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void sb_str(sbuf *b, const char *s)
{
    if (s) sb_mem(b, s, (u32)strlen(s));
}

void sb_u32w(sbuf *b, u32 v, int width)
{
    char t[12];
    int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i < width && i < 12) t[i++] = '0';
    while (i) sb_putc(b, t[--i]);
}

void sb_u32(sbuf *b, u32 v) { sb_u32w(b, v, 0); }

void sb_i32(sbuf *b, i32 v)
{
    if (v < 0) { sb_putc(b, '-'); sb_u32(b, (u32)(-(v + 1)) + 1); }
    else sb_u32(b, (u32)v);
}

void sb_u64(sbuf *b, u64 v)
{
    char t[24];
    int i = 0;
    do {
        u64 r;
        v = rt_udiv64(v, 10, &r);
        t[i++] = (char)('0' + (int)r);
    } while (v);
    while (i) sb_putc(b, t[--i]);
}

static const char hexd[] = "0123456789abcdef";

void sb_hex32(sbuf *b, u32 v, int width)
{
    char t[8];
    int i = 0;
    do { t[i++] = hexd[v & 15]; v >>= 4; } while (v && i < 8);
    while (i < width && i < 8) t[i++] = '0';
    while (i) sb_putc(b, t[--i]);
}

void sb_hexbytes(sbuf *b, const u8 *p, u32 n)
{
    u32 i;
    char *o;
    sb_reserve(b, n * 2);
    if (b->len + n * 2 > b->cap) return;
    o = b->p + b->len;
    for (i = 0; i < n; i++) {
        *o++ = hexd[p[i] >> 4];
        *o++ = hexd[p[i] & 15];
    }
    b->len += n * 2;
    b->p[b->len] = 0;
}

void sb_time(sbuf *b, u64 unix_us)
{
    u64 frac;
    u64 sec = rt_udiv64(unix_us, 1000000, &frac);
    sb_u64(b, sec);
    sb_putc(b, '.');
    sb_u32w(b, (u32)frac, 6);
}

static void sb_cp(sbuf *b, u32 c)   /* code point as UTF-8 */
{
    if (c < 0x80) sb_putc(b, (char)c);
    else if (c < 0x800) {
        sb_putc(b, (char)(0xc0 | (c >> 6)));
        sb_putc(b, (char)(0x80 | (c & 63)));
    } else if (c < 0x10000) {
        sb_putc(b, (char)(0xe0 | (c >> 12)));
        sb_putc(b, (char)(0x80 | ((c >> 6) & 63)));
        sb_putc(b, (char)(0x80 | (c & 63)));
    } else {
        sb_putc(b, (char)(0xf0 | (c >> 18)));
        sb_putc(b, (char)(0x80 | ((c >> 12) & 63)));
        sb_putc(b, (char)(0x80 | ((c >> 6) & 63)));
        sb_putc(b, (char)(0x80 | (c & 63)));
    }
}

void sb_json(sbuf *b, const char *s, u32 n)
{
    const u8 *p = (const u8 *)s, *e = p + n;
    sb_putc(b, '"');
    while (p < e) {
        u32 c = *p;
        if (c < 0x80) {
            p++;
            if (c == '"' || c == '\\') { sb_putc(b, '\\'); sb_putc(b, (char)c); }
            else if (c == '\n') sb_str(b, "\\n");
            else if (c == '\r') sb_str(b, "\\r");
            else if (c == '\t') sb_str(b, "\\t");
            else if (c < 0x20 || c == 0x7f) { sb_str(b, "\\u00"); sb_hex32(b, c, 2); }
            else sb_putc(b, (char)c);
            continue;
        }
        {   /* validate one UTF-8 sequence */
            int need = c >= 0xf0 && c < 0xf5 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
            u32 cp = need == 3 ? c & 7 : need == 2 ? c & 15 : c & 31;
            int i;
            if (need > 0 && p + need < e) {
                for (i = 1; i <= need; i++) {
                    if ((p[i] & 0xc0) != 0x80) break;
                    cp = (cp << 6) | (p[i] & 63);
                }
                if (i > need && !(need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp < 0xe000)))
                    && !(need == 3 && (cp < 0x10000 || cp > 0x10ffff))) {
                    sb_mem(b, p, (u32)need + 1);
                    p += need + 1;
                    continue;
                }
            }
            sb_cp(b, 0xfffd);
            p++;
        }
    }
    sb_putc(b, '"');
}

void sb_jsonz(sbuf *b, const char *s)
{
    sb_json(b, s ? s : "", s ? (u32)strlen(s) : 0);
}

void sb_utf16(sbuf *b, const u16 *w, u32 n)
{
    u32 i;
    for (i = 0; i < n && (n != 0xffffffffu || w[i]); i++) {
        u32 c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        } else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        sb_cp(b, c);
    }
}
