/* ring.h - lock-free event ring shared by TickHook.dll (writers: any thread
 * of the vendor program) and Tick (one reader).
 *
 * Writers never wait: they reserve space with one compare-and-swap; if the
 * ring is full the record is dropped and 'drops' is counted.  Offsets are
 * free-running u32 counters; the data area size is a power of two.
 * Record layout: u32 header (total length, multiple of 4; bit 31 = done),
 * then the record body.  The reader zeroes what it consumed. */
#ifndef TICK_RING_H
#define TICK_RING_H
#include "rt.h"

#define RING_MAGIC   0x4b434954u    /* "TICK" */
#define RING_VERSION 1
#define RING_DATA    (1u << 20)     /* 1 MB */
#define RING_DONE    0x80000000u
#define RING_MAXREC  (64u * 1024)   /* larger payloads are split */

typedef struct {
    u32 magic, version, size;
    volatile u32 active;            /* 1 while Tick reads; writers skip work when 0 */
    volatile u32 reserve;           /* next free offset (writers) */
    volatile u32 tail;              /* next offset to read (reader) */
    volatile u32 drops;             /* records dropped because the ring was full */
    volatile u32 hooked;            /* number of import slots patched (status) */
    u32 tick_pid, target_pid;
    u32 pad[6];
} ring_hdr;                         /* 64 bytes, followed by 'size' data bytes */

/* Record types (body = ring_rec, then 'dlen' payload bytes) */
enum {
    REC_TEXT = 1,       /* payload: UTF-8 status text */
    REC_OPEN,           /* a = ok, b = GetLastError; payload: UTF-8 path */
    REC_CLOSE,
    REC_READ,           /* a = requested; payload: bytes read */
    REC_WRITE,          /* a = requested, b = written (or ~0 if pending); payload: bytes */
    REC_DCB,            /* a = ok; payload: DCB (28 bytes) */
    REC_TIMEOUTS,       /* a = ok; payload: COMMTIMEOUTS (20 bytes) */
    REC_ESCAPE,         /* a = function, b = ok */
    REC_IOCTL,          /* a = code, b = in length, c = out length; payload: in then out */
    REC_PURGE,          /* a = flags */
    REC_SETUP,          /* a = in queue, b = out queue */
    REC_ERROR           /* payload: UTF-8 text */
};

#define RECF_QPC 1      /* t is a QueryPerformanceCounter value, else a FILETIME */

typedef struct {
    u32 hdr;
    u16 type, flags;
    u32 tid;
    u32 handle;
    u64 t;
    u32 a, b, c;
    u32 dlen;
    char port[8];       /* "COM1" ... */
} ring_rec;             /* 48 bytes */

/* writer: copies rec (hdr is filled in) and up to two payload pieces;
 * returns 0 when dropped */
int ring_put(ring_hdr *h, ring_rec *rec, const void *p1, u32 n1, const void *p2, u32 n2);

/* reader: copies the next complete record into buf (cap bytes) and returns
 * its length; 0 when there is nothing (yet).  A record that stays unfinished
 * longer than the caller tolerates can be skipped with ring_skip. */
u32 ring_get(ring_hdr *h, void *buf, u32 cap);
void ring_skip(ring_hdr *h);
void ring_init(ring_hdr *h, u32 size);

#endif
