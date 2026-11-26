/* ring.c - see ring.h */
#include "ring.h"

#define DATA(h) ((u8 *)((h) + 1))

void ring_init(ring_hdr *h, u32 size)
{
    memset(h, 0, sizeof *h + size);
    h->magic = RING_MAGIC;
    h->version = RING_VERSION;
    h->size = size;
}

static void copy_in(ring_hdr *h, u32 off, const void *src, u32 n)
{
    u32 m = h->size - 1, o = off & m, first = h->size - o;
    if (!n) return;
    if (first >= n) memcpy(DATA(h) + o, src, n);
    else {
        memcpy(DATA(h) + o, src, first);
        memcpy(DATA(h), (const u8 *)src + first, n - first);
    }
}

static void copy_out(ring_hdr *h, u32 off, void *dst, u32 n)
{
    u32 m = h->size - 1, o = off & m, first = h->size - o;
    if (first >= n) memcpy(dst, DATA(h) + o, n);
    else {
        memcpy(dst, DATA(h) + o, first);
        memcpy((u8 *)dst + first, DATA(h), n - first);
    }
}

static void zero(ring_hdr *h, u32 off, u32 n)
{
    u32 m = h->size - 1, o = off & m, first = h->size - o;
    if (first >= n) memset(DATA(h) + o, 0, n);
    else {
        memset(DATA(h) + o, 0, first);
        memset(DATA(h), 0, n - first);
    }
}

int ring_put(ring_hdr *h, ring_rec *rec, const void *p1, u32 n1, const void *p2, u32 n2)
{
    u32 len = ((u32)sizeof(ring_rec) + n1 + n2 + 3) & ~3u, r;
    if (len > h->size / 4) { __sync_fetch_and_add(&h->drops, 1); return 0; }
    for (;;) {
        u32 t;
        r = h->reserve;
        t = h->tail;
        if (r - t + len > h->size) { __sync_fetch_and_add(&h->drops, 1); return 0; }
        if (__sync_bool_compare_and_swap(&h->reserve, r, r + len)) break;
    }
    rec->dlen = n1 + n2;
    *(volatile u32 *)(DATA(h) + (r & (h->size - 1))) = len;     /* length known, not done */
    copy_in(h, r + 4, (const u8 *)rec + 4, (u32)sizeof(ring_rec) - 4);
    copy_in(h, r + (u32)sizeof(ring_rec), p1, n1);
    copy_in(h, r + (u32)sizeof(ring_rec) + n1, p2, n2);
    __sync_synchronize();
    *(volatile u32 *)(DATA(h) + (r & (h->size - 1))) = len | RING_DONE;
    return 1;
}

u32 ring_get(ring_hdr *h, void *buf, u32 cap)
{
    u32 t = h->tail, hdr, len;
    if (t == h->reserve) return 0;
    hdr = *(volatile u32 *)(DATA(h) + (t & (h->size - 1)));
    if (!(hdr & RING_DONE)) return 0;
    __sync_synchronize();
    len = hdr & ~RING_DONE;
    if (len < sizeof(ring_rec) || len > h->size / 4 || (len & 3)) {   /* corrupt: resync */
        ring_skip(h);
        return 0;
    }
    copy_out(h, t, buf, len < cap ? len : cap);
    zero(h, t, len);
    __sync_synchronize();
    h->tail = t + len;
    return len < cap ? len : cap;
}

void ring_skip(ring_hdr *h)
{
    u32 t = h->tail, r = h->reserve, len;
    if (t == r) return;
    len = *(volatile u32 *)(DATA(h) + (t & (h->size - 1))) & ~RING_DONE;
    if (len < sizeof(ring_rec) || len > r - t || (len & 3)) len = r - t;   /* lost track: drop all */
    zero(h, t, len);
    __sync_synchronize();
    h->tail = t + len;
    __sync_fetch_and_add(&h->drops, 1);
}
