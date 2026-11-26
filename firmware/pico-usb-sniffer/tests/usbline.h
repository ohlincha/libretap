// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, LibreTap contributors.
//
// A stand-in for the USB bus and the capture PIO, for the host tests: builds USB packets (with their CRCs), turns
// them into line levels (bit stuffing, NRZI) and hands the words the PIO program would push (31 samples per word,
// the rest at the end of the packet, then the inverted bit counter) to a callback, as core 1 receives them.
#ifndef USBLINE_H
#define USBLINE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef void (*line_push_fn)(uint32_t word, void *ctx);

// USB CRC-5 of a token's 11 bits (address, endpoint or frame number), reflected form
static inline uint32_t usb_crc5(uint32_t v11)
{
  uint32_t crc = 0x1f;
  for (int i = 0; i < 11; i++)
  {
    uint32_t bit = (v11 >> i) & 1;
    crc = ((crc ^ bit) & 1) ? (crc >> 1) ^ 0x14 : crc >> 1;
  }
  return crc ^ 0x1f;
}

static inline uint16_t usb_crc16(const uint8_t *d, int n)
{
  uint16_t crc = 0xffff;
  for (int i = 0; i < n; i++)
  {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc ^ 0xffff;
}

static inline uint8_t pid_byte(int pid) { return (uint8_t)((pid & 0xf) | ((~pid & 0xf) << 4)); }

// Packet bytes after SYNC; returns the length
static inline int pkt_token(uint8_t *p, int pid, int addr, int ep)
{
  uint32_t v = (addr & 0x7f) | ((ep & 0xf) << 7);
  v |= usb_crc5(v) << 11;
  p[0] = pid_byte(pid);
  p[1] = v & 0xff;
  p[2] = v >> 8;
  return 3;
}

static inline int pkt_sof(uint8_t *p, int frame)
{
  uint32_t v = frame & 0x7ff;
  v |= usb_crc5(v) << 11;
  p[0] = pid_byte(5);
  p[1] = v & 0xff;
  p[2] = v >> 8;
  return 3;
}

static inline int pkt_data(uint8_t *p, int pid, const uint8_t *d, int n)
{
  uint16_t crc = usb_crc16(d, n);
  p[0] = pid_byte(pid);
  memcpy(p + 1, d, n);
  p[1 + n] = crc & 0xff;
  p[2 + n] = crc >> 8;
  return n + 3;
}

static inline int pkt_handshake(uint8_t *p, int pid)
{
  p[0] = pid_byte(pid);
  return 1;
}

typedef struct
{
  bool fs;
  line_push_fn push;
  void *ctx;
  uint32_t isr;
  int isr_count;
  uint32_t counter; // the PIO's X: starts at ~0, decremented per sample
} pio_t;

static inline void pio_sample(pio_t *p, int level)
{
  p->counter--;
  p->isr = (p->isr << 1) | (level & 1); // IN shifts left
  if (++p->isr_count == 31)             // autopush at 31 bits
  {
    p->push(p->isr, p->ctx);
    p->isr = 0;
    p->isr_count = 0;
  }
}

static inline void pio_eop(pio_t *p)
{
  p->push(p->isr, p->ctx);     // push (the rest; an empty ISR pushes 0)
  p->push(p->counter, p->ctx); // mov isr, x; push
  p->isr = 0;
  p->isr_count = 0;
  p->counter = 0xffffffff;
}

// The words of one packet (bytes after SYNC); returns the number of bits on the line (stuffing included), so the
// caller can advance its clock. `stuff_error`: leave out one stuffed bit (a corrupted packet).
static inline int line_packet(pio_t *p, const uint8_t *bytes, int n, bool stuff_error)
{
  int idle = p->fs ? 1 : 0; // D+ level of J
  int level = idle;
  int ones = 0, bits = 0;
  uint8_t all[2048];

  all[0] = 0x80;
  memcpy(all + 1, bytes, n);
  p->counter = 0xffffffff;
  p->isr = 0;
  p->isr_count = 0;

  for (int i = 0; i < n + 1; i++)
  {
    for (int b = 0; b < 8; b++)
    {
      int bit = (all[i] >> b) & 1;
      if (!bit)
        level ^= 1; // NRZI: a zero is a change
      pio_sample(p, level);
      bits++;
      ones = bit ? ones + 1 : 0;
      if (ones == 6)
      {
        if (!stuff_error)
        {
          level ^= 1; // the stuffed zero
          pio_sample(p, level);
          bits++;
        }
        ones = 0;
      }
    }
  }
  pio_sample(p, 0); // SE0: both lines low, D+ sampled as 0
  pio_eop(p);
  return bits + 2;
}

// A bus reset: the PIO first sees a one-sample packet, then 32 samples of SE0
static inline void line_reset(pio_t *p)
{
  p->counter = 0xffffffff;
  p->isr = 0;
  p->isr_count = 0;
  pio_sample(p, 0);
  pio_eop(p);
  p->push(0xffffffff, p->ctx);
}

// A low-speed keep-alive: an end of packet alone
static inline void line_keepalive(pio_t *p)
{
  p->counter = 0xffffffff;
  p->isr = 0;
  p->isr_count = 0;
  pio_sample(p, 0);
  pio_eop(p);
}

#endif
