/* recev.h - network and USB records -> events (recev.c) */
#ifndef TICK_RECEV_H
#define TICK_RECEV_H
#include "core.h"
#include "ring.h"

typedef struct {
    u32 net_in, net_out, usb_in, usb_out;
    char found[400];        /* plain words: "VENDOR.EXE connected to 192.168.0.20:5000 (TCP)" */
} recev_counts;

void recev_reset(void);
/* 1 if the record was a network (or, for recev_usb, a 'u' device) record and was handled */
int recev_net(const ring_rec *r, const u8 *p, u64 t, int posix, const char *prog, recev_counts *c);
int recev_usb(const ring_rec *r, const u8 *p, u64 t, int posix, const char *prog, recev_counts *c);
void recev_addr(sbuf *b, const u8 *sockaddr_bytes);

#endif
