# LibreTap session format (version 1, draft)

A **session** is one recording by one recorder: LibreTap on the sniffer laptop, or Tick on the instrument's
own PC. It is a plain folder, readable without LibreTap: JSON lines, PNG/JPEG images, optional pcap.
A **project** (LibreTap) combines sessions recorded at the same time and stores the clock offsets between them.

Everything is UTF-8. Bytes on a link are lowercase hex strings. Unknown keys must be ignored by readers, so
recorders may add fields without a version change.

## Folder

```
<name>/                     e.g. 20261130-101500-cora   (any name; the folder is the session)
  session.json              who recorded, when, which sources
  events.jsonl              everything that happened, one JSON object per line, in recording order
  frames/                   screen pictures (Tick: screenshots; LibreTap: camera stills), referenced by events
  files/                    copies of files (e.g. the vendor program's settings before the session)
  raw/                      optional raw captures (pcap from dumpcap, raw bytes per serial line, sniffer dumps)
```

A recorder writes `events.jsonl` as it goes (append, flush often), so a session that ends in a crash or a
pulled USB stick is still readable up to the last complete line. A reader ignores a final incomplete line (and
skips, but counts, any other line that is not JSON).

Lines are in recording order, which is *nearly* time order: a serial-tap chunk is written when it is complete, with
the time its first bytes arrived, so it can follow a line with a later `t`. Readers sort events by `t` (stable).
LibreTap writes `t` and `kind` first on each line so the file reads well; readers must not rely on key order.

## session.json

```json
{
  "format": "libretap-session",
  "version": 1,
  "recorder": "Tick 0.1.0",              // or "LibreTap 0.1.0"
  "host": "WIN2000-LAB",                 // computer name, may be ""
  "os": "Windows 5.0 build 2195",
  "started": "2026-11-30T10:15:00.123+01:00",
  "clock": "Recorder's own wall clock; event t = seconds since 1970-01-01 UTC by that clock",
  "sources": [
    {"id": "screen", "kind": "screen", "label": "Screen 1024x768", "width": 1024, "height": 768},
    {"id": "com1", "kind": "api-serial", "label": "COM1 (VENDOR.EXE)", "process": "VENDOR.EXE"}
  ],
  "note": "",
  "t_started": 1796031300.123,           // optional: "started" as epoch seconds (LibreTap writes it)
  "ended": "2026-11-30T11:02:10.456+01:00", "t_ended": 1796034130.456,   // optional, written at the end
  "stats": {"bytes_a": 1234, "bytes_b": 5678, "chunks": 210, "stills": 340, "marks": 7, "syncs": 1},  // optional
  "demo": true                           // optional: recorded from LibreTap's demo instrument, not real hardware
}
```

Source entries carry what a reader needs to label and time their events:

```jsonc
{"id": "tap", "kind": "serial-tap", "label": "Serial tap 9600 baud 8N1",
 "names": {"a": "PC", "b": "Instrument"}, "ports": {"a": "/dev/ttyUSB0", "b": "/dev/ttyUSB1"},
 "baud": 9600, "format": "8N1", "gap_ms": 20, "raw": {"a": "raw/a.bin", "b": "raw/b.bin"}}
{"id": "camera", "kind": "camera", "label": "HD USB Camera (/dev/video2) 1920x1080", "camera": "/dev/video2",
 "width": 1920, "height": 1080, "every_s": 0.5}
```

Source kinds:

| kind | recorder | data |
|---|---|---|
| `serial-tap` | LibreTap | two listen-only lines; `dir` is `a` (PC's TX) or `b` (instrument's TX); `names` gives labels |
| `usb-pico` | LibreTap | Pico usb-sniffer-lite: `usb` events (control transfers, resets) and `data` per endpoint (below) |
| `ethernet` | LibreTap | pcapng in `raw/`; `data` per TCP chunk / UDP datagram, `packet` summaries (below) |
| `usbpcap` | LibreTap (import) | a Windows USBPcap capture read in: the same `usb` / `data` events as `usb-pico` |
| `camera` | LibreTap | stills of the instrument PC's screen |
| `screen` | Tick | screenshots (full frames and changed regions) |
| `api-serial` | Tick | calls the vendor program makes on a COM port |
| `api-net` | Tick | socket calls (send/recv/connect) |
| `api-usb` | Tick | DeviceIoControl/ReadFile/WriteFile on non-COM device handles |
| `api-file` | Tick | files the vendor program opens, reads and writes (names, sizes; contents optional) |
| `input` | Tick | mouse clicks, key presses (key names only, never typed text), active window title |

## events.jsonl

Every event has `t` (float seconds, Unix epoch, recorder's clock, at least millisecond precision) and `kind`.
Most have `src` (a source id). Kinds:

```jsonc
{"t":1796031300.123,"kind":"data","src":"tap","dir":"a","hex":"2a49444e3f0d","end":1796031300.129}
                                    // bytes seen on a line; t = first bytes arrived, end (optional) = last bytes
{"t":…,"kind":"data","src":"com1","dir":"out","hex":"…","call":"WriteFile","n":6}    // Tick: program wrote
{"t":…,"kind":"data","src":"com1","dir":"in","hex":"…","call":"ReadFile","n":11}     // Tick: program read
{"t":…,"kind":"config","src":"com1","baud":9600,"bits":8,"parity":"N","stop":1,
      "rts":"handshake","dtr":"on","timeouts":{"read_interval":50}}                   // port settings changed
{"t":…,"kind":"open","src":"com1","path":"\\\\.\\COM1","ok":true}                       // handle opened/closed
{"t":…,"kind":"close","src":"com1"}
{"t":…,"kind":"call","src":"usb0","call":"DeviceIoControl","code":"0x00220004","in":"…","out":"…"}
{"t":…,"kind":"frame","src":"screen","file":"frames/000123.png","full":false,"x":10,"y":20,"w":300,"h":40}
{"t":…,"kind":"frame","src":"camera","file":"frames/cam_000045.jpg","full":true}
{"t":…,"kind":"input","src":"input","what":"click","button":"left","x":512,"y":300,"window":"Scan settings"}
{"t":…,"kind":"input","src":"input","what":"window","title":"Scan settings"}
{"t":…,"kind":"mark","n":3,"note":"integration time 1 s"}                              // operator or agent mark
{"t":…,"kind":"sync","n":1}                                                             // Sync button (manual sync)
{"t":…,"kind":"status","text":"COM1 opened by VENDOR.EXE"}                              // human-readable notes
{"t":…,"kind":"error","text":"camera disconnected"}
```

Serial tap: a chunk ends after `gap_ms` of silence on its line, or as soon as the other line starts talking. The
raw bytes of each line are also in `raw/a.bin` and `raw/b.bin` (no timing). A `config` event at the start records the
line settings the tap listened with.

Camera stills (LibreTap): a full-resolution JPEG every `every_s` seconds while the picture changes, and at least one
every 5 s when it does not; always `full: true`. `t` is when the frame reached the laptop (a frame or two late).

USB through the Pico sniffer (`usb-pico`, LibreTap): one capture per recording (the sniffer holds 232 kB). The
sniffer's text dump is kept in `raw/usb_cap.txt`; its decoded transfers are events. `dir` is `out` (host → device,
i.e. the PC's side) or `in`; `addr` is the device address, `ep` the endpoint number; `what` says it in words;
`t_ms` is the time in the dump (ms since the capture's first frame), and `t` = when Record started the capture +
`t_ms` (about ±50 ms).

```jsonc
{"t":…,"kind":"usb","src":"usb","addr":5,"ep":0,"dir":"out","what":"FTDI SET_BAUD_RATE wValue=0x4138 … (about 9600 baud)","t_ms":12.3}
{"t":…,"kind":"usb","src":"usb","addr":0,"ep":0,"dir":"in","what":"GET_DESCRIPTOR device #0: USB 2.00, VID 0403 PID 6001, …"}
{"t":…,"kind":"usb","src":"usb","what":"bus reset"}
{"t":…,"kind":"data","src":"usb","addr":5,"ep":2,"dir":"out","what":"bulk 6 bytes","hex":"4d4541533f0d","t_ms":14.0}
{"t":…,"kind":"data","src":"usb","addr":5,"ep":1,"dir":"in","what":"bulk 12 bytes","hex":"…"}   // FTDI status bytes removed
```

USB events also carry `setup` (the 8 setup bytes of a control transfer, hex) and `dev` (the device's `vid:pid` when
the capture saw its descriptor). Imported USBPcap captures (`usbpcap`) keep only the devices chosen when importing
(by USB id or address; hubs optionally); the source entry lists them (`devices`) and counts the left-out transfers
(`dropped_transfers`). Bulk transfers of USB-serial bridges are joined into lines there (until CR/LF, a 0.3 s pause
or the other direction), since such bridges often carry one character per transfer.

Control transfers (endpoint 0) are `usb` events, with the data stage in `hex` except for descriptors (decoded in
`what`). Bulk and interrupt payload is `data`, merged per endpoint until a short packet or a 10 ms pause (interrupt:
one report each). `truncated: true` marks data the sniffer cut short (its "limit to 16 bytes" display setting).
Source entry: `{"id":"usb","kind":"usb-pico","port":"/dev/ttyACM0","speed":"full","raw":"raw/usb_cap.txt","settings":{…}}`.

Ethernet (`ethernet`, LibreTap): dumpcap writes `raw/eth_NNNNN_<date>.pcapng` (a ring of files, 50 MB each, 20 at
most). TCP is reassembled per connection and direction (out-of-order segments held back, retransmitted bytes dropped)
and cut into chunks like the serial tap; UDP is one `data` event per datagram. `dir` `a` = towards the instrument
(to `instrument_ip` when one is set; else from the side that opened the connection, or from the higher port),
`b` = from it. Everything else worth a line is a `packet` event; pure TCP acknowledgements are only counted.

```jsonc
{"t":…,"kind":"data","src":"eth","dir":"a","proto":"tcp","conn":"192.168.0.10:50000 → 192.168.0.20:5025","hex":"2a49444e3f0a","end":…}
{"t":…,"kind":"data","src":"eth","dir":"b","proto":"udp","conn":"192.168.0.10:40000 → 192.168.0.20:5000","hex":"504f4e47"}
{"t":…,"kind":"packet","src":"eth","proto":"arp","text":"ARP: who has 192.168.0.20? tell 192.168.0.10"}
{"t":…,"kind":"packet","src":"eth","proto":"tcp","text":"TCP connection opened: 192.168.0.10:50000 → 192.168.0.20:5025"}
```

`conn` is always written PC side first. Source entry: `{"id":"eth","kind":"ethernet","interface":"eno1",
"instrument_ip":"192.168.0.20","names":{"a":"PC","b":"Instrument"},"raw":"raw/eth_*.pcapng","gap_ms":20}`. Packet
times are the capturing computer's clock, the same clock as the other LibreTap events.

Screen frames: the first frame and every Nth (default every 60th stored frame) are `full: true`; the others are
changed regions to be painted over the previous picture. A reader rebuilds the screen at time T by taking the
last full frame before T and painting the later regions up to T. Unchanged screenshots are not stored at all.
Tick details: one screenshot can give several region frames; they have the same `t` and follow each other in the
file. Full frames also carry `x`, `y` (0) and `w`, `h`. Tick also stores a full frame when more than half the screen
changed, and its own window is painted flat grey in the pictures (`"tick": {"tick_window_masked": true}` in
session.json) because its clock changes ten times a second. PNG files are 8-bit RGB.

Tick session details (Tick 0.2; `recorder` is `"Tick 0.2.0"`, or `"TickScreen 0.2.0"` for the screen-only build):
- session.json is rewritten when a source appears during recording (an `api-serial` source when the program first
  opens a COM port) and at the end (`ended`). It also has `utc_offset_min` (the PC's offset from UTC at the start)
  and `tick` (its screenshot options). `tick.capture` is `"screen"` (whole screen, the default) or
  `"program-windows"`: only the vendor program's visible top-level windows (dialogs included) are in the pictures,
  everything else is black (0,0,0); `tick.capture_process` then names that program (`""` while none is chosen:
  pictures are all black). Changes of the chosen program are also noted as `status` events. Folder name: `YYYYMMDD-HHMMSS-<computer name in lowercase>`.
- `api-serial` source ids are the port in lowercase (`com1`); `com-attached` for a port that was already open when
  Tick attached to a running program (its name is then unknown).
- `data`: `call` is `ReadFile`, `WriteFile`, `GetOverlappedResult` (an overlapped read completed),
  `ReadFile (overlapped, seen late)` (completion noticed only when the program reused its OVERLAPPED; `t` is late)
  or `TransmitCommChar`. Reads add `req` (bytes asked for); reads that return 0 bytes are not recorded. Writes are
  recorded when the call is made with all bytes passed; `written` is added if fewer were written, `pending: true`
  for overlapped writes still in progress.
- `config` from Tick names the `call`: `SetCommState` (baud, bits, parity `N/O/E/M/S`, stop `1`/`1.5`/`2`, rts
  `off/on/handshake/toggle`, dtr `off/on/handshake`, optional `cts_flow`, `dsr_flow`, `xonxoff` `in/out/both` with
  `xon`/`xoff` byte values), `SetCommTimeouts` (`timeouts` with `read_interval`, `read_total_multiplier`,
  `read_total_constant`, `write_total_multiplier`, `write_total_constant`, in ms), `EscapeCommFunction` (`dtr` or
  `rts` `on`/`off`, `xoff_sent`/`xon_sent`) or `SetCommBreak` (`break` `on`/`off`). `ok: false` when the call failed;
  `current: true` for the settings read from a port when attaching.
- `open` adds `error` (Windows error code) when `ok` is false and `overlapped: true` for overlapped handles.
- `call` events: `PurgeComm` (`flags`, hex), `SetupComm` (`in_queue`, `out_queue`), `DeviceIoControl` on a COM handle.
- `status`/`error` texts tell when Tick started the program, when its listener became active, when the program
  ended (with exit code), and when listener records were lost because Tick could not keep up.
- Network (Tick 0.4, `api-net`): one source `net` per program (`"process"`). Events carry `conn` (a connection id
  like `n3`, one per socket), `proto` (`tcp`/`udp`), `local` and `remote` (`"192.168.0.20:5000"`,
  `"[fe80:0:0:0:0:0:0:1]:5000"`): `open` (`call` `connect` or `accept`, `ok`, `error` = system error number),
  `data` (`dir` `out`/`in`, `hex`, `n`, `call` = `send`, `recv`, `sendto`, `recvfrom`, `WSASend`, `WSARecv`,
  `WSASendTo`, `WSARecvFrom`, `sendmsg`, `recvmsg`, `read`, `write`; `req` for receives; `sent` if fewer bytes went
  out, `pending: true` for overlapped sends still in progress; for UDP `remote` is the datagram's peer), `close`.
  Example: `{"t":…,"kind":"data","src":"net","conn":"n1","proto":"tcp","local":"127.0.0.1:2920","remote":
  "127.0.0.1:5800","dir":"out","hex":"2a49444e3f0d","call":"send","n":6}`.
- USB and other devices (Tick 0.4, `api-usb`): one source per device handle path, with `device` (the path); ids like
  `usb-vid_0403-pid_6001` (Windows `\\?\usb#vid_…&pid_…#…`), `hid-vid_046d-pid_c52b-mi_02`, `dev-<driver name>`
  (Windows `\\.\Name` vendor drivers), `usb-001-004` (Linux `/dev/bus/usb/001/004`), `hidraw0`, `usbtmc0`.
  `open`/`close`; `data` from ReadFile/WriteFile (`read`/`write`); pipe/bulk/interrupt transfers as `data` with
  `endpoint` (`"0x81"`; bit 7 = in) and `call` (`WinUsb_ReadPipe`, `WinUsb_WritePipe`, `ioctl USBDEVFS_BULK`,
  `ioctl USBDEVFS_SUBMITURB` for OUT, `ioctl USBDEVFS_REAPURB` for IN); HID reports as `data` with `report`
  (`feature`/`input`/`output`, `call` `HidD_…`; the first byte is the report id); control transfers as `data` with
  `transfer: "control"`, `setup` (8 bytes hex), `request_type`, `request`, `value`, `index`, `length` and the data
  stage in `hex`; `ok: false` when the call failed (then `hex` of an IN transfer is empty); other device ioctls as
  `call` events (`DeviceIoControl` / `ioctl`, `code`, `in`, `out`).
- Tick for Linux (0.3) writes the same format. `os` is like `"Linux 6.12.1 x86_64"`. Its `api-serial` sources are
  named after the device (`/dev/ttyUSB0` -> id `ttyusb0`, `/dev/pts/3` -> `pts3`, a path outside /dev -> its file name)
  and carry `device` (the path). `data` calls are `read`, `write`, `readv`, `writev`. `config` events come from
  `tcsetattr` or `ioctl TCSETS` (`baud`, `bits`, `parity`, `stop`, `rts: "handshake"` + `cts_flow` for RTS/CTS,
  `xonxoff`, `raw` or `canonical`, `vmin`, `vtime`, and the raw `termios` flags) or from `ioctl TIOCMSET/TIOCMBIS/
  TIOCMBIC` (`dtr`, `rts` `on`/`off`) and `ioctl TIOCSBRK/TIOCCBRK`/`tcsendbreak` (`break`); `call` events from
  `tcflush` (`queue` `in`/`out`/`both`) and other ioctls (`code`). A failed open of a serial-looking device is an
  `open` event with `ok: false` and `error` (text). Screen sources are labelled with the X display; `cursor` is false
  (the pointer is not drawn on Linux). `tick/program-output.log` holds the started program's terminal output when
  Tick runs with its terminal UI.
- `files/` holds the program's small settings files (`*.ini`, `*.cfg`, `*.cnf`, `*.conf`, `*.set`, `*.par`, `*.prm`
  from its folder, up to 1 MB each) copied before it started; `tick/` holds the listener Tick loaded into the program: `TickHook.dll`, or for a
  64-bit program `TickHook64.dll` and `TickHelper64.exe` (readers ignore this folder).

## Projects (recordings)

Every LibreTap recording is a project folder: `project.json` and the session folders inside it. Sessions added
later (Tick's, from a USB stick) are copied in, so a recording is always one folder to keep or send.

```
20261130-101500/
  project.json
  20261130-101500-libretap/          LibreTap's session (the reference clock)
  20261130-101448-WIN2000-LAB/       a Tick session, added
```

```json
{"format": "libretap-project", "version": 1, "name": "20261130-101500",
 "clock": "project_time = t + offset + drift * (t - t0), per session",
 "sessions": [
   {"path": "20261130-101500-libretap", "offset": 0, "drift": 0, "t0": 1796031300.1, "how": "reference", "match": {}},
   {"path": "20261130-101448-WIN2000-LAB", "offset": 3587.6575, "drift": 7.995e-05, "t0": 1796027712.4,
    "how": "traffic", "match": {"matches": 1241, "residual": 0.0009, "confidence": 0.98, "message": "…"}}]}
```

`path` is relative to the project folder (absolute when outside it). `how` is `reference`, `traffic`, `sync`, `manual`
or `none` (not matched yet).

## Clock matching (projects)

Each recorder uses its own clock. A LibreTap project stores, per session, `offset` and `drift` so that
`project_time = t + offset + drift * (t - t0)`. They come from:
1. **traffic matching**: the same bytes in a Tick `api-serial` session and a LibreTap `serial-tap` session
   (several matches give the drift);
2. **manual sync**: `sync` events with the same `n` pressed on both at the same moment (about ±0.3 s).

Traffic matching in LibreTap 0.1: each data event of one session (both directions: `out`↔`a`, `in`↔`b`) is looked
up in the other session's byte stream of the same direction; every place it occurs gives a candidate time
difference. A histogram (0.1 s bins, each event's vote shared among its candidates) finds the true difference even
for a program that polls the same command over and over (the answers carry unique readings). Then a line is fitted
through the closest candidate of each write (`out`/`a` only; reads reach Tick late), with shrinking windows, giving
offset and drift (drift only when the matches span 30 s or more). The median lag of the reads is reported as
`read_lag`. A result counts when at least 3 pieces agree and the best difference clearly beats the next best
(`confidence` > 0.6); otherwise Sync presses are used.
