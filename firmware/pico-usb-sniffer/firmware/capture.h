// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Changed for LibreTap's USB Sniffer Stream (2026): the capture ring replaces the buffer (capture_ring.h).

#ifndef _CAPTURE_H_
#define _CAPTURE_H_

/*- Includes ----------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "capture_ring.h"

/*- Variables ---------------------------------------------------------------*/
extern uint32_t g_ring[CAP_RING_WORDS];
extern cap_t g_cap;

/*- Prototypes --------------------------------------------------------------*/
void capture_init(void);
void capture_command(int cmd);   // 's' start (core 1 answers 'S'), 'p' stop (answers 'P')
int capture_reply(void);

#endif // _CAPTURE_H_
