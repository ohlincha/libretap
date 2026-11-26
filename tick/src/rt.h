/* rt.h - the tiny runtime Tick uses instead of a C library.
 *
 * Tick and TickHook.dll are built without any C runtime so they run on
 * Windows 95..11 from a USB stick with nothing installed.  The portable
 * parts (PNG encoder, screen diff, ring buffer, text buffers) are also
 * compiled natively on Linux for the unit tests; then rt.h maps onto libc.
 */
#ifndef TICK_RT_H
#define TICK_RT_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;
typedef long long i64;

#ifdef _WIN32
#include <stddef.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
void *rt_alloc(u32 n);          /* zeroed; NULL on failure */
void rt_free(void *p);
#else
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
static inline void *rt_alloc(u32 n) { return calloc(1, n ? n : 1); }
static inline void rt_free(void *p) { free(p); }
#endif

/* 64-bit unsigned division without the compiler's helper functions. */
u64 rt_udiv64(u64 n, u64 d, u64 *rem);

/* Growable text buffer (for JSON lines, paths, status text). */
typedef struct {
    char *p;
    u32 len, cap;
    int oom;                    /* set when an allocation failed */
} sbuf;

void sb_init(sbuf *b, u32 cap);
void sb_free(sbuf *b);
void sb_reset(sbuf *b);
void sb_reserve(sbuf *b, u32 extra);
void sb_putc(sbuf *b, char c);
void sb_mem(sbuf *b, const void *p, u32 n);
void sb_str(sbuf *b, const char *s);
void sb_u32(sbuf *b, u32 v);
void sb_i32(sbuf *b, i32 v);
void sb_u64(sbuf *b, u64 v);
void sb_u32w(sbuf *b, u32 v, int width);           /* zero padded */
void sb_hex32(sbuf *b, u32 v, int width);          /* lowercase, zero padded */
void sb_hexbytes(sbuf *b, const u8 *p, u32 n);     /* lowercase hex, no separators */
void sb_time(sbuf *b, u64 unix_us);                /* "1796031300.123456" */
/* JSON string with quotes.  Input is UTF-8; control characters are escaped,
 * invalid UTF-8 bytes are replaced by U+FFFD so the output is always valid. */
void sb_json(sbuf *b, const char *s, u32 n);
void sb_jsonz(sbuf *b, const char *s);
/* UTF-16 (n units, or up to NUL when n == 0xffffffff) to UTF-8 */
void sb_utf16(sbuf *b, const u16 *w, u32 n);

#endif
