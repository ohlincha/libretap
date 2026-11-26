// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Copyright (c) 2026, LibreTap contributors. Streaming additions (new file, not in usb-sniffer-lite).
//
// The capture ring between the two cores. Core 1 (capture.c) puts the PIO's words in, exactly as usb-sniffer-lite
// put them in its linear buffer: per packet a header word, the end time, then the raw bit words. Core 0 (main.c,
// stream.c) takes packets out, decodes and filters them. Both indices only grow (they wrap at 2^32); the ring index
// is `index & CAP_MASK`.
//
//   header: bits 0..16 the bit count (end-of-packet sample included; 0 = bus reset, 1 = end of packet alone),
//           bits 17..30 the number of data words that follow the end time. CAP_LOST_MARK instead: the capture had
//           no room for a while, the next word is the number of packets it could not store.
//
// When the ring is nearly full (core 0 or the USB link cannot keep up), core 1 stops storing and counts the packets
// it drops; once there is room again it stores a LOST marker first. Nothing is lost silently.
//
// cap_word() is the capture loop's body; it is in a header so that the unit tests run the same code.
#ifndef _CAPTURE_RING_H_
#define _CAPTURE_RING_H_

#include <stdint.h>
#include <stdbool.h>

#ifndef CAP_RING_WORDS
#define CAP_RING_WORDS   32768          // 128 kB (a power of two)
#endif
#define CAP_MASK         (CAP_RING_WORDS - 1)
#ifndef CAP_MAX_WORDS
#define CAP_MAX_WORDS    320            // longest packet stored (1023-byte isochronous + stuffing: 310 words)
#endif
#define CAP_NEED         (CAP_MAX_WORDS + 4)
#define CAP_LOST_MARK    0x80000000u

#ifndef CAP_DMB
#define CAP_DMB()        __asm volatile ("dmb" ::: "memory")
#endif

#ifndef CAP_NOW
#define CAP_NOW()        (TIMER->TIMERAWL)
#endif

typedef struct
{
  uint32_t          *ring;
  volatile uint32_t wr;      // published by core 1: everything before it is complete
  volatile uint32_t rd;      // published by core 0: everything before it has been read
  volatile uint32_t lost;    // packets dropped since the last LOST marker
  uint32_t          packet;  // where the current packet's header goes
  uint32_t          index;   // where its next data word goes
  bool              dropping;
} cap_t;

//-----------------------------------------------------------------------------
static inline void cap_reset(cap_t *c)
{
  c->rd = 0;
  c->wr = 0;
  c->lost = 0;
  c->packet = 0;
  c->index = 2;
  c->dropping = false;
}

//-----------------------------------------------------------------------------
static inline __attribute__((always_inline)) void cap_word(cap_t *c, uint32_t v)
{
  if (v & 0x80000000)
  {
    // the end of a packet: the PIO pushes the inverted bit counter
    if (c->dropping)
    {
      c->lost++;

      if (CAP_RING_WORDS - (c->packet - c->rd) >= CAP_NEED + 2)
      {
        c->ring[c->packet & CAP_MASK] = CAP_LOST_MARK;
        c->ring[(c->packet + 1) & CAP_MASK] = c->lost;
        c->lost = 0;
        c->dropping = false;
        c->packet += 2;
        c->index = c->packet + 2;
        CAP_DMB();
        c->wr = c->packet;
      }
      return;
    }

    uint32_t size = 0xffffffff - v;
    uint32_t words = c->index - c->packet - 2;

    c->ring[c->packet & CAP_MASK] = (words << 17) | ((size > 0x1ffff) ? 0x1ffff : size);
    c->ring[(c->packet + 1) & CAP_MASK] = CAP_NOW();
    c->packet = c->index;
    c->index += 2;
    CAP_DMB();
    c->wr = c->packet;

    if (CAP_RING_WORDS - (c->packet - c->rd) < CAP_NEED)
      c->dropping = true;
  }
  else if (!c->dropping && (c->index - c->packet) < (CAP_MAX_WORDS + 2))
  {
    c->ring[c->index++ & CAP_MASK] = v;
  }
}

#endif // _CAPTURE_RING_H_
