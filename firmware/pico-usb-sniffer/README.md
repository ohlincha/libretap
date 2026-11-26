# USB Sniffer Stream (Raspberry Pi Pico firmware)

Continuous USB capture for LibreTap: a fork of **[usb-sniffer-lite](https://github.com/ataradov/usb-sniffer-lite)
by Alex Taradov** (BSD-3-Clause, see [LICENSE](LICENSE)). All the hard parts are his: the PIO programs that sample
a low- or full-speed USB bus with an RP2040, the packet decoder, the bare-metal USB CDC stack, the startup code.
This fork changes what happens to the packets.

usb-sniffer-lite fills the Pico's RAM (232 kB, often well under a minute) and prints a text dump after the capture
stops, so each capture is one short piece: Record, one action, Stop. USB Sniffer Stream captures without stopping and
streams over the Pico's own USB port while it captures. It drops the idle traffic of a busy bus on the Pico itself,
so it keeps up indefinitely with what instruments normally send.

**Status: untested on real hardware.** The filter, the record format and the capture loop's ring handling pass the
host tests. The whole firmware (core 0) boots, enumerates and streams correctly in the rp2040js emulator. The PIO
capture on core 1 is usb-sniffer-lite's, unchanged, but it has not been run in this firmware on a real Pico yet.

## Wiring

The same as usb-sniffer-lite, so a Pico wired for one runs either firmware:

| Pico pin | Signal | USB cable wire |
|---|---|---|
| GND | ground | black |
| GPIO 10 | D+ | green |
| GPIO 11 | D- | white |

The red wire (VBUS) stays connected through the cable and does not go to the Pico. GPIO 12 is used internally. The
trigger input (GPIO 18) is gone. The Pico's LED blinks slowly when idle and fast while streaming. GPIO 26 drives an
optional error LED (packets with errors: wrong speed setting or a poor splice).

## Flashing

1. Hold BOOTSEL on the Pico while plugging it into the laptop. A drive called `RPI-RP2` appears.
2. Copy `bin/UsbSnifferStream.uf2` onto it. The Pico restarts as "USB Sniffer Stream (RP2040)", a serial port
   (`/dev/ttyACM0`, `COM5`, …). It keeps usb-sniffer-lite's USB IDs (6666:6610), so its udev rule
   (`bin/90-usb-sniffer-lite.rules`: keeps ModemManager away, adds a `/dev/usbsniffer` link) applies to both.
3. To go back, copy usb-sniffer-lite's `UsbSnifferLite.uf2` the same way.

LibreTap tells the two firmwares apart by itself (the `v` command) and records continuously with this one.

## Commands

Single characters on the serial port, as in usb-sniffer-lite (any terminal works, e.g. `picocom /dev/ttyACM0`):

| Key | |
|---|---|
| `h` | help: the settings in usb-sniffer-lite's format (`  e - Capture speed       : Full`) |
| `v` | version: `USB Sniffer Stream 1.0.0 protocol 1` (usb-sniffer-lite does not answer `v`) |
| `e` | capture speed: Full / Low (only while stopped) |
| `k` | keep idle traffic: Disabled (default) / Enabled (SOF, NAKed transactions and keep-alives are streamed too) |
| `o` | output format: Binary (default, for LibreTap) / Text (usb-sniffer-lite-style lines, for a terminal) |
| `s` | start streaming |
| `p` | stop streaming (an END record follows) |

usb-sniffer-lite's `b` (show buffer), `g` (trigger), `l` (limit), `t` and `a` (display formats) and `f` (fold) do not
apply to a stream and are gone. Replies to commands given while binary records are streaming come as TEXT records.

## What is kept

A full-speed bus carries a SOF every millisecond, and hosts poll IN endpoints all the time; a device with nothing to
say answers NAK, often hundreds of times per millisecond. Unless "keep idle traffic" is on, the Pico:

- counts SOFs (and low-speed keep-alives) as frames and drops them;
- drops transactions answered by NAK: IN + NAK, and OUT + DATA + NAK (the host sends the same data again later);
- sends everything else, packet by packet: SETUP, every transaction with data (token, DATA, handshake), STALL,
  NYET, isochronous data, bus resets, and every packet with an error (CRC, bit stuffing, PID, sync, size).

A STATUS record every second gives the frame count and last frame number, how many idle packets were dropped, how
many were sent, errors, losses, and the peak fill of both buffers. On an idle bus that is the only traffic, so
LibreTap can tell "bus alive, nothing happening" from "cable not connected".

## Record format (protocol 1)

Binary, little-endian. It is binary rather than text because it is about three times smaller than hex text. Its
framing lets a reader find its place again after lost bytes. Every record:

```
0xA5 | type (1) | length (2) | time (4, µs, the Pico's timer, wraps after 71.6 min) | payload (length) | CRC-16 (2)
```

The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, length, time and payload. If the header or the
CRC does not check out, a reader skips one byte and looks for the next 0xA5.

| Type | Name | Payload |
|---|---|---|
| 1 | HELLO | u8 protocol (1), u8 speed (0 low, 1 full), u8 flags (bit 0: keep idle traffic), u8 0, u32 capture ring bytes, u32 output buffer bytes, ASCII name and version |
| 2 | PACKET | u8 flags (bit 0 stuffing, 1 CRC, 2 PID, 3 sync, 4 bit count, 5 size error; bit 7 truncated), then the packet from its PID byte on: tokens/SOF 3 bytes (PID + 11 bits + CRC5), data packets PID + data + CRC16, handshakes the PID alone |
| 3 | RESET | none (bus reset or device unplugged) |
| 4 | KEEPALIVE | none (low speed, only with "keep idle traffic") |
| 5 | LOST | u32 packets the capture could not store (buffer full), u32 records not sent (output full), since the previous LOST |
| 6 | STATUS | u32 frames, u16 last frame number (0xFFFF: none), u32 idle packets dropped, u32 packets sent, u32 packets with errors, u32 lost (capture, total), u32 lost (output, total), u8 capture buffer peak %, u8 output buffer peak % |
| 7 | TEXT | ASCII (replies to commands while streaming) |
| 8 | END | as STATUS; streaming has stopped |

PACKET time is when the packet started on the bus (±1 µs). HELLO, STATUS, END and TEXT carry the time they were
made. Because they are sent right away, a reader can line up the Pico's clock with its own.

## Throughput and what happens when it is too much

- **Bus side.** A full-speed bus moves at most about 1.2 MB/s of payload. A host polling an idle device can produce
  around 340,000 packets per second (IN/NAK pairs), and core 0 has to look at every one of them. The idle path
  (word-level decoding of 4-byte tokens and SOFs, NAK by its bit pattern) takes about 330 cycles per packet in the
  emulator's cycle model, about 365,000 packets/s at 120 MHz. On real hardware this has to be confirmed (see below).
- **Link side.** The CDC port is full speed too. With the upstream USB stack (one 64-byte buffer) expect a few
  hundred kB/s. A PACKET record is 11 bytes plus the packet. A typical instrument exchange (token, 20 data bytes,
  ACK) is about 60 bytes. A USB-serial instrument at 9600 baud, FTDI status polls included, is about 5 kB/s: a few
  percent of the link.
- **Bursts** go into two buffers: the capture ring (128 kB of raw line words, between the cores) and the output
  ring (80 kB of records). Together they hold roughly 150–200 ms of a fully loaded bus beyond what the link drains.
  That is plenty for a spectrum download, which only briefly exceeds the link.
- **Sustained overload** (more than the link or core 0 can take for longer): the output ring fills. Core 0 then
  stops taking packets from the capture ring and keeps room for one more record. Then the capture ring fills, and
  core 1 drops whole packets and counts them. Once there is room again it stores a marker, and the stream carries a
  LOST record with the exact count at the place of the gap. Nothing is lost silently: STATUS also reports totals
  and buffer peaks. With "keep idle traffic" on, a busy bus will overflow quickly; it is meant for short looks.

## Building

The upstream plain Makefile; no SDK. Needs the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(`arm-none-eabi-gcc`; 14.2.rel1 was used) on the PATH and Python 3 for the UF2 step:

```
cd firmware/make
make            # build/UsbSnifferStream.{elf,bin,hex,uf2}, copied to ../../bin/
```

`tools/bin2uf2.py` writes the UF2 file and fills in the CRC-32 the boot ROM checks in the first 256 bytes (the
job of ataradov's bin2uf2 tool). Run on usb-sniffer-lite's own release image, it reproduces that UF2 byte for byte.

## Tests

- `make -C tests`: host tests (gcc/clang, with address and undefined-behaviour sanitizers). Synthetic USB traffic
  goes through a stand-in for the capture PIO (`tests/usbline.h`: CRCs, bit stuffing, NRZI, the PIO's 31-bit words
  and bit counter). It then passes through the capture loop's own code (`capture_ring.h`), the ring consumer and the
  filter, and the records are parsed back. Checked: decoding of every packet kind at both speeds (all 2048
  tokens/frame numbers, maximum stuffing, 1023-byte packets), SOF/NAK floods dropped, transactions kept in order
  with the right times, keep-idle mode, resets, CRC and stuffing errors, isochronous and STALL, capture ring
  overflow (LOST count exact), output congestion (records dropped whole, reported once), resynchronisation after
  garbage, text output, low speed.
- `make -C tests emu RP2040JS=…/node_modules/rp2040js/dist/cjs/index.js`: runs the firmware in
  [rp2040js](https://github.com/wokwi/rp2040js) (MIT; installed separately with npm, not part of this repository).
  rp2040js emulates core 0 only, so `tests/emu_capture.c` stands in for `capture.c`: it feeds canned PIO words into
  the ring through the capture loop's code. The test checks boot, USB enumeration, commands, streaming of 20 loops of
  traffic (exact packet, frame and drop counts) and TEXT replies while streaming, and prints a cycle profile.

Only a real Pico can test the PIO capture under this firmware, timing with both cores busy, the CDC throughput, and
real hosts' NAK floods. Each STATUS record reports buffer peaks and losses to help with that.

## Files

Unchanged from usb-sniffer-lite (commit 10a19d9): `firmware/usb.c`, `usb.h`, `usb_std.c`, `usb_std.h`,
`usb_cdc.c`, `usb_cdc.h`, `usb_descriptors.h`, `utils.h`, `hal_gpio.h`, `pio_asm.h`, `startup_rp2040.c`,
`linker/rp2040.ld`, `bin/90-usb-sniffer-lite.rules`, and `include/` (CMSIS headers © Arm, Apache-2.0; `rp2040.h`
© Raspberry Pi, BSD-3-Clause).

Changed (marked at the top of each file):
- `capture.c`, `capture.h`: core 1 streams into the capture ring instead of filling a buffer once (PIO programs
  and pin setup unchanged); the trigger, the limit and the display are gone.
- `main.c`: core 0 handles the commands, drains the capture ring through the filter into an output ring, sends it
  over USB, and writes a STATUS record every second (clock and USB setup unchanged).
- `globals.h`: only the speed setting is left.
- `usb_descriptors.c`: product and manufacturer strings only.
- `make/Makefile`: file names, the UF2 step.

New: `firmware/stream.c`, `stream.h` (filter, record encoder, text lines; the decoder and CRC tables come from
usb-sniffer-lite's `capture.c`), `firmware/capture_ring.h`, `tools/bin2uf2.py`, `tests/`. Not taken: `display.c`,
`utils.c`, the hardware design files. The new files are BSD-3-Clause like the rest.
