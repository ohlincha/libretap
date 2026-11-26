// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Copyright (c) 2026, LibreTap contributors. Streaming additions (new file, not in usb-sniffer-lite).
//
// The streaming part of USB Sniffer Stream: turns the raw packets the capture core leaves in the capture ring
// (capture_ring.h) into a continuous stream of records for the laptop, dropping the idle traffic of a busy bus.
// Plain C without hardware access, so it also compiles on a PC for the unit tests (tests/).
//
// Filter (unless "keep idle traffic" is on): SOF packets and low-speed keep-alives are only counted (frame counter);
// a transaction answered by NAK (IN + NAK, OUT + DATA + NAK) is dropped and counted. Everything else is sent:
// SETUP, every transaction with data, STALL, NYET, packets with errors, bus resets.
//
// Record format (protocol 1; also in ../../README.md and LibreTap's docs/FORMAT.md):
//   0xA5 | type | length (2 bytes LE) | time (4 bytes LE, us, the Pico's timer) | payload (length bytes) | CRC-16 (LE)
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff) over type, length, time and payload. A reader that loses its place
// looks for the next 0xA5 whose header and CRC check out.
#ifndef _STREAM_H_
#define _STREAM_H_

#include <stdint.h>
#include <stdbool.h>

#define SE_VERSION        "1.0.0"
#define SE_PROTOCOL       1
#define SE_NAME           "USB Sniffer Stream"

#define SE_SYNC           0xA5
#define SE_HEADER_SIZE    8      // sync, type, length, time
#define SE_RECORD_EXTRA   10     // header + CRC
#define SE_MAX_PAYLOAD    2048

enum
{
  SE_HELLO     = 1,   // streaming started: u8 protocol, u8 speed (0 low, 1 full), u8 flags (bit 0: keep idle traffic),
                      // u8 0, u32 capture ring bytes, u32 output buffer bytes, firmware name and version (ASCII)
  SE_PACKET    = 2,   // u8 flags (SE_ERR_*), then the packet from its PID on (PID, fields or data, CRC)
  SE_RESET     = 3,   // bus reset (no payload)
  SE_KEEPALIVE = 4,   // low-speed keep-alive (only with "keep idle traffic")
  SE_LOST      = 5,   // u32 packets the capture core could not store (ring full), u32 records not sent (output full)
  SE_STATUS    = 6,   // once a second: SE_STATUS_SIZE bytes, see se_status()
  SE_TEXT      = 7,   // command replies while streaming (ASCII)
  SE_END       = 8,   // streaming stopped: same payload as SE_STATUS
};

// SE_PACKET flags (the capture's error checks, as usb-sniffer-lite's ERROR [...] line)
#define SE_ERR_STUFF      0x01
#define SE_ERR_CRC        0x02
#define SE_ERR_PID        0x04
#define SE_ERR_SYNC       0x08
#define SE_ERR_NBIT       0x10
#define SE_ERR_SIZE       0x20
#define SE_ERR_MASK       0x3f
#define SE_TRUNCATED      0x80   // longer than the capture keeps (CAP_MAX_WORDS): the tail is missing

#define SE_STATUS_SIZE    28     // u32 frames, u16 last frame number (0xffff: none), u32 idle packets dropped,
                                 // u32 packets sent, u32 packets with errors, u32 lost (capture, total),
                                 // u32 lost (output, total), u8 capture ring peak %, u8 output buffer peak %

// USB packet IDs
enum
{
  SE_PID_OUT = 1, SE_PID_IN = 9, SE_PID_SOF = 5, SE_PID_SETUP = 13,
  SE_PID_DATA0 = 3, SE_PID_DATA1 = 11, SE_PID_DATA2 = 7, SE_PID_MDATA = 15,
  SE_PID_ACK = 2, SE_PID_NAK = 10, SE_PID_STALL = 14, SE_PID_NYET = 6,
  SE_PID_PRE = 12, SE_PID_SPLIT = 8, SE_PID_PING = 4, SE_PID_RESERVED = 0,
};

#define SE_MAX_BYTES      1280   // decoded packet bytes kept (sync included); CAP_MAX_WORDS * 31 / 8 fits

// Where records go: the firmware's output ring (USB), a buffer in the tests.
typedef struct
{
  int  (*space)(void *ctx);                              // bytes that can be written now
  void (*write)(void *ctx, const uint8_t *data, int size);
  void *ctx;
} se_out_t;

typedef struct
{
  se_out_t out;
  bool     fs;           // full speed (else low speed)
  bool     keep_all;     // keep idle traffic (SOF, NAKed transactions, keep-alives)
  bool     text;         // human-readable lines instead of binary records

  // the transaction being collected (a token and its data), sent once it is clear it was not answered by NAK
  bool     tok_valid;
  uint32_t tok_time;
  uint8_t  tok[4];       // SYNC, PID, address/endpoint, CRC
  bool     dat_valid;
  uint32_t dat_time, dat_err;
  int      dat_size;
  uint8_t  dat[SE_MAX_BYTES];

  uint8_t  pkt[SE_MAX_BYTES];   // scratch for decoding

  uint32_t start_time;   // HELLO time (text mode prints times relative to it)
  uint32_t last_time;    // the latest packet's time
  // counters (since the start)
  uint32_t frames, frame_no, dropped, sent, errors, lost_capture, lost_output;
  // not yet reported in an SE_LOST record
  uint32_t pend_lost_capture, pend_lost_output;
  // peaks since the last status, in percent (filled in by the firmware)
  uint8_t  ring_peak, out_peak;
} se_t;

#ifdef __cplusplus
extern "C" {
#endif

uint16_t se_crc16(const uint8_t *data, int size, uint16_t crc);

// Decodes one raw packet from the capture ring (NRZI, bit stuffing, CRC and PID checks; usb-sniffer-lite's
// process_packet). `bits` is the bit count the capture measured (the end-of-packet sample included), `words` the data
// words stored. Writes the bytes (SYNC first) to out, returns their number, sets *err to SE_ERR_* / SE_TRUNCATED.
int se_decode(const uint32_t *ring, uint32_t mask, uint32_t start, uint32_t words, uint32_t bits, bool fs,
              uint8_t *out, int max, uint32_t *err);

// The PID of a raw packet from its first data word alone (SYNC and PID are never bit-stuffed), or -1 when the first
// 16 bits are not a valid SYNC + PID. Lets the filter drop NAKs without decoding them.
int se_quick_pid(uint32_t first_word, uint32_t bits, bool fs);

void se_start(se_t *se, const se_out_t *out, bool fs, bool keep_all, bool text, uint32_t now,
              uint32_t ring_bytes, uint32_t out_bytes);
// One raw packet: hdr and end time as the capture core stored them, its data words at ring[start..]
void se_raw(se_t *se, const uint32_t *ring, uint32_t mask, uint32_t start, uint32_t hdr, uint32_t end_time);
// Consumes the ring entry at rd (a packet or a LOST marker); returns the index of the next one.
uint32_t se_consume(se_t *se, const uint32_t *ring, uint32_t mask, uint32_t rd);
void se_lost(se_t *se, uint32_t lost_capture);   // the capture core could not store this many packets
void se_flush(se_t *se);                         // sends the transaction being collected
void se_status(se_t *se, uint32_t now, int type);   // SE_STATUS or SE_END (flushes first)
void se_text(se_t *se, uint32_t now, const char *text);   // a TEXT record (binary) or the text itself

#ifdef __cplusplus
}
#endif

#endif // _STREAM_H_
