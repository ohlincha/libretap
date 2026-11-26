/* test_usb_decode.c - native test of the USB paths of the listener without USB hardware:
 * the Linux hook library's usbfs decoding (control, bulk, URB submit/reap) is called with
 * made-up transfers on a "/dev/bus/usb/001/004" handle, and Windows-style device records
 * (\\?\usb#vid_..., \\?\hid#..., WinUSB/HID calls) go through recev.c; the session events
 * are printed and checked.  Build: see Makefile target test-usb. */
#define _GNU_SOURCE
#include "../hook/tickhook_linux.c"         /* the real hook code (its open/read/... too) */
#undef memcpy
#undef memset
#undef TICK_RT_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/rt.h"
#include "../src/recev.h"

/* minimal session stand-ins for recev.c */
static char lines[64][1200];
static int nlines;
void copy_str(char *d, const char *s, u32 cap) { snprintf(d, cap, "%s", s ? s : ""); }
void cat_str(char *d, const char *s, u32 cap) { size_t n = strlen(d); if (n < cap) snprintf(d + n, cap - n, "%s", s); }
int session_has_source(const char *id) { (void)id; return 0; }
void session_add_source(const char *id, const char *json) { printf("source %s: %s\n", id, json); }
void ev_begin(sbuf *b, u64 t, const char *kind) { (void)t; sb_reset(b); sb_str(b, "{\"kind\":\""); sb_str(b, kind); sb_putc(b, '"'); }
void ev_end(sbuf *b) { sb_str(b, "}"); snprintf(lines[nlines++ % 64], 1200, "%s", b->p); printf("%s\n", b->p); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void drain(const char *prog, int posix)
{
    static u8 buf[RING_MAXREC + 256];
    recev_counts rc;
    u32 n;
    memset(&rc, 0, sizeof rc);
    while ((n = ring_get(R, buf, sizeof buf))) {
        const ring_rec *r = (const ring_rec *)buf;
        if (!recev_usb(r, buf + sizeof *r, 0, posix, prog, &rc)) recev_net(r, buf + sizeof *r, 0, posix, prog, &rc);
    }
    if (rc.found[0]) printf("status: %s\n", rc.found);
}

static int has(const char *s)
{
    int i;
    for (i = 0; i < nlines && i < 64; i++) if (strstr(lines[i], s)) return 1;
    return 0;
}

int main(void)
{
    static u8 mem[sizeof(ring_hdr) + RING_DATA];
    int fd = (int)sc(SYS_openat, AT_FDCWD, (long)"/dev/null", O_RDWR, 0, 0, 0);
    u8 data[64];
    R = (ring_hdr *)mem;
    ring_init(R, RING_DATA);
    R->active = 1;
    init_done = 1;

    /* Linux: a usbfs handle */
    after_open("/dev/bus/usb/001/004", fd, O_RDWR);
    {
        struct usbdevfs_ctrltransfer c = {0xc0, 0x01, 0x0002, 0x0000, 4, 1000, data};
        memcpy(data, "\x11\x22\x33\x44", 4);
        usbfs(fd, ports[fd], USBDEVFS_CONTROL, &c, 4);
    }
    {
        struct usbdevfs_bulktransfer b = {0x02, 5, 1000, (void *)"*IDN?"};
        usbfs(fd, ports[fd], USBDEVFS_BULK, &b, 5);
    }
    {
        static u8 ub[64];
        struct usbdevfs_urb u, *pu = &u;
        memset(&u, 0, sizeof u);
        u.type = USBDEVFS_URB_TYPE_BULK; u.endpoint = 0x81; u.buffer = ub; u.buffer_length = 64; u.actual_length = 6;
        memcpy(ub, "ANSWER", 6);
        usbfs(fd, ports[fd], USBDEVFS_SUBMITURB, &u, 0);      /* IN submit: nothing yet */
        usbfs(fd, ports[fd], USBDEVFS_REAPURB, &pu, 0);       /* reaped: 6 bytes in */
        u.type = USBDEVFS_URB_TYPE_CONTROL; u.endpoint = 0;
        memcpy(ub, "\x40\x05\x34\x12\x00\x00\x02\x00\xaa\xbb", 10);
        u.buffer_length = 10;
        usbfs(fd, ports[fd], USBDEVFS_SUBMITURB, &u, 0);      /* control OUT with 2 data bytes */
    }
    drain("vendorprog", 1);
    CHECK(has("\"usb-001-004\"") || has("usb-001-004"));
    CHECK(has("\"setup\":\"c001020000000400\"") && has("\"hex\":\"11223344\""));
    CHECK(has("\"endpoint\":\"0x02\",\"hex\":\"2a49444e3f\""));
    CHECK(has("\"endpoint\":\"0x81\",\"hex\":\"414e53574552\""));
    CHECK(has("\"setup\":\"4005341200000200\"") && has("\"hex\":\"aabb\""));

    /* Windows-style records: a WinUSB device and a HID device path */
    nlines = 0;
    recev_reset();
    {
        ring_rec r;
        const char *p1 = "\\\\?\\usb#vid_0403&pid_6001#A1B2C3#{a5dcbf10-6530-11d2-901f-00c04fb951ed}";
        const char *p2 = "\\\\?\\hid#vid_046d&pid_c52b&mi_02#7&1d0bb5f4&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        u8 pay[64];
        memset(&r, 0, sizeof r);
        r.type = REC_OPEN; r.a = 1; memcpy(r.port, "u1", 3);
        ring_put(R, &r, p1, (u32)strlen(p1), 0, 0);
        r.type = REC_USB_CTRL; r.a = 1; r.b = 2; r.c = CALL_WINUSB_CONTROL;
        memcpy(pay, "\xc0\x01\x00\x00\x00\x00\x02\x00\x99\x98", 10);
        ring_put(R, &r, pay, 10, 0, 0);
        r.type = REC_USB_XFER; r.b = 0x02; r.c = CALL_WINUSB_WRITE;
        ring_put(R, &r, "HELLO", 5, 0, 0);
        memcpy(r.port, "u2", 3);
        r.type = REC_OPEN; r.a = 1;
        ring_put(R, &r, p2, (u32)strlen(p2), 0, 0);
        r.type = REC_USB_XFER; r.b = 0x80; r.c = CALL_HID_GETFEATURE;
        ring_put(R, &r, "\x00\x01\x02", 3, 0, 0);
        drain("VENDOR.EXE", 0);
    }
    CHECK(has("\"src\":\"usb-vid_0403-pid_6001\"") && has("\"transfer\":\"control\""));
    CHECK(has("\"endpoint\":\"0x02\",\"hex\":\"48454c4c4f\",\"call\":\"WinUsb_WritePipe\""));
    CHECK(has("\"src\":\"hid-vid_046d-pid_c52b"));
    CHECK(has("\"report\":\"feature\",\"hex\":\"000102\",\"call\":\"HidD_GetFeature\""));
    printf(fails ? "%d FAILED\n" : "usb decode tests passed\n", fails);
    return fails != 0;
}
