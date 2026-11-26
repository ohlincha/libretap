// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Copyright (c) 2026, LibreTap contributors. Streaming additions (new file, not in usb-sniffer-lite; se_decode and
// the CRC tables come from usb-sniffer-lite's capture.c, the text lines follow its display.c).
//
// See stream.h. No hardware access: compiles for the RP2040 and on a PC (tests/).

/*- Includes ----------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "stream.h"
#ifndef CAP_NOW
#define CAP_NOW()  0u   // only the ring's layout is used here; the capture loop is in capture.c
#define CAP_DMB()
#endif
#include "capture_ring.h"

/*- Constants ---------------------------------------------------------------*/
static const uint16_t crc16_usb_tab[256] =
{
  0x0000, 0xc0c1, 0xc181, 0x0140, 0xc301, 0x03c0, 0x0280, 0xc241,
  0xc601, 0x06c0, 0x0780, 0xc741, 0x0500, 0xc5c1, 0xc481, 0x0440,
  0xcc01, 0x0cc0, 0x0d80, 0xcd41, 0x0f00, 0xcfc1, 0xce81, 0x0e40,
  0x0a00, 0xcac1, 0xcb81, 0x0b40, 0xc901, 0x09c0, 0x0880, 0xc841,
  0xd801, 0x18c0, 0x1980, 0xd941, 0x1b00, 0xdbc1, 0xda81, 0x1a40,
  0x1e00, 0xdec1, 0xdf81, 0x1f40, 0xdd01, 0x1dc0, 0x1c80, 0xdc41,
  0x1400, 0xd4c1, 0xd581, 0x1540, 0xd701, 0x17c0, 0x1680, 0xd641,
  0xd201, 0x12c0, 0x1380, 0xd341, 0x1100, 0xd1c1, 0xd081, 0x1040,
  0xf001, 0x30c0, 0x3180, 0xf141, 0x3300, 0xf3c1, 0xf281, 0x3240,
  0x3600, 0xf6c1, 0xf781, 0x3740, 0xf501, 0x35c0, 0x3480, 0xf441,
  0x3c00, 0xfcc1, 0xfd81, 0x3d40, 0xff01, 0x3fc0, 0x3e80, 0xfe41,
  0xfa01, 0x3ac0, 0x3b80, 0xfb41, 0x3900, 0xf9c1, 0xf881, 0x3840,
  0x2800, 0xe8c1, 0xe981, 0x2940, 0xeb01, 0x2bc0, 0x2a80, 0xea41,
  0xee01, 0x2ec0, 0x2f80, 0xef41, 0x2d00, 0xedc1, 0xec81, 0x2c40,
  0xe401, 0x24c0, 0x2580, 0xe541, 0x2700, 0xe7c1, 0xe681, 0x2640,
  0x2200, 0xe2c1, 0xe381, 0x2340, 0xe101, 0x21c0, 0x2080, 0xe041,
  0xa001, 0x60c0, 0x6180, 0xa141, 0x6300, 0xa3c1, 0xa281, 0x6240,
  0x6600, 0xa6c1, 0xa781, 0x6740, 0xa501, 0x65c0, 0x6480, 0xa441,
  0x6c00, 0xacc1, 0xad81, 0x6d40, 0xaf01, 0x6fc0, 0x6e80, 0xae41,
  0xaa01, 0x6ac0, 0x6b80, 0xab41, 0x6900, 0xa9c1, 0xa881, 0x6840,
  0x7800, 0xb8c1, 0xb981, 0x7940, 0xbb01, 0x7bc0, 0x7a80, 0xba41,
  0xbe01, 0x7ec0, 0x7f80, 0xbf41, 0x7d00, 0xbdc1, 0xbc81, 0x7c40,
  0xb401, 0x74c0, 0x7580, 0xb541, 0x7700, 0xb7c1, 0xb681, 0x7640,
  0x7200, 0xb2c1, 0xb381, 0x7340, 0xb101, 0x71c0, 0x7080, 0xb041,
  0x5000, 0x90c1, 0x9181, 0x5140, 0x9301, 0x53c0, 0x5280, 0x9241,
  0x9601, 0x56c0, 0x5780, 0x9741, 0x5500, 0x95c1, 0x9481, 0x5440,
  0x9c01, 0x5cc0, 0x5d80, 0x9d41, 0x5f00, 0x9fc1, 0x9e81, 0x5e40,
  0x5a00, 0x9ac1, 0x9b81, 0x5b40, 0x9901, 0x59c0, 0x5880, 0x9841,
  0x8801, 0x48c0, 0x4980, 0x8941, 0x4b00, 0x8bc1, 0x8a81, 0x4a40,
  0x4e00, 0x8ec1, 0x8f81, 0x4f40, 0x8d01, 0x4dc0, 0x4c80, 0x8c41,
  0x4400, 0x84c1, 0x8581, 0x4540, 0x8701, 0x47c0, 0x4680, 0x8641,
  0x8201, 0x42c0, 0x4380, 0x8341, 0x4100, 0x81c1, 0x8081, 0x4040,
};

static const uint8_t crc5_usb_tab[256] =
{
  0x00, 0x0e, 0x1c, 0x12, 0x11, 0x1f, 0x0d, 0x03, 0x0b, 0x05, 0x17, 0x19, 0x1a, 0x14, 0x06, 0x08,
  0x16, 0x18, 0x0a, 0x04, 0x07, 0x09, 0x1b, 0x15, 0x1d, 0x13, 0x01, 0x0f, 0x0c, 0x02, 0x10, 0x1e,
  0x05, 0x0b, 0x19, 0x17, 0x14, 0x1a, 0x08, 0x06, 0x0e, 0x00, 0x12, 0x1c, 0x1f, 0x11, 0x03, 0x0d,
  0x13, 0x1d, 0x0f, 0x01, 0x02, 0x0c, 0x1e, 0x10, 0x18, 0x16, 0x04, 0x0a, 0x09, 0x07, 0x15, 0x1b,
  0x0a, 0x04, 0x16, 0x18, 0x1b, 0x15, 0x07, 0x09, 0x01, 0x0f, 0x1d, 0x13, 0x10, 0x1e, 0x0c, 0x02,
  0x1c, 0x12, 0x00, 0x0e, 0x0d, 0x03, 0x11, 0x1f, 0x17, 0x19, 0x0b, 0x05, 0x06, 0x08, 0x1a, 0x14,
  0x0f, 0x01, 0x13, 0x1d, 0x1e, 0x10, 0x02, 0x0c, 0x04, 0x0a, 0x18, 0x16, 0x15, 0x1b, 0x09, 0x07,
  0x19, 0x17, 0x05, 0x0b, 0x08, 0x06, 0x14, 0x1a, 0x12, 0x1c, 0x0e, 0x00, 0x03, 0x0d, 0x1f, 0x11,
  0x14, 0x1a, 0x08, 0x06, 0x05, 0x0b, 0x19, 0x17, 0x1f, 0x11, 0x03, 0x0d, 0x0e, 0x00, 0x12, 0x1c,
  0x02, 0x0c, 0x1e, 0x10, 0x13, 0x1d, 0x0f, 0x01, 0x09, 0x07, 0x15, 0x1b, 0x18, 0x16, 0x04, 0x0a,
  0x11, 0x1f, 0x0d, 0x03, 0x00, 0x0e, 0x1c, 0x12, 0x1a, 0x14, 0x06, 0x08, 0x0b, 0x05, 0x17, 0x19,
  0x07, 0x09, 0x1b, 0x15, 0x16, 0x18, 0x0a, 0x04, 0x0c, 0x02, 0x10, 0x1e, 0x1d, 0x13, 0x01, 0x0f,
  0x1e, 0x10, 0x02, 0x0c, 0x0f, 0x01, 0x13, 0x1d, 0x15, 0x1b, 0x09, 0x07, 0x04, 0x0a, 0x18, 0x16,
  0x08, 0x06, 0x14, 0x1a, 0x19, 0x17, 0x05, 0x0b, 0x03, 0x0d, 0x1f, 0x11, 0x12, 0x1c, 0x0e, 0x00,
  0x1b, 0x15, 0x07, 0x09, 0x0a, 0x04, 0x16, 0x18, 0x10, 0x1e, 0x0c, 0x02, 0x01, 0x0f, 0x1d, 0x13,
  0x0d, 0x03, 0x11, 0x1f, 0x1c, 0x12, 0x00, 0x0e, 0x06, 0x08, 0x1a, 0x14, 0x17, 0x19, 0x0b, 0x05,
};

/*- Implementations ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
static uint16_t crc16_usb(const uint8_t *data, int size)
{
  uint16_t crc = 0xffff;

  for (int i = 0; i < size; i++)
    crc = crc16_usb_tab[(crc ^ data[i]) & 0xff] ^ (crc >> 8);

  return crc;
}

//-----------------------------------------------------------------------------
static uint8_t crc5_usb(const uint8_t *data, int size)
{
  uint8_t crc = 0xff;

  for (int i = 0; i < size; i++)
    crc = crc5_usb_tab[(crc ^ data[i]) & 0xff] ^ (crc >> 8);

  return crc;
}

//-----------------------------------------------------------------------------
// CRC-16/CCITT-FALSE for the records, a nibble table at a time (small and fast enough for the RP2040)
uint16_t se_crc16(const uint8_t *data, int size, uint16_t crc)
{
  static const uint16_t tab[16] = { 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
                                    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef };

  for (int i = 0; i < size; i++)
  {
    crc = (uint16_t)((crc << 4) ^ tab[((crc >> 12) ^ (data[i] >> 4)) & 0xf]);
    crc = (uint16_t)((crc << 4) ^ tab[((crc >> 12) ^ data[i]) & 0xf]);
  }
  return crc;
}

//-----------------------------------------------------------------------------
// usb-sniffer-lite's process_packet, reading from the capture ring instead of the linear buffer
int se_decode(const uint32_t *ring, uint32_t mask, uint32_t start, uint32_t words, uint32_t bits, bool fs,
              uint8_t *out_data, int max, uint32_t *err)
{
  uint32_t v = 0x80000000;
  uint32_t error = 0;
  int out_size = 0;
  int out_bit = 0;
  int out_byte = 0;
  int stuff_count = 0;
  int size = (bits > 0) ? (int)bits - 1 : 0; // without the end-of-packet sample
  int pid, npid;

  while (size)
  {
    if (words == 0)
    {
      error |= SE_TRUNCATED | SE_ERR_SIZE; // the capture kept only CAP_MAX_WORDS of it
      break;
    }

    uint32_t w = ring[start++ & mask];
    int bit_count;

    words--;

    if (size < 31)
    {
      w <<= (30-size);
      bit_count = size;
    }
    else
    {
      bit_count = 31;
    }

    v ^= (w ^ (w << 1));

    for (int i = 0; i < bit_count; i++)
    {
      int bit = (v & 0x80000000) ? 0 : 1;

      v <<= 1;

      if (stuff_count == 6)
      {
        if (bit)
          error |= SE_ERR_STUFF;

        stuff_count = 0;
        continue;
      }
      else if (bit)
        stuff_count++;
      else
        stuff_count = 0;

      out_byte |= (bit << out_bit);
      out_bit++;

      if (out_bit == 8)
      {
        if (out_size < max)
          out_data[out_size++] = out_byte;
        else
          error |= SE_TRUNCATED;
        out_byte = 0;
        out_bit = 0;
      }
    }

    size -= bit_count;
  }

  if (out_bit)
    error |= SE_ERR_NBIT;

  if (out_size < 1)
  {
    *err = error | SE_ERR_SIZE;
    return out_size;
  }

  if (out_data[0] != (fs ? 0x80 : 0x81))
    error |= SE_ERR_SYNC;

  if (out_size < 2)
  {
    *err = error | SE_ERR_SIZE;
    return out_size;
  }

  pid = out_data[1] & 0x0f;
  npid = (~out_data[1] >> 4) & 0x0f;

  if ((pid != npid) || (pid == SE_PID_RESERVED))
    error |= SE_ERR_PID;

  if (pid == SE_PID_SOF || pid == SE_PID_IN || pid == SE_PID_OUT || pid == SE_PID_SETUP || pid == SE_PID_PING ||
      pid == SE_PID_SPLIT)
  {
    if (((pid == SE_PID_SPLIT) && (out_size != 5)) || ((pid != SE_PID_SPLIT) && (out_size != 4)))
      error |= SE_ERR_SIZE;
    else if (crc5_usb(&out_data[2], out_size-2) != 0x09)
      error |= SE_ERR_CRC;
  }
  else if (pid == SE_PID_DATA0 || pid == SE_PID_DATA1 || pid == SE_PID_DATA2 || pid == SE_PID_MDATA)
  {
    if (out_size < 4)
      error |= SE_ERR_SIZE;
    else if (!(error & SE_TRUNCATED) && crc16_usb(&out_data[2], out_size-2) != 0xb001)
      error |= SE_ERR_CRC;
  }

  *err = error;
  return out_size;
}

//-----------------------------------------------------------------------------
#define INLINE static inline __attribute__((always_inline))

// bit order reversed (the line carries bytes LSB first)
static const uint8_t rev8_tab[256] =
{
#define R2(n) n, n + 2*64, n + 1*64, n + 3*64
#define R4(n) R2(n), R2(n + 2*16), R2(n + 1*16), R2(n + 3*16)
#define R6(n) R4(n), R4(n + 2*4), R4(n + 1*4), R4(n + 3*4)
  R6(0), R6(2), R6(1), R6(3)
#undef R2
#undef R4
#undef R6
};

INLINE uint32_t rev8(uint32_t b)
{
  return rev8_tab[b & 0xff];
}

//-----------------------------------------------------------------------------
int se_quick_pid(uint32_t w, uint32_t bits, bool fs)
{
  int size = (bits > 0) ? (int)bits - 1 : 0;

  if (size < 16)
    return -1;

  if (size < 31)
    w <<= (30-size); // the first sample at bit 30, as in se_decode

  // NRZI: bit i (sample at bit 30-i) is 1 when the line did not change; the line was idle (J) before the packet
  uint32_t t = ~(w ^ ((w >> 1) | 0x40000000));
  uint32_t sync = rev8((t >> 23) & 0xff);
  uint32_t pid = rev8((t >> 15) & 0xff);

  if (sync != (fs ? 0x80u : 0x81u))
    return -1;

  if ((pid & 0x0f) != ((~pid >> 4) & 0x0f))
    return -1;

  return pid & 0x0f;
}

/*- Output ------------------------------------------------------------------*/

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

#define LOST_RECORD_SIZE (SE_RECORD_EXTRA + 8)

//-----------------------------------------------------------------------------
static bool write_record(se_t *se, int type, uint32_t time, const uint8_t *p1, int n1, const uint8_t *p2, int n2)
{
  uint8_t h[SE_HEADER_SIZE];
  uint8_t c[2];
  uint16_t crc;

  h[0] = SE_SYNC;
  h[1] = type;
  put16(&h[2], n1 + n2);
  put32(&h[4], time);
  crc = se_crc16(&h[1], SE_HEADER_SIZE-1, 0xffff);
  crc = se_crc16(p1, n1, crc);
  crc = se_crc16(p2, n2, crc);
  put16(c, crc);

  se->out.write(se->out.ctx, h, SE_HEADER_SIZE);
  if (n1)
    se->out.write(se->out.ctx, p1, n1);
  if (n2)
    se->out.write(se->out.ctx, p2, n2);
  se->out.write(se->out.ctx, c, 2);
  return true;
}

//-----------------------------------------------------------------------------
// Text mode: a small line builder (no printf on the RP2040)
typedef struct
{
  char buf[3 * SE_MAX_BYTES + 160];
  int  len;
} line_t;

static void l_str(line_t *l, const char *s)
{
  while (*s && l->len < (int)sizeof(l->buf) - 1)
    l->buf[l->len++] = *s++;
}

static void l_dec(line_t *l, uint32_t v, int width)
{
  char tmp[12];
  int n = 0;

  do
  {
    tmp[n++] = '0' + (v % 10);
    v /= 10;
  } while (v);

  while (width-- > n)
    l_str(l, " ");

  while (n)
  {
    char s[2] = { tmp[--n], 0 };
    l_str(l, s);
  }
}

static void l_hex(line_t *l, uint32_t v, int digits)
{
  char s[2] = { 0, 0 };

  while (digits--)
  {
    s[0] = "0123456789abcdef"[(v >> (digits * 4)) & 0xf];
    l_str(l, s);
  }
}

static void l_time(se_t *se, line_t *l, uint32_t time)
{
  l->len = 0;
  l_dec(l, time - se->start_time, 10);
  l_str(l, " : ");
}

static bool l_emit(se_t *se, line_t *l)
{
  l_str(l, "\r\n");
  se->out.write(se->out.ctx, (const uint8_t *)l->buf, l->len);
  return true;
}

static line_t g_line;

static const char *pid_name(int pid)
{
  static const char *names[16] = { "RESERVED", "OUT", "ACK", "DATA0", "PING", "SOF", "NYET", "DATA2",
                                   "SPLIT", "IN", "NAK", "DATA1", "PRE/ERR", "SETUP", "STALL", "MDATA" };
  return names[pid & 0xf];
}

// usb-sniffer-lite's display.c lines (time in us since the start of streaming)
static int text_packet(se_t *se, line_t *l, uint32_t time, uint32_t err, const uint8_t *b, int n)
{
  int pid = (n > 0) ? (b[0] & 0x0f) : 0;

  l_time(se, l, time);

  if (err & (SE_ERR_MASK | SE_TRUNCATED))
  {
    static const char *names[] = { "STUFF", "CRC", "PID", "SYNC", "NBIT", "SIZE" };
    bool first = true;

    l_str(l, "ERROR [");
    for (int i = 0; i < 6; i++)
      if (err & (1 << i))
      {
        if (!first)
          l_str(l, ", ");
        l_str(l, names[i]);
        first = false;
      }
    if (err & SE_TRUNCATED)
      l_str(l, first ? "TRUNCATED" : ", TRUNCATED");
    l_str(l, "]: ");
    if (n > 0)
    {
      l_str(l, "PID = 0x");
      l_hex(l, b[0], 2);
      l_str(l, ", ");
    }
    if (n > 1)
    {
      l_str(l, "DATA: ");
      for (int i = 1; i < n && i <= 16; i++)
      {
        l_hex(l, b[i], 2);
        l_str(l, " ");
      }
      if (n > 17)
        l_str(l, "...");
    }
    return l->len;
  }

  if (pid == SE_PID_SOF && n >= 3)
  {
    l_str(l, "SOF #");
    l_dec(l, ((b[2] << 8) | b[1]) & 0x7ff, 0);
  }
  else if ((pid == SE_PID_IN || pid == SE_PID_OUT || pid == SE_PID_SETUP) && n >= 3)
  {
    int v = (b[2] << 8) | b[1];

    l_str(l, pid_name(pid));
    l_str(l, ": 0x");
    l_hex(l, v & 0x7f, 2);
    l_str(l, "/");
    l_hex(l, (v >> 7) & 0xf, 1);
  }
  else if (pid == SE_PID_DATA0 || pid == SE_PID_DATA1 || pid == SE_PID_DATA2 || pid == SE_PID_MDATA)
  {
    l_str(l, pid_name(pid));
    if (n <= 3)
    {
      l_str(l, ": ZLP");
    }
    else
    {
      l_str(l, " (");
      l_dec(l, n - 3, 0);
      l_str(l, "): ");
      for (int i = 1; i < n - 2; i++)
      {
        l_hex(l, b[i], 2);
        l_str(l, " ");
      }
    }
  }
  else
  {
    l_str(l, pid_name(pid));
  }

  return l->len;
}

//-----------------------------------------------------------------------------
// Room for this many bytes and, unless it is the LOST record itself, a later LOST record?
static bool room(se_t *se, int size, bool lost_record)
{
  return se->out.space(se->out.ctx) >= size + (lost_record ? 0 : LOST_RECORD_SIZE);
}

static void lost_report(se_t *se)
{
  if (0 == se->pend_lost_capture && 0 == se->pend_lost_output)
    return;

  if (se->text)
  {
    line_t *l = &g_line;
    l_time(se, l, se->last_time);
    l_str(l, "LOST: ");
    l_dec(l, se->pend_lost_capture, 0);
    l_str(l, " packets not captured (buffer full), ");
    l_dec(l, se->pend_lost_output, 0);
    l_str(l, " records not sent (link too slow)");
    if (!room(se, l->len + 2, true))
      return;
    l_emit(se, l);
  }
  else
  {
    uint8_t p[8];

    if (!room(se, LOST_RECORD_SIZE, true))
      return;
    put32(&p[0], se->pend_lost_capture);
    put32(&p[4], se->pend_lost_output);
    write_record(se, SE_LOST, se->last_time, p, 8, 0, 0);
  }

  se->pend_lost_capture = 0;
  se->pend_lost_output = 0;
}

// Room for a record of `size` bytes, after the LOST record that has to come first (if any), keeping room for a
// later LOST record? A LOST record goes out only together with the record after it, not while the output is full.
static bool make_room(se_t *se, int size)
{
  if (se->pend_lost_capture || se->pend_lost_output)
  {
    if (!room(se, LOST_RECORD_SIZE + 160 + size, false)) // 160: a text LOST line is longer than the record
      return false;
    lost_report(se);
  }
  return room(se, size, false);
}

// A record (binary) or line (text); `count` packets are lost if there is no room for it.
static void emit(se_t *se, int type, uint32_t time, const uint8_t *p1, int n1, const uint8_t *p2, int n2, int count)
{

  if (se->text)
  {
    line_t *l = &g_line;

    if (type == SE_PACKET)
    {
      text_packet(se, l, time, p1[0], p2, n2);
    }
    else
    {
      l_time(se, l, time);
      if (type == SE_RESET)
        l_str(l, "--- RESET ---");
      else if (type == SE_KEEPALIVE)
        l_str(l, "LS SOF");
      else
      {
        for (int i = 0; i < n1 && l->len < (int)sizeof(l->buf) - 3; i++)
          l->buf[l->len++] = p1[i];
      }
    }

    if (!make_room(se, l->len + 2))
    {
      se->pend_lost_output += count;
      se->lost_output += count;
      return;
    }
    l_emit(se, l);
  }
  else
  {
    if (!make_room(se, SE_RECORD_EXTRA + n1 + n2))
    {
      se->pend_lost_output += count;
      se->lost_output += count;
      return;
    }
    write_record(se, type, time, p1, n1, p2, n2);
  }

  if (type == SE_PACKET)
    se->sent += count;
}

static void emit_packet(se_t *se, uint32_t time, uint32_t err, const uint8_t *bytes, int size)
{
  uint8_t flags = err;

  // bytes[0] is SYNC: the record starts at the PID
  emit(se, SE_PACKET, time, &flags, 1, bytes + 1, size > 0 ? size - 1 : 0, 1);
}

/*- The filter --------------------------------------------------------------*/

static uint32_t packet_start(se_t *se, uint32_t end_time, uint32_t bits)
{
  if (se->fs)
    return end_time - ((bits * 5461) >> 16); // Divide by 12
  else
    return end_time - ((bits * 43691) >> 16); // Divide by 1.5
}

//-----------------------------------------------------------------------------
void se_start(se_t *se, const se_out_t *out, bool fs, bool keep_all, bool text, uint32_t now,
              uint32_t ring_bytes, uint32_t out_bytes)
{
  memset(se, 0, sizeof(*se));
  se->out = *out;
  se->fs = fs;
  se->keep_all = keep_all;
  se->text = text;
  se->start_time = now;
  se->last_time = now;
  se->frame_no = 0xffff;

  if (text)
  {
    se_text(se, now, fs ? (keep_all ? "Streaming started (full speed, all traffic)" : "Streaming started (full speed, idle traffic dropped)")
                        : (keep_all ? "Streaming started (low speed, all traffic)" : "Streaming started (low speed, idle traffic dropped)"));
  }
  else
  {
    static const char name[] = SE_NAME " " SE_VERSION;
    uint8_t p[12];

    p[0] = SE_PROTOCOL;
    p[1] = fs ? 1 : 0;
    p[2] = keep_all ? 1 : 0;
    p[3] = 0;
    put32(&p[4], ring_bytes);
    put32(&p[8], out_bytes);
    emit(se, SE_HELLO, now, p, 12, (const uint8_t *)name, sizeof(name) - 1, 0);
  }
}

//-----------------------------------------------------------------------------
void se_text(se_t *se, uint32_t now, const char *text)
{
  emit(se, SE_TEXT, now, (const uint8_t *)text, (int)strlen(text), 0, 0, 0);
}

//-----------------------------------------------------------------------------
void se_flush(se_t *se)
{
  if (se->tok_valid)
  {
    emit_packet(se, se->tok_time, 0, se->tok, 4);
    se->tok_valid = false;
  }

  if (se->dat_valid)
  {
    emit_packet(se, se->dat_time, se->dat_err, se->dat, se->dat_size);
    se->dat_valid = false;
  }
}

//-----------------------------------------------------------------------------
void se_lost(se_t *se, uint32_t lost_capture)
{
  se_flush(se); // the transaction it belonged to may be incomplete: send what there is
  se->pend_lost_capture += lost_capture;
  se->lost_capture += lost_capture;
  lost_report(se);
}

//-----------------------------------------------------------------------------
static bool is_token(int pid)
{
  return pid == SE_PID_IN || pid == SE_PID_OUT || pid == SE_PID_SETUP;
}

static bool is_data(int pid)
{
  return pid == SE_PID_DATA0 || pid == SE_PID_DATA1 || pid == SE_PID_DATA2 || pid == SE_PID_MDATA;
}

static bool is_handshake(int pid)
{
  return pid == SE_PID_ACK || pid == SE_PID_NAK || pid == SE_PID_STALL || pid == SE_PID_NYET;
}

#define HDR_FAST4  ((2u << 17) | 33)   // 4 bytes without a stuffed bit: 32 samples + SE0 in two words
#define HDR_NAK    ((1u << 17) | 17)   // a handshake: 16 samples + SE0 in one word

//-----------------------------------------------------------------------------
// Tokens and SOFs (nearly all of the idle traffic) are 4 bytes and rarely bit-stuffed. Then the NRZI decoding works
// on whole words: decoded bit i (sample at bit 30-i of the first word) is 1 when the line did not change. Returns
// false when the packet has an error of any kind (se_decode then says which).
INLINE bool fast4(se_t *se, const uint32_t *ring, uint32_t mask, uint32_t start, uint8_t *b)
{
  uint32_t w0 = ring[start & mask];
  uint32_t w1 = ring[(start + 1) & mask];
  uint32_t t = ~(w0 ^ ((w0 >> 1) | 0x40000000));
  uint32_t b31 = ~((w1 >> 1) ^ w0) & 1; // sample 31 is bit 1 of the second word, sample 30 bit 0 of the first

  b[0] = rev8((t >> 23) & 0xff);
  b[1] = rev8((t >> 15) & 0xff);
  b[2] = rev8((t >> 7) & 0xff);
  b[3] = rev8(((t << 1) | b31) & 0xff);

  if (b[0] != (se->fs ? 0x80 : 0x81))
    return false;
  if ((b[1] & 0x0f) != ((~b[1] >> 4) & 0x0f))
    return false;
  return crc5_usb(&b[2], 2) == 0x09;
}

//-----------------------------------------------------------------------------
static void hold_token(se_t *se, uint32_t time, const uint8_t *b)
{
  se_flush(se);
  se->tok_valid = true;
  se->tok_time = time;
  se->tok[0] = b[0]; // not memcpy: a call costs more than the copy
  se->tok[1] = b[1];
  se->tok[2] = b[2];
  se->tok[3] = b[3];
}

//-----------------------------------------------------------------------------
void se_raw(se_t *se, const uint32_t *ring, uint32_t mask, uint32_t start, uint32_t hdr, uint32_t end_time)
{
  uint32_t words = (hdr >> 17) & 0x3fff;
  uint32_t bits = hdr & 0x1ffff;
  uint32_t time = packet_start(se, end_time, bits);
  uint32_t err;
  int pid, n;

  se->last_time = time;

  if (hdr == HDR_FAST4)
  {
    uint8_t b[4];

    if (fast4(se, ring, mask, start, b))
    {
      pid = b[1] & 0x0f;

      if (pid == SE_PID_SOF)
      {
        se->frames++;
        se->frame_no = ((b[3] << 8) | b[2]) & 0x7ff;
        if (se->keep_all)
          emit_packet(se, time, 0, b, 4);
        else
        {
          se_flush(se); // a token nobody answered (isochronous, or a timeout)
          se->dropped++;
        }
        return;
      }

      if (is_token(pid))
      {
        if (se->keep_all)
          emit_packet(se, time, 0, b, 4);
        else
          hold_token(se, time, b);
        return;
      }
    }
  }
  else if (hdr == HDR_NAK && se->tok_valid && !se->keep_all && se_quick_pid(ring[start & mask], bits, se->fs) == SE_PID_NAK)
  {
    se->dropped += 2 + (se->dat_valid ? 1 : 0); // the token, its data (OUT), the NAK
    se->tok_valid = false;
    se->dat_valid = false;
    return;
  }

  if (bits == 0) // SE0 for a long time: a bus reset (or the device was unplugged)
  {
    se_flush(se);
    emit(se, SE_RESET, time, 0, 0, 0, 0, 1);
    return;
  }

  if (bits == 1) // an end of packet alone
  {
    if (!se->fs) // low speed: the keep-alive, once a millisecond (the frame)
    {
      se_flush(se);
      se->frames++;
      if (se->keep_all)
        emit(se, SE_KEEPALIVE, time, 0, 0, 0, 0, 1);
      else
        se->dropped++;
    }
    return; // full speed: the start of a reset, not a packet
  }

  n = se_decode(ring, mask, start, words, bits, se->fs, se->pkt, SE_MAX_BYTES, &err);

  if (bits > 0xffff)
    err |= SE_ERR_SIZE | SE_ERR_SYNC; // usb-sniffer-lite: "Synchronization error. Check your speed setting."

  pid = (n >= 2) ? (se->pkt[1] & 0x0f) : -1;

  if (err & (SE_ERR_MASK | SE_TRUNCATED))
  {
    se_flush(se);
    se->errors++;
    emit_packet(se, time, err, se->pkt, n);
    return;
  }

  if (pid == SE_PID_SOF) // with a stuffed bit
  {
    se->frames++;
    se->frame_no = ((se->pkt[3] << 8) | se->pkt[2]) & 0x7ff;
  }

  if (se->keep_all)
  {
    emit_packet(se, time, err, se->pkt, n);
    return;
  }

  if (pid == SE_PID_SOF)
  {
    se_flush(se);
    se->dropped++;
    return;
  }

  if (is_token(pid) && n == 4)
  {
    hold_token(se, time, se->pkt);
    return;
  }

  if (is_data(pid) && se->tok_valid && !se->dat_valid)
  {
    se->dat_valid = true;
    se->dat_time = time;
    se->dat_err = err;
    se->dat_size = n;
    memcpy(se->dat, se->pkt, n);
    return;
  }

  if (is_handshake(pid) && se->tok_valid)
  {
    if (pid == SE_PID_NAK)
    {
      se->dropped += 2 + (se->dat_valid ? 1 : 0);
      se->tok_valid = false;
      se->dat_valid = false;
      return;
    }
    se_flush(se);
    emit_packet(se, time, err, se->pkt, n);
    return;
  }

  se_flush(se);
  emit_packet(se, time, err, se->pkt, n);
}

//-----------------------------------------------------------------------------
uint32_t se_consume(se_t *se, const uint32_t *ring, uint32_t mask, uint32_t rd)
{
  uint32_t hdr = ring[rd & mask];
  uint32_t word1 = ring[(rd + 1) & mask];

  if (hdr == CAP_LOST_MARK)
  {
    se_lost(se, word1);
    return rd + 2;
  }

  se_raw(se, ring, mask, rd + 2, hdr, word1);
  return rd + 2 + ((hdr >> 17) & 0x3fff);
}

//-----------------------------------------------------------------------------
void se_status(se_t *se, uint32_t now, int type)
{
  if (type == SE_END)
    se_flush(se);

  if (se->text)
  {
    line_t *l = &g_line;

    lost_report(se);
    l_time(se, l, now);
    l_str(l, type == SE_END ? "Streaming stopped: " : "STATUS: ");
    l_dec(l, se->frames, 0);
    l_str(l, " frames");
    if (se->frame_no != 0xffff)
    {
      l_str(l, " (last #");
      l_dec(l, se->frame_no, 0);
      l_str(l, ")");
    }
    l_str(l, ", ");
    l_dec(l, se->dropped, 0);
    l_str(l, " idle packets dropped, ");
    l_dec(l, se->sent, 0);
    l_str(l, " sent, ");
    l_dec(l, se->errors, 0);
    l_str(l, " with errors, lost ");
    l_dec(l, se->lost_capture, 0);
    l_str(l, " + ");
    l_dec(l, se->lost_output, 0);
    l_str(l, ", buffers ");
    l_dec(l, se->ring_peak, 0);
    l_str(l, "% + ");
    l_dec(l, se->out_peak, 0);
    l_str(l, "%");
    if (room(se, l->len + 2, type == SE_END))
      l_emit(se, l);
  }
  else
  {
    uint8_t p[SE_STATUS_SIZE];

    put32(&p[0], se->frames);
    put16(&p[4], se->frame_no);
    put32(&p[6], se->dropped);
    put32(&p[10], se->sent);
    put32(&p[14], se->errors);
    put32(&p[18], se->lost_capture);
    put32(&p[22], se->lost_output);
    p[26] = se->ring_peak;
    p[27] = se->out_peak;
    lost_report(se);
    if (room(se, SE_RECORD_EXTRA + SE_STATUS_SIZE, type == SE_END))
      write_record(se, type, now, p, SE_STATUS_SIZE, 0, 0);
  }

  se->ring_peak = 0;
  se->out_peak = 0;
}
