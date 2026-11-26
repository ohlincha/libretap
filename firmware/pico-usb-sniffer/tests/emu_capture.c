// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, LibreTap contributors.
//
// Stands in for capture.c in the emulator build (make -C tests emu): rp2040js emulates core 0 only, without the
// inter-core FIFO or a USB bus to sniff. So core 0 plays core 1 here: 's' clears the ring and answers 'S', and each
// time main.c asks for a reply, the next few PIO words of canned traffic (emu_traffic.h) go into the ring through
// cap_word(), the capture loop's own body. Everything else (USB, commands, ring consumer, filter, encoder, output
// ring) is the real firmware.
#include <stdint.h>
#include <stdbool.h>
#include "rp2040.h"
#include "capture.h"
#include "globals.h"
#include "emu_traffic.h"

#define EMU_LOOPS        20
#define WORDS_PER_CALL   48

uint32_t g_ring[CAP_RING_WORDS];
cap_t g_cap = { .ring = g_ring };
int g_capture_speed = CaptureSpeed_Full;

static int reply = 0;
static bool feeding = false;
static uint32_t pos = 0, loops = 0;

void capture_init(void)
{
}

void capture_command(int cmd)
{
  if (cmd == 's')
  {
    cap_reset(&g_cap);
    feeding = true;
    pos = 0;
    loops = 0;
    reply = 'S';
  }
  else if (cmd == 'p')
  {
    feeding = false;
    reply = 'P';
  }
}

int capture_reply(void)
{
  int r = reply;

  reply = 0;
  for (int i = 0; feeding && i < WORDS_PER_CALL; i++)
  {
    cap_word(&g_cap, emu_words[pos]);
    if (++pos == EMU_WORDS)
    {
      pos = 0;
      if (++loops == EMU_LOOPS)
        feeding = false;
    }
  }
  return r;
}
