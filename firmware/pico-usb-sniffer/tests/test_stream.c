// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, LibreTap contributors.
//
// Host tests for USB Sniffer Stream: synthetic USB traffic goes through a stand-in for the capture PIO (usbline.h),
// core 1's capture loop body (cap_word from capture_ring.h, the code the firmware runs), the ring consumer and the
// filter/encoder (stream.c); the records are parsed back and checked. Build and run: make -C tests
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_now; // us: the Pico's timer
#define CAP_NOW() g_now
#define CAP_DMB()
#include "../firmware/capture_ring.h"
#include "../firmware/stream.h"
#include "usbline.h"

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/*- The firmware's pieces, wired as on the Pico ------------------------------*/

static uint32_t ring[CAP_RING_WORDS];
static cap_t cap = { .ring = ring };
static se_t se;
static uint32_t rd;

static uint8_t out[4 << 20];
static int out_len, out_cap;

static int out_space(void *ctx) { (void)ctx; return out_cap - out_len; }
static void out_write(void *ctx, const uint8_t *d, int n)
{
  (void)ctx;
  if (out_len + n > out_cap) { printf("  overflow of the output (the encoder did not check space)\n"); g_failures++; return; }
  memcpy(out + out_len, d, n);
  out_len += n;
}
static const se_out_t sink = { out_space, out_write, 0 };

static void push_cb(uint32_t w, void *ctx) { (void)ctx; cap_word(&cap, w); }
static void noop_cb(uint32_t w, void *ctx) { (void)w; (void)ctx; }
static pio_t pio;

static void start(bool fs, bool keep_all, bool text)
{
  memset(ring, 0, sizeof(ring));
  cap_reset(&cap);
  rd = 0;
  out_len = 0;
  out_cap = sizeof(out);
  g_now = 1000;
  pio = (pio_t){ .fs = fs, .push = push_cb };
  se_start(&se, &sink, fs, keep_all, text, g_now, sizeof(ring), sizeof(out));
}

static void consume(void)
{
  while (rd != cap.wr)
  {
    rd = se_consume(&se, ring, CAP_MASK, rd);
    cap.rd = rd;
  }
}

// One packet on the bus; returns its start time (us). The clock moves on by the packet's length and a small gap.
static uint32_t send_ex(const uint8_t *b, int n, bool stuff_error)
{
  pio_t dry = { .fs = pio.fs, .push = noop_cb };
  int bits = line_packet(&dry, b, n, stuff_error);
  uint32_t t0 = g_now;
  g_now = t0 + (pio.fs ? (uint32_t)(bits + 11) / 12 : (uint32_t)(bits * 2 + 2) / 3);
  line_packet(&pio, b, n, stuff_error);
  g_now += 1;
  return t0;
}
static uint32_t send(const uint8_t *b, int n) { return send_ex(b, n, false); }

static uint32_t tok(int pid, int addr, int ep) { uint8_t p[3]; pkt_token(p, pid, addr, ep); return send(p, 3); }
static uint32_t dat(int pid, const void *d, int n) { uint8_t p[2048]; int k = pkt_data(p, pid, d, n); return send(p, k); }
static uint32_t hs(int pid) { uint8_t p[1]; pkt_handshake(p, pid); return send(p, 1); }
static int g_frame = 100;
static void sof(void) { uint8_t p[3]; pkt_sof(p, g_frame++ & 0x7ff); g_now = (g_now / 1000 + 1) * 1000; send(p, 3); }

/*- Reading the records back ------------------------------------------------*/

typedef struct
{
  int type;
  uint32_t time;
  int len;
  uint8_t p[2100];
} rec_t;

static rec_t recs[20000];
static int nrecs, skipped;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The reader LibreTap implements: find 0xA5, check the header and CRC, else skip one byte and look again
static void parse(const uint8_t *b, int n)
{
  nrecs = 0;
  skipped = 0;
  int i = 0;
  while (i < n)
  {
    if (b[i] != SE_SYNC) { i++; skipped++; continue; }
    if (i + SE_HEADER_SIZE > n) break;
    int len = b[i + 2] | b[i + 3] << 8;
    if (b[i + 1] < SE_HELLO || b[i + 1] > SE_END || len > SE_MAX_PAYLOAD) { i++; skipped++; continue; }
    if (i + SE_RECORD_EXTRA + len > n) break;
    uint16_t crc = se_crc16(b + i + 1, SE_HEADER_SIZE - 1 + len, 0xffff);
    if ((b[i + SE_HEADER_SIZE + len] | b[i + SE_HEADER_SIZE + len + 1] << 8) != crc) { i++; skipped++; continue; }
    if (nrecs < (int)(sizeof(recs) / sizeof(recs[0])))
    {
      rec_t *r = &recs[nrecs++];
      r->type = b[i + 1];
      r->time = rd32(b + i + 4);
      r->len = len;
      memcpy(r->p, b + i + SE_HEADER_SIZE, len);
    }
    i += SE_RECORD_EXTRA + len;
  }
}

static int count_type(int type)
{
  int n = 0;
  for (int i = 0; i < nrecs; i++)
    n += recs[i].type == type;
  return n;
}

static const rec_t *find_type(int type, int nth)
{
  for (int i = 0; i < nrecs; i++)
    if (recs[i].type == type && nth-- == 0)
      return &recs[i];
  return 0;
}

static int rec_pid(const rec_t *r) { return (r->type == SE_PACKET && r->len >= 2) ? (r->p[1] & 0xf) : -1; }

// The PACKET records' PIDs as a string like "SETUP DATA0 ACK"
static void pids(char *s, int max)
{
  static const char *names[16] = { "RES", "OUT", "ACK", "DATA0", "PING", "SOF", "NYET", "DATA2",
                                   "SPLIT", "IN", "NAK", "DATA1", "PRE", "SETUP", "STALL", "MDATA" };
  s[0] = 0;
  for (int i = 0; i < nrecs; i++)
    if (recs[i].type == SE_PACKET && (int)strlen(s) + 8 < max)
    {
      if (s[0]) strcat(s, " ");
      strcat(s, names[rec_pid(&recs[i])]);
    }
}

static uint32_t status_u32(const rec_t *r, int off) { return rd32(r->p + off); }

/*- Tests -------------------------------------------------------------------*/

static uint32_t got[512];
static int ngot;
static void collect(uint32_t w, void *c) { (void)c; if (ngot < 512) got[ngot++] = w; }

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245 + 12345; return rnd_state >> 8; }

// Every kind of packet, random data (runs of ones for stuffing), both speeds: decode and quick PID agree
static void test_decode(void)
{
  printf("decode and quick PID\n");
  for (int speed = 0; speed < 2; speed++)
  {
    for (int k = 0; k < 400; k++)
    {
      uint8_t p[1100], d[1030];
      int n, pid;
      int kind = k % 6;
      if (kind == 0) { pid = (int[]){ SE_PID_IN, SE_PID_OUT, SE_PID_SETUP }[k % 3]; n = pkt_token(p, pid, rnd() & 0x7f, rnd() & 0xf); }
      else if (kind == 1) { pid = SE_PID_SOF; n = pkt_sof(p, rnd() & 0x7ff); }
      else if (kind == 2) { pid = (int[]){ SE_PID_ACK, SE_PID_NAK, SE_PID_STALL, SE_PID_NYET }[k % 4]; n = pkt_handshake(p, pid); }
      else
      {
        int len = (kind == 5) ? (int)(rnd() % 1024) : (int)(rnd() % 65);
        for (int i = 0; i < len; i++) d[i] = (k & 1) ? 0xff : (uint8_t)rnd(); // all ones: maximum stuffing
        pid = (int[]){ SE_PID_DATA0, SE_PID_DATA1, SE_PID_DATA2, SE_PID_MDATA }[k % 4];
        n = pkt_data(p, pid, d, len);
      }
      // capture into a private array (no ring) through the same PIO stand-in
      ngot = 0;
      pio_t pp = { .fs = speed == 1, .push = collect };
      line_packet(&pp, p, n, false);
      uint32_t marker = got[ngot - 1];
      uint32_t bits = 0xffffffff - marker;
      uint8_t dec[SE_MAX_BYTES];
      uint32_t err = 0;
      int m = se_decode(got, 511, 0, ngot - 1, bits, speed == 1, dec, SE_MAX_BYTES, &err);
      CHECK(err == 0);
      CHECK(m == n + 1 && dec[0] == (speed ? 0x80 : 0x81) && memcmp(dec + 1, p, n) == 0);
      CHECK(se_quick_pid(got[0], bits, speed == 1) == pid);
    }
  }
  // a corrupted packet: CRC error; a missing stuffed bit: stuffing error
  {
    uint8_t p[40], d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ngot = 0;
    pio_t pp = { .fs = true, .push = collect };
    int n = pkt_data(p, SE_PID_DATA0, d, 8);
    p[3] ^= 0x10;
    line_packet(&pp, p, n, false);
    uint8_t dec[SE_MAX_BYTES];
    uint32_t err = 0;
    se_decode(got, 511, 0, ngot - 1, 0xffffffff - got[ngot - 1], true, dec, SE_MAX_BYTES, &err);
    CHECK(err & SE_ERR_CRC);
    uint8_t ff[8];
    memset(ff, 0xff, 8);
    n = pkt_data(p, SE_PID_DATA1, ff, 8);
    ngot = 0;
    line_packet(&pp, p, n, true);
    err = 0;
    se_decode(got, 511, 0, ngot - 1, 0xffffffff - got[ngot - 1], true, dec, SE_MAX_BYTES, &err);
    CHECK(err & (SE_ERR_STUFF | SE_ERR_CRC | SE_ERR_NBIT));
  }
}

// An instrument's enumeration and one question/answer, inside a bus full of SOFs and IN/NAK polling
static void test_filter(void)
{
  printf("filter: SOF and NAK floods dropped, transactions kept\n");
  start(true, false, false);
  uint32_t t_setup = 0, t_answer = 0;
  int naks = 0;
  for (int f = 0; f < 100; f++)
  {
    sof();
    for (int i = 0; i < 20; i++) { tok(SE_PID_IN, 5, 1); hs(SE_PID_NAK); naks++; }
    if (f == 10)
    {
      uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
      uint8_t desc[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0x03, 0x04, 0x01, 0x60, 0, 6, 1, 2, 3, 1 };
      t_setup = tok(SE_PID_SETUP, 5, 0); dat(SE_PID_DATA0, setup, 8); hs(SE_PID_ACK);
      tok(SE_PID_IN, 5, 0); hs(SE_PID_NAK);                      // not ready yet
      tok(SE_PID_IN, 5, 0); dat(SE_PID_DATA1, desc, 18); hs(SE_PID_ACK);
      tok(SE_PID_OUT, 5, 0); dat(SE_PID_DATA1, "", 0); hs(SE_PID_ACK);
    }
    if (f == 50)
    {
      tok(SE_PID_OUT, 5, 2); dat(SE_PID_DATA0, "MEAS?\r", 6); hs(SE_PID_NAK);   // busy: dropped (resent below)
      tok(SE_PID_OUT, 5, 2); dat(SE_PID_DATA0, "MEAS?\r", 6); hs(SE_PID_ACK);
    }
    if (f == 60)
      { t_answer = tok(SE_PID_IN, 5, 1); dat(SE_PID_DATA1, "M 0.5123 V\r\n", 12); hs(SE_PID_ACK); }
    if (f % 10 == 9)
      consume(); // core 0 keeps up
  }
  consume();
  se_status(&se, g_now, SE_STATUS);
  parse(out, out_len);
  CHECK(skipped == 0);
  CHECK(nrecs > 0 && recs[0].type == SE_HELLO && recs[0].p[0] == SE_PROTOCOL && recs[0].p[1] == 1 && recs[0].p[2] == 0);
  char s[512];
  pids(s, sizeof(s));
  printf("  kept: %s\n", s);
  CHECK(strcmp(s, "SETUP DATA0 ACK IN DATA1 ACK OUT DATA1 ACK OUT DATA0 ACK IN DATA1 ACK") == 0);
  const rec_t *r = find_type(SE_PACKET, 0);
  CHECK(r && r->p[0] == 0 && r->len == 4 && (r->p[2] & 0x7f) == 5);   // SETUP 0x05/0: flags, PID, 2 bytes
  CHECK(r && r->time >= t_setup - 1 && r->time <= t_setup + 1);
  r = find_type(SE_PACKET, 10);                                         // DATA0 "MEAS?\r"
  CHECK(r && r->len == 1 + 1 + 6 + 2 && memcmp(r->p + 2, "MEAS?\r", 6) == 0);
  r = find_type(SE_PACKET, 12);
  CHECK(r && rec_pid(r) == SE_PID_IN && r->time >= t_answer - 1 && r->time <= t_answer + 1);
  const rec_t *st = find_type(SE_STATUS, 0);
  CHECK(st && st->len == SE_STATUS_SIZE);
  if (st)
  {
    printf("  status: frames %u, idle dropped %u, sent %u\n", status_u32(st, 0), status_u32(st, 6), status_u32(st, 10));
    CHECK(status_u32(st, 0) == 100);                                    // frames
    CHECK((st->p[4] | st->p[5] << 8) == ((g_frame - 1) & 0x7ff));      // last frame number
    CHECK(status_u32(st, 6) == (uint32_t)(100 + 2 * naks + 2 + 3));                // SOFs, IN+NAK pairs, the control NAK, OUT+DATA+NAK
    CHECK(status_u32(st, 10) == 15);
    CHECK(status_u32(st, 18) == 0 && status_u32(st, 22) == 0);
  }
  int idle_bytes = 100 * 4 + naks * 4; // what the idle traffic would have been on the bus (roughly)
  printf("  %d bytes of records for %d bytes of bus traffic\n", out_len, idle_bytes);
}

// Every address/endpoint and every frame number: the word-level fast path for 4-byte packets and the bit-by-bit
// decoder (tokens with a stuffed bit) must agree
static void test_all_tokens(void)
{
  printf("all 2048 tokens and frame numbers (fast path and stuffed tokens)\n");
  start(true, false, false);
  for (int a = 0; a < 128; a++)
    for (int e = 0; e < 16; e++) { tok(SE_PID_IN, a, e); hs(SE_PID_NAK); }
  for (int f = 0; f < 2048; f++) { uint8_t p[3]; pkt_sof(p, f); send(p, 3); }
  consume();
  parse(out, out_len);
  CHECK(count_type(SE_PACKET) == 0);
  CHECK(se.dropped == 2 * 2048 + 2048 && se.frames == 2048 && se.frame_no == 2047 && se.errors == 0);
  // kept tokens come out byte for byte
  out_len = 0;
  int bad = 0;
  for (int a = 0; a < 128; a++)
    for (int e = 0; e < 16; e++) { tok(SE_PID_OUT, a, e); dat(SE_PID_DATA0, "z", 1); hs(SE_PID_ACK); }
  consume();
  parse(out, out_len);
  CHECK(count_type(SE_PACKET) == 3 * 2048);
  for (int i = 0, k = 0; i < nrecs; i++)
    if (recs[i].type == SE_PACKET && rec_pid(&recs[i]) == SE_PID_OUT)
    {
      uint8_t p[3];
      pkt_token(p, SE_PID_OUT, k / 16, k % 16);
      if (recs[i].len != 4 || recs[i].p[0] != 0 || memcmp(recs[i].p + 1, p, 3) != 0) bad++;
      k++;
    }
  CHECK(bad == 0);
  // a SOF with a broken CRC is an error, not a frame
  out_len = 0;
  uint8_t p[3];
  pkt_sof(p, 77);
  p[2] ^= 0x40;
  send(p, 3);
  consume();
  parse(out, out_len);
  const rec_t *r = find_type(SE_PACKET, 0);
  CHECK(r && (r->p[0] & SE_ERR_CRC) && se.frames == 2048);
}

static void test_keep_all(void)
{
  printf("keep idle traffic: everything is sent\n");
  start(true, true, false);
  int total = 0;
  for (int f = 0; f < 10; f++)
  {
    sof(); total++;
    for (int i = 0; i < 5; i++) { tok(SE_PID_IN, 3, 1); hs(SE_PID_NAK); total += 2; }
  }
  tok(SE_PID_OUT, 3, 2); dat(SE_PID_DATA0, "abc", 3); hs(SE_PID_ACK); total += 3;
  consume();
  parse(out, out_len);
  CHECK(recs[0].type == SE_HELLO && recs[0].p[2] == 1);
  CHECK(count_type(SE_PACKET) == total);
  CHECK(rec_pid(find_type(SE_PACKET, 0)) == SE_PID_SOF);
  CHECK(rec_pid(find_type(SE_PACKET, 2)) == SE_PID_NAK);
  // switched off while running (the 'k' command)
  se.keep_all = false;
  out_len = 0;
  sof(); tok(SE_PID_IN, 3, 1); hs(SE_PID_NAK);
  consume();
  parse(out, out_len);
  CHECK(count_type(SE_PACKET) == 0);
}

static void test_reset_errors(void)
{
  printf("bus reset, packets with errors, isochronous, STALL\n");
  start(true, false, false);
  sof();
  line_reset(&pio);
  g_now += 10000;
  sof();
  uint8_t p[16], d[4] = { 9, 8, 7, 6 };
  int n = pkt_data(p, SE_PID_DATA0, d, 4);
  p[2] ^= 1;
  tok(SE_PID_OUT, 1, 1);
  send(p, n);                       // CRC error: the token goes out first, then the bad packet
  hs(SE_PID_ACK);
  tok(SE_PID_IN, 1, 3); dat(SE_PID_DATA0, d, 4);   // isochronous: no handshake; the next SOF ends it
  sof();
  tok(SE_PID_IN, 1, 0); hs(SE_PID_STALL);
  sof();
  consume();
  parse(out, out_len);
  CHECK(count_type(SE_RESET) == 1);
  char s[256];
  pids(s, sizeof(s));
  printf("  kept: %s\n", s);
  CHECK(strcmp(s, "OUT DATA0 ACK IN DATA0 IN STALL") == 0);
  const rec_t *bad = find_type(SE_PACKET, 1);
  CHECK(bad && (bad->p[0] & SE_ERR_CRC));
  CHECK(se.errors == 1);
}

// Core 0 stalls (or the link does) while the bus keeps going: the capture ring fills, core 1 counts what it drops
static void test_capture_overflow(void)
{
  printf("capture ring full: a LOST record with the exact count\n");
  start(true, false, false);
  int fed = 0;
  for (int i = 0; i < 12000; i++)
  {
    tok(SE_PID_OUT, 7, 2); dat(SE_PID_DATA0, "0123456789", 10); hs(SE_PID_ACK);
    fed += 3;
  }
  CHECK(cap.dropping);
  uint32_t lost_in_core1 = cap.lost;
  consume();                                   // core 0 catches up ...
  tok(SE_PID_OUT, 7, 2); dat(SE_PID_DATA0, "after", 5); hs(SE_PID_ACK);   // ... the next packets are stored again
  fed += 3;
  consume();
  parse(out, out_len);
  const rec_t *lost = find_type(SE_LOST, 0);
  CHECK(lost != 0);
  int kept = count_type(SE_PACKET);
  uint32_t lost_n = lost ? rd32(lost->p) : 0;
  printf("  fed %d packets, %d sent, %u lost (core 1 had counted %u before the marker)\n", fed, kept, lost_n, lost_in_core1);
  CHECK(lost && rd32(lost->p + 4) == 0);
  CHECK((int)(kept + lost_n) == fed);
  // the LOST record sits where the gap is. The packet whose end let core 1 resume was itself not stored (its words
  // had been dropped), so the last transaction lost its token: LOST, DATA0 "after", ACK
  CHECK(nrecs >= 3 && recs[nrecs - 3].type == SE_LOST);
  const rec_t *last = &recs[nrecs - 2];
  CHECK(last->type == SE_PACKET && last->len == 1 + 1 + 5 + 2 && memcmp(last->p + 2, "after", 5) == 0);
}

// The host does not read for a while: the output fills; records are counted, never half written
static void test_output_full(void)
{
  printf("output full: records dropped whole and reported\n");
  start(true, false, false);
  out_cap = out_len + 300;                     // room for a few records only
  int transactions = 0;
  for (int i = 0; i < 50; i++)
  {
    tok(SE_PID_IN, 2, 1); dat(SE_PID_DATA1, "hello world", 11); hs(SE_PID_ACK);
    transactions++;
    consume();
  }
  out_cap = sizeof(out);                       // the host reads again
  tok(SE_PID_IN, 2, 1); dat(SE_PID_DATA1, "again", 5); hs(SE_PID_ACK);
  transactions++;
  consume();
  parse(out, out_len);
  CHECK(skipped == 0);
  CHECK(count_type(SE_LOST) == 1); // reported once, when there was room again, not while the output was full
  const rec_t *lost = find_type(SE_LOST, 0);
  CHECK(lost && rd32(lost->p) == 0 && rd32(lost->p + 4) > 0);
  uint32_t lost_out = lost ? rd32(lost->p + 4) : 0;
  CHECK(recs[nrecs - 4].type == SE_LOST); // right before the transaction that got through
  printf("  %d packets sent, %u not sent (reported)\n", count_type(SE_PACKET), lost_out);
  CHECK(count_type(SE_PACKET) + (int)lost_out == transactions * 3);
  CHECK(se.lost_output == lost_out);
}

static void test_resync(void)
{
  printf("reader: resynchronises after garbage and a damaged record\n");
  start(true, false, false);
  for (int i = 0; i < 20; i++) { tok(SE_PID_OUT, 4, 2); dat(SE_PID_DATA0, "xy", 2); hs(SE_PID_ACK); }
  consume();
  parse(out, out_len);
  int good = nrecs;
  static uint8_t bad[1 << 16];
  int n = 0;
  memcpy(bad + n, "\xa5\x02\xff\x7f garbage \xa5\xa5", 15); n += 15;   // junk that starts like a record
  memcpy(bad + n, out, out_len); n += out_len;
  bad[15 + 8 + 30] ^= 0x55;                                           // damage one byte inside an early record
  parse(bad, n);
  printf("  %d of %d records read back, %d bytes skipped\n", nrecs, good, skipped);
  CHECK(nrecs == good - 1 || nrecs == good - 2);
  CHECK(recs[nrecs - 1].type == SE_PACKET && rec_pid(&recs[nrecs - 1]) == SE_PID_ACK);
}

static void test_text(void)
{
  printf("text output (for a terminal)\n");
  start(true, false, true);
  sof();
  tok(SE_PID_SETUP, 0, 0);
  uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 64, 0 };
  dat(SE_PID_DATA0, setup, 8);
  hs(SE_PID_ACK);
  tok(SE_PID_IN, 5, 1); hs(SE_PID_NAK);
  line_reset(&pio);
  consume();
  se_status(&se, g_now, SE_END);
  out[out_len] = 0;
  printf("%s", (char *)out);
  CHECK(strstr((char *)out, "Streaming started (full speed, idle traffic dropped)") != 0);
  CHECK(strstr((char *)out, " : SETUP: 0x00/0\r\n") != 0);
  CHECK(strstr((char *)out, " : DATA0 (8): 80 06 00 01 00 00 40 00 \r\n") != 0);
  CHECK(strstr((char *)out, " : ACK\r\n") != 0);
  CHECK(strstr((char *)out, "NAK") == 0);
  CHECK(strstr((char *)out, "--- RESET ---") != 0);
  CHECK(strstr((char *)out, "Streaming stopped: 1 frames (last #") != 0);
}

static void test_low_speed(void)
{
  printf("low speed: keep-alives are frames; packets decode\n");
  start(false, false, false);
  for (int f = 0; f < 30; f++)
  {
    g_now = (g_now / 1000 + 1) * 1000;
    line_keepalive(&pio);
    tok(SE_PID_IN, 2, 1); hs(SE_PID_NAK);
    if (f == 7) { tok(SE_PID_IN, 2, 1); dat(SE_PID_DATA0, "\x00\x00\x04\x00\x00\x00\x00\x00", 8); hs(SE_PID_ACK); }
  }
  consume();
  se_status(&se, g_now, SE_STATUS);
  parse(out, out_len);
  CHECK(recs[0].p[1] == 0);
  char s[128];
  pids(s, sizeof(s));
  CHECK(strcmp(s, "IN DATA0 ACK") == 0);
  const rec_t *st = find_type(SE_STATUS, 0);
  CHECK(st && status_u32(st, 0) == 30 && (st->p[4] | st->p[5] << 8) == 0xffff);
  CHECK(st && status_u32(st, 14) == 0);
}

static void test_oversize(void)
{
  printf("a packet longer than the capture keeps: flagged as truncated\n");
  start(true, false, false);
  // the longest legal packet (1023 bytes, all ones: maximum stuffing) fits ...
  uint8_t d[1200];
  memset(d, 0xff, sizeof(d));
  tok(SE_PID_OUT, 1, 3); dat(SE_PID_DATA0, d, 1023);
  // ... 1200 bytes (noise, a wrong speed setting) do not: 11200 line bits, more than CAP_MAX_WORDS * 31
  tok(SE_PID_OUT, 1, 3);
  uint8_t p[1300];
  int n = pkt_data(p, SE_PID_DATA0, d, 1200);
  send(p, n);
  d[0] = 0x55;
  memset(d, 0x55, 1000);
  tok(SE_PID_OUT, 1, 3); dat(SE_PID_DATA1, d, 1000); // fits
  sof();
  consume();
  parse(out, out_len);
  const rec_t *max = find_type(SE_PACKET, 1);
  CHECK(max && max->p[0] == 0 && max->len == 1 + 1 + 1023 + 2);
  const rec_t *big = find_type(SE_PACKET, 3);
  CHECK(big && (big->p[0] & SE_TRUNCATED));
  const rec_t *ok = find_type(SE_PACKET, 5);
  CHECK(ok && ok->p[0] == 0 && ok->len == 1 + 1 + 1000 + 2);
}

// The worst idle load: a full-speed bus polled with IN/NAK as fast as it goes. How much raw ring does a second of
// it take, and how long does the filter need per packet on this PC (the RP2040 is ~30-50x slower: see the README)?
static void test_flood_throughput(void)
{
  printf("NAK flood: 170000 IN/NAK pairs per second for 1 s\n");
  start(true, false, false);
  int pairs = 0;
  for (int f = 0; f < 1000; f++)
  {
    sof();
    for (int i = 0; i < 170; i++) { tok(SE_PID_IN, 9, 1); hs(SE_PID_NAK); pairs++; }
    consume();
  }
  parse(out, out_len);
  CHECK(count_type(SE_PACKET) == 0);
  CHECK(nrecs == 1 && se.dropped == (uint32_t)(1000 + 2 * pairs));
  printf("  %d pairs dropped, %d bytes sent (the HELLO record)\n", pairs, out_len);
}

// A sample stream for LibreTap's tests (app/tests/data/usb_stream_ftdi.bin): the FTDI instrument of LibreTap's
// usbdump.h (enumeration at 9600 baud, a question and its answer) on a busy bus, through the firmware's own filter
static int write_sample(const char *path)
{
  start(true, false, false);
  g_now = 4294000000u; // the Pico's timer wraps during the recording
  out_len = 0;
  se_start(&se, &sink, true, false, false, g_now, sizeof(ring), sizeof(out));
  uint8_t dev[18] = { 0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x03, 0x04, 0x01, 0x60, 0x00, 0x06, 0x01, 0x02, 0x03, 0x01 };
  uint8_t cfg[32] = { 0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x2d, 0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0xff,
                      0xff, 0x02, 0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00 };
  struct { int addr; uint8_t setup[8]; const uint8_t *data; int len; } ctl[] = {
    { 0, { 0x80, 6, 0, 1, 0, 0, 64, 0 }, dev, 18 },
    { 0, { 0x00, 5, 5, 0, 0, 0, 0, 0 }, 0, 0 },
    { 5, { 0x80, 6, 0, 1, 0, 0, 18, 0 }, dev, 18 },
    { 5, { 0x80, 6, 0, 2, 0, 0, 9, 0 }, cfg, 9 },
    { 5, { 0x80, 6, 0, 2, 0, 0, 32, 0 }, cfg, 32 },
    { 5, { 0x00, 9, 1, 0, 0, 0, 0, 0 }, 0, 0 },
    { 5, { 0x40, 3, 0x38, 0x41, 0, 0, 0, 0 }, 0, 0 },   // FTDI SET_BAUD_RATE 9600
    { 5, { 0x40, 4, 0x08, 0x00, 0, 0, 0, 0 }, 0, 0 },   // FTDI SET_DATA 8N1
  };
  sof();
  line_reset(&pio);
  g_now += 10000;
  for (unsigned i = 0; i < sizeof(ctl) / sizeof(ctl[0]); i++)
  {
    sof();
    bool in = ctl[i].setup[0] & 0x80;
    tok(SE_PID_SETUP, ctl[i].addr, 0); dat(SE_PID_DATA0, ctl[i].setup, 8); hs(SE_PID_ACK);
    if (ctl[i].len) { tok(SE_PID_IN, ctl[i].addr, 0); hs(SE_PID_NAK); tok(SE_PID_IN, ctl[i].addr, 0); dat(SE_PID_DATA1, ctl[i].data, ctl[i].len); hs(SE_PID_ACK); }
    tok(in ? SE_PID_OUT : SE_PID_IN, ctl[i].addr, 0); dat(SE_PID_DATA1, "", 0); hs(SE_PID_ACK);
    consume();
  }
  for (int f = 0; f < 300; f++) // 0.3 s of polling; the question at 100 ms, the answer at 250 ms
  {
    sof();
    for (int i = 0; i < 30; i++) { tok(SE_PID_IN, 5, 1); hs(SE_PID_NAK); }
    if (f % 16 == 0) { tok(SE_PID_IN, 5, 1); dat(f % 32 ? SE_PID_DATA1 : SE_PID_DATA0, "\x01\x60", 2); hs(SE_PID_ACK); } // FTDI status
    if (f == 100) { tok(SE_PID_OUT, 5, 2); dat(SE_PID_DATA0, "MEAS?\r", 6); hs(SE_PID_ACK); }
    if (f == 250) { tok(SE_PID_IN, 5, 1); dat(SE_PID_DATA0, "\x01\x60" "M 0.5123 V\r\n", 14); hs(SE_PID_ACK); }
    consume();
  }
  se_status(&se, g_now, SE_END);
  FILE *f = fopen(path, "wb");
  if (!f) return 1;
  fwrite(out, 1, out_len, f);
  fclose(f);
  printf("%s: %d bytes\n", path, out_len);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc == 3 && strcmp(argv[1], "--sample") == 0)
    return write_sample(argv[2]);
  CHECK(se_crc16((const uint8_t *)"123456789", 9, 0xffff) == 0x29b1); // CRC-16/CCITT-FALSE check value
  test_decode();
  test_filter();
  test_all_tokens();
  test_keep_all();
  test_reset_errors();
  test_capture_overflow();
  test_output_full();
  test_resync();
  test_text();
  test_low_speed();
  test_oversize();
  test_flood_throughput();
  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
