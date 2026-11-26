// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Changed for LibreTap's USB Sniffer Stream (2026): only the capture speed is left of the settings; the PIDs moved
// to stream.h, the buffer to capture_ring.h.

#ifndef _GLOBALS_H_
#define _GLOBALS_H_

/*- Includes ----------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>

/*- Definitions -------------------------------------------------------------*/
enum
{
  CaptureSpeed_Low,
  CaptureSpeed_Full,
  CaptureSpeedCount,
};

/*- Variables ---------------------------------------------------------------*/
extern int g_capture_speed;

/*- Prototypes --------------------------------------------------------------*/
void set_error(bool error);

#endif // _GLOBALS_H_
