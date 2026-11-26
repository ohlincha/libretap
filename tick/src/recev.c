/* recev.c - network and USB/device records from the hooks (ring.h) as
 * session events (docs/FORMAT.md); shared by Tick for Windows and Linux.
 * Serial (COM/tty) records stay in launch.c / posix/listener_posix.c. */
#include "recev.h"

static struct { char port[8]; char path[260]; char id[48]; } devs[64];
static int ndevs;

void recev_reset(void) { ndevs = 0; }

static void put_hex_u(sbuf *b, u32 v, int w) { sb_str(b, "0x"); sb_hex32(b, v, w); }

/* "192.168.0.20:5000" or "[fe80::1]:5000" from sockaddr_in / sockaddr_in6 bytes */
void recev_addr(sbuf *b, const u8 *sa)
{
    u16 fam = (u16)(sa[0] | sa[1] << 8);
    u32 port = (u32)(sa[2] << 8 | sa[3]);
    if (fam == 2) {
        sb_u32(b, sa[4]); sb_putc(b, '.'); sb_u32(b, sa[5]); sb_putc(b, '.');
        sb_u32(b, sa[6]); sb_putc(b, '.'); sb_u32(b, sa[7]);
    } else if (fam == 23 || fam == 10) {        /* AF_INET6 on Windows / Linux */
        int i;
        sb_putc(b, '[');
        for (i = 0; i < 8; i++) {
            if (i) sb_putc(b, ':');
            sb_hex32(b, (u32)(sa[8 + 2 * i] << 8 | sa[9 + 2 * i]), 1);
        }
        sb_putc(b, ']');
    } else { sb_str(b, "?"); return; }
    sb_putc(b, ':');
    sb_u32(b, port);
}

static int addr_known(const u8 *sa) { return sa[0] || sa[1]; }

static void net_fields(sbuf *b, const ring_rec *r, const tick_net *ni)
{
    sb_str(b, ",\"src\":\"net\",\"conn\":\"");
    sb_str(b, r->port);
    sb_str(b, ni->proto == 17 ? "\",\"proto\":\"udp\"" : "\",\"proto\":\"tcp\"");
    if (addr_known(ni->local)) { sb_str(b, ",\"local\":\""); recev_addr(b, ni->local); sb_putc(b, '"'); }
    if (addr_known(ni->remote)) { sb_str(b, ",\"remote\":\""); recev_addr(b, ni->remote); sb_putc(b, '"'); }
}

static void ensure_net_source(const char *prog)
{
    sbuf b;
    if (session_has_source("net")) return;
    sb_init(&b, 200);
    sb_str(&b, "{\"id\": \"net\", \"kind\": \"api-net\", \"label\": ");
    {
        sbuf l;
        sb_init(&l, 100);
        sb_str(&l, "Network (");
        sb_str(&l, prog);
        sb_putc(&l, ')');
        sb_jsonz(&b, l.p);
        sb_free(&l);
    }
    sb_str(&b, ", \"process\": ");
    sb_jsonz(&b, prog);
    sb_putc(&b, '}');
    session_add_source("net", b.p);
    sb_free(&b);
}

int recev_net(const ring_rec *r, const u8 *p, u64 t, int posix, const char *prog, recev_counts *c)
{
    const tick_net *ni = (const tick_net *)p;
    const u8 *data = p + sizeof(tick_net);
    u32 n = r->dlen >= sizeof(tick_net) ? r->dlen - (u32)sizeof(tick_net) : 0;
    sbuf b;
    if (r->type < REC_NET_OPEN || r->type > REC_NET_CLOSE || r->dlen < sizeof(tick_net)) return 0;
    ensure_net_source(prog);
    sb_init(&b, 256 + n * 2);
    if (r->type == REC_NET_OPEN) {
        ev_begin(&b, t, "open");
        net_fields(&b, r, ni);
        sb_str(&b, ",\"call\":\""); sb_str(&b, ring_call_name(r->c, posix)); sb_putc(&b, '"');
        sb_str(&b, r->a ? ",\"ok\":true" : ",\"ok\":false");
        if (r->b) { sb_str(&b, ",\"error\":"); sb_u32(&b, r->b); }
        if (r->a && c) {    /* plain words for the window */
            sbuf w;
            sb_init(&w, 200);
            sb_str(&w, prog);
            sb_str(&w, r->c == CALL_ACCEPT ? " accepted a connection from " : " connected to ");
            if (addr_known(ni->remote)) recev_addr(&w, ni->remote); else sb_str(&w, "?");
            sb_str(&w, ni->proto == 17 ? " (UDP)" : " (TCP)");
            copy_str(c->found, w.p ? w.p : "", sizeof c->found);
            sb_free(&w);
        }
    } else if (r->type == REC_NET_CLOSE) {
        ev_begin(&b, t, "close");
        net_fields(&b, r, ni);
    } else {
        int out = r->type == REC_NET_SEND;
        ev_begin(&b, t, "data");
        net_fields(&b, r, ni);
        sb_str(&b, out ? ",\"dir\":\"out\",\"hex\":\"" : ",\"dir\":\"in\",\"hex\":\"");
        sb_hexbytes(&b, data, n);
        sb_str(&b, "\",\"call\":\""); sb_str(&b, ring_call_name(r->c, posix));
        sb_str(&b, "\",\"n\":"); sb_u32(&b, n);
        if (out) {
            if (r->b == 0xffffffffu) sb_str(&b, ",\"pending\":true");
            else if (r->b != n && r->a == n) { sb_str(&b, ",\"sent\":"); sb_u32(&b, r->b); }
        } else { sb_str(&b, ",\"req\":"); sb_u32(&b, r->a); }
        if (c) { if (out) c->net_out += n; else c->net_in += n; }
        if (c && !c->found[0] && addr_known(ni->remote)) {     /* UDP without connect */
            sbuf w;
            sb_init(&w, 200);
            sb_str(&w, prog);
            sb_str(&w, out ? " sends to " : " receives from ");
            recev_addr(&w, ni->remote);
            sb_str(&w, ni->proto == 17 ? " (UDP)" : " (TCP)");
            copy_str(c->found, w.p ? w.p : "", sizeof c->found);
            sb_free(&w);
        }
    }
    ev_end(&b);
    sb_free(&b);
    return 1;
}

/* ---------------- USB and other devices ---------------- */

static int dev_index(const char *port)
{
    int i;
    for (i = 0; i < ndevs; i++) if (!strcmp(devs[i].port, port)) return i;
    return -1;
}

/* "\\?\usb#vid_0403&pid_6001#..." -> usb-vid_0403-pid_6001; "/dev/bus/usb/001/004" -> usb-001-004 */
static void dev_id(char *id, u32 cap, const char *path)
{
    const char *p = path;
    u32 n = 0;
    int dashes = 0;
    if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "\\\\.\\", 4)) p += 4;
    else if (!strncmp(p, "/dev/bus/usb/", 13)) { copy_str(id, "usb-", cap); n = 4; p += 13; }
    else if (!strncmp(p, "/dev/", 5)) p += 5;
    for (; *p && n + 1 < cap; p++) {
        char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') id[n++] = ch;
        else if (n && id[n - 1] != '-') {
            if (ch == '#' && ++dashes >= 2) break;  /* drop the instance id after vid/pid */
            if (ch == '{') break;
            id[n++] = '-';
        }
    }
    while (n && id[n - 1] == '-') n--;
    id[n] = 0;
    if (!n) copy_str(id, "device", cap);
    if (strncmp(id, "usb", 3) && strncmp(id, "hid", 3)) {   /* a vendor driver name: mark it as a device */
        char t[48];
        copy_str(t, "dev-", sizeof t);
        cat_str(t, id, sizeof t);
        copy_str(id, t, cap);
    }
}

static void words_for_device(char *out, u32 cap, const char *prog, const char *path)
{
    sbuf w;
    const char *v = 0, *q;
    sb_init(&w, 200);
    sb_str(&w, prog);
    for (q = path; *q; q++)
        if ((q[0] | 32) == 'v' && (q[1] | 32) == 'i' && (q[2] | 32) == 'd' && q[3] == '_') { v = q + 4; break; }
    if (v && v[4] == '&' && (v[5] | 32) == 'p' && (v[6] | 32) == 'i' && (v[7] | 32) == 'd' && v[8] == '_') {
        sb_str(&w, strstr(path, "hid#") || strstr(path, "HID#") ? " opened HID device " : " opened USB device ");
        sb_str(&w, "VID ");
        sb_mem(&w, v, 4);
        sb_str(&w, " PID ");
        sb_mem(&w, v + 9, 4);
    } else {
        sb_str(&w, " opened device ");
        sb_str(&w, path);
    }
    copy_str(out, w.p ? w.p : "", cap);
    sb_free(&w);
}

int recev_usb(const ring_rec *r, const u8 *p, u64 t, int posix, const char *prog, recev_counts *c)
{
    char port[8], id[48];
    int di;
    sbuf b;
    memcpy(port, r->port, 8);
    port[7] = 0;
    if (port[0] != 'u') return 0;
    di = dev_index(port);
    if (r->type == REC_OPEN) {
        char path[260];
        u32 n = r->dlen < sizeof path - 1 ? r->dlen : (u32)sizeof path - 1;
        memcpy(path, p, n);
        path[n] = 0;
        if (di < 0 && ndevs < 64) di = ndevs++;
        if (di < 0) return 1;
        copy_str(devs[di].port, port, sizeof devs[di].port);
        copy_str(devs[di].path, path, sizeof devs[di].path);
        dev_id(devs[di].id, sizeof devs[di].id, path);
        if (!session_has_source(devs[di].id)) {
            sbuf s;
            sb_init(&s, 400);
            sb_str(&s, "{\"id\": \"");
            sb_str(&s, devs[di].id);
            sb_str(&s, "\", \"kind\": \"api-usb\", \"label\": ");
            {
                sbuf l;
                sb_init(&l, 300);
                sb_str(&l, path);
                sb_str(&l, " (");
                sb_str(&l, prog);
                sb_putc(&l, ')');
                sb_jsonz(&s, l.p);
                sb_free(&l);
            }
            sb_str(&s, ", \"process\": ");
            sb_jsonz(&s, prog);
            sb_str(&s, ", \"device\": ");
            sb_jsonz(&s, path);
            sb_putc(&s, '}');
            session_add_source(devs[di].id, s.p);
            sb_free(&s);
        }
        sb_init(&b, 400);
        ev_begin(&b, t, "open");
        sb_str(&b, ",\"src\":\""); sb_str(&b, devs[di].id);
        sb_str(&b, "\",\"path\":"); sb_jsonz(&b, path);
        sb_str(&b, r->a ? ",\"ok\":true" : ",\"ok\":false");
        if (!r->a) { sb_str(&b, ",\"error\":"); sb_u32(&b, r->b); }
        ev_end(&b);
        sb_free(&b);
        if (r->a && c) words_for_device(c->found, sizeof c->found, prog, path);
        return 1;
    }
    if (di < 0) return 1;
    copy_str(id, devs[di].id, sizeof id);
    sb_init(&b, 256 + r->dlen * 2);
    switch (r->type) {
    case REC_CLOSE:
        ev_begin(&b, t, "close");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id); sb_putc(&b, '"');
        break;
    case REC_READ:
    case REC_WRITE:
        ev_begin(&b, t, "data");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, r->type == REC_READ ? "\",\"dir\":\"in\",\"hex\":\"" : "\",\"dir\":\"out\",\"hex\":\"");
        sb_hexbytes(&b, p, r->dlen);
        sb_str(&b, "\",\"call\":\""); sb_str(&b, ring_call_name(r->c, posix));
        sb_str(&b, "\",\"n\":"); sb_u32(&b, r->dlen);
        if (c) { if (r->type == REC_READ) c->usb_in += r->dlen; else c->usb_out += r->dlen; }
        break;
    case REC_USB_XFER: {
        int in = (r->b & 0x80) != 0;
        int hid = r->c >= CALL_HID_GETFEATURE && r->c <= CALL_HID_SETOUTPUT;
        ev_begin(&b, t, "data");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, in ? "\",\"dir\":\"in\"" : "\",\"dir\":\"out\"");
        if (hid) {
            sb_str(&b, r->c == CALL_HID_GETFEATURE || r->c == CALL_HID_SETFEATURE ? ",\"report\":\"feature\""
                       : r->c == CALL_HID_GETINPUT ? ",\"report\":\"input\"" : ",\"report\":\"output\"");
        } else { sb_str(&b, ",\"endpoint\":\""); put_hex_u(&b, r->b & 0xff, 2); sb_putc(&b, '"'); }
        sb_str(&b, ",\"hex\":\""); sb_hexbytes(&b, p, r->dlen);
        sb_str(&b, "\",\"call\":\""); sb_str(&b, ring_call_name(r->c, posix));
        sb_str(&b, "\",\"n\":"); sb_u32(&b, r->dlen);
        if (!r->a) sb_str(&b, ",\"ok\":false");
        if (c) { if (in) c->usb_in += r->dlen; else c->usb_out += r->dlen; }
        break;
    }
    case REC_USB_CTRL:
        if (r->dlen < 8) { sb_free(&b); return 1; }
        ev_begin(&b, t, "data");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, (p[0] & 0x80) ? "\",\"dir\":\"in\"" : "\",\"dir\":\"out\"");
        sb_str(&b, ",\"transfer\":\"control\",\"setup\":\""); sb_hexbytes(&b, p, 8);
        sb_str(&b, "\",\"request_type\":\""); put_hex_u(&b, p[0], 2);
        sb_str(&b, "\",\"request\":"); sb_u32(&b, p[1]);
        sb_str(&b, ",\"value\":"); sb_u32(&b, (u32)(p[2] | p[3] << 8));
        sb_str(&b, ",\"index\":"); sb_u32(&b, (u32)(p[4] | p[5] << 8));
        sb_str(&b, ",\"length\":"); sb_u32(&b, (u32)(p[6] | p[7] << 8));
        sb_str(&b, ",\"hex\":\""); sb_hexbytes(&b, p + 8, r->dlen - 8);
        sb_str(&b, "\",\"call\":\""); sb_str(&b, ring_call_name(r->c, posix));
        sb_str(&b, "\",\"n\":"); sb_u32(&b, r->dlen - 8);
        if (!r->a) sb_str(&b, ",\"ok\":false");
        if (c) { if (p[0] & 0x80) c->usb_in += r->dlen - 8; else c->usb_out += r->dlen - 8; }
        break;
    case REC_IOCTL:
        ev_begin(&b, t, "call");
        sb_str(&b, ",\"src\":\""); sb_str(&b, id);
        sb_str(&b, posix ? "\",\"call\":\"ioctl\",\"code\":\"" : "\",\"call\":\"DeviceIoControl\",\"code\":\"");
        put_hex_u(&b, r->a, 8);
        sb_str(&b, "\",\"in\":\"");
        if (r->b <= r->dlen) sb_hexbytes(&b, p, r->b);
        sb_str(&b, "\",\"out\":\"");
        if (r->b + r->c <= r->dlen) sb_hexbytes(&b, p + r->b, r->c);
        sb_putc(&b, '"');
        break;
    default:
        sb_free(&b);
        return 1;
    }
    ev_end(&b);
    sb_free(&b);
    return 1;
}
