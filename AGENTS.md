# AGENTS.md — for AI assistants working with LibreTap

You are helping someone free an old laboratory instrument from its old computer. LibreTap records what the
vendor software says to the instrument (and what the instrument answers), together with the instrument PC's screen;
then you and the operator work out the protocol, so new, open software can drive the instrument.

Everything LibreTap does is available from the terminal: `LibreTapCLI` (add `--json` to any command). A running
LibreTap window or `LibreTapCLI record` listens on a local socket, so the same commands drive whichever is running.

## The pieces

- **LibreTap** (this repository, `app/`): runs on a laptop next to the instrument. Sources:
  - **serial tap**: a passive two-lead DB-9 tap, two USB-serial adapters; line `a` = the PC's TX, `b` = the instrument's.
  - **USB sniffer**: a Raspberry Pi Pico with usb-sniffer-lite in the instrument's USB cable (low/full speed),
    one short capture per action.
  - **Ethernet**: a mirroring switch (TL-SG105E) and Wireshark's dumpcap; TCP/UDP conversations.
  - **camera**: stills of the instrument PC's screen.
  - **USBPcap import**: a capture made with Wireshark/USBPcap on the instrument PC.
- **Tick** (`tick/`): a small program for the instrument PC itself (Windows 2000–11, Linux): records its screen and
  the vendor program's serial port calls into the same format. Its session folders are added to a LibreTap
  recording; the clocks are matched from the traffic both saw (or Sync presses).
- **Formats**: [docs/FORMAT.md](docs/FORMAT.md) (sessions: folders of JSON lines and pictures; projects), and
  [docs/PROTOCOL-FORMAT.md](docs/PROTOCOL-FORMAT.md) (protocol descriptions). You can read the files directly.

## Command reference

Capture (the operator is at the instrument; you run these):

```
LibreTapCLI ports [--json]                   what is plugged in: tap adapters, Pico, cameras, network, in sentences
LibreTapCLI guess PORT [PORT]                baud rate, while the vendor software talks
LibreTapCLI record [--a P --b P] [--baud N|auto] [--usb-pico PORT|auto] [--ethernet IFACE|auto [--instrument-ip IP]]
                   [--camera ID|auto|none] [--name NAME] [--seconds S]      headless recording
LibreTapCLI status | start | mark "note" | sync | stop                      control the running recorder or window
LibreTapCLI import REC TICK_SESSION          add a Tick session and match the clocks
LibreTapCLI import-usbpcap FILE --device VID:PID … [--hubs]               a USBPcap capture, chosen devices only
LibreTapCLI camera [--id ID] [--json]        camera picture: readability of the screen's text, screen region, settings
LibreTapCLI camera optimise | reset          set exposure and focus for the screen | camera defaults
LibreTapCLI camera set exposure_auto=1 exposure=120 focus_auto=1 …        change settings (Linux cameras; saved)
LibreTapCLI camera region auto|whole|X,Y,W,H | still FILE.jpg              where the screen is | the current picture
```

`camera` talks to the running window or recorder when there is one (otherwise it opens the camera itself). Check
the verdict before recording: anything but "Text readable" means the stills will not show the settings; run
`camera optimise`, then look at `camera still` yourself. Settings are Linux-only for now (Windows/macOS cameras go
through ffmpeg); the readability check works everywhere. Its thresholds are calibrated on synthetic pictures only.

Analysis (REC = recording folder, name, or `latest`; a moment = `--mark N` or `--at SECONDS`):

```
LibreTapCLI sessions | show REC [--hex] | stats REC
LibreTapCLI exchanges REC                    question → answer pairs per link, repeats folded, counts, timing
LibreTapCLI diff REC --mark 3 --mark 4       what differs between two moments, byte by byte, per kind of message
LibreTapCLI hints REC [--family TEXT]        text/binary, line end, header, length field, checksum candidates,
                                             numbers/bytes that vary and their values at each mark
LibreTapCLI grep REC TEXT | hex:AA BB
LibreTapCLI screen REC --mark N --out f.png  the screen when that mark was set (look at it!)
LibreTapCLI screen REC --at T --out before.png --after after.png
                                             cause and effect around a message (T: its time from `show`): the
                                             screen before it, and the first changed picture after it with the
                                             changes outlined (an answer shows up after; a setting changed before)
LibreTapCLI keyframes REC --out DIR          screen pictures at marks and where a command changed
LibreTapCLI export REC --ai DIR              index.md + traffic, exchanges, hints, diffs between marks, key frames
LibreTapCLI decode REC --protocol FILE [--unexplained]   sentences, and the % of bytes explained
LibreTapCLI match A B [--save] | decode-usb DUMP | pcap FILE
```

## The workflow with the operator

1. **Before**: ask what the instrument is, how it connects (RS-232, USB, Ethernet), what the vendor software is.
   `LibreTapCLI ports --json` to see what is plugged in. With a camera: `LibreTapCLI camera` until it says "Text
   readable" (`camera optimise`; ask the operator to move the camera closer or square to the screen if not). For RS-232, `guess` the baud rate while the software talks.
2. **Record**: `LibreTapCLI record … &` (or the operator presses Record in the window). Ask the operator to change
   **one setting at a time** in the vendor software. Before each change: `LibreTapCLI mark "integration time 1 s → 2 s"`
   (the operator can also press Mark / the space bar). Ask them to save the vendor software's result file for each
   measurement: known values to find in the bytes. With the USB sniffer: one capture per action (start, action, stop).
3. **Look**: `exchanges` (what is polled, what changes), `diff --mark N --mark N+1` for each change,
   `screen --mark N --out …` and read the picture: the settings shown on the screen are the ground truth.
   `hints` for framing, checksums and length fields.
4. **Describe**: write a protocol description (JSON) message by message; `decode --protocol` shows the sentences
   and the coverage; work through the unexplained kinds (`--unexplained`) until the coverage is high.
5. **Report** to the operator in plain words: what each command does, what is verified, what is guessed.

## Safety rules

- **Never send anything to an instrument** (and never write code that does) without the operator's explicit OK for
  that exact command. LibreTap itself only listens: the tap cannot transmit, and nothing in LibreTap writes to an
  instrument's port.
- Instruments can be dangerous: **lasers** (eyes), **high voltage**, **vacuum**, **heaters**, **dosing**
  (titrators, pumps: chemicals), moving stages. When a command switches any of these, say so in capitals in the
  description and in your messages, and say what the operator must check first (interlocks, shutters, covers).
- Say clearly what is **verified** (seen on the screen, measured, confirmed by the operator) and what is **guessed**
  (inferred from bytes). A protocol description's `notes` should say it too.
- **Recordings may contain private data**: USB captures include keyboards (typed passwords) and USB sticks; screens
  show user names and file names; Ethernet captures show other traffic. Import only the instrument's devices
  (`import-usbpcap --device`), and never publish raw captures without the operator's consent.
- Don't modify vendor software or its files; don't publish vendor code.

## Worked example: Anton Paar Cora 5000 (Raman spectrometer)

`app/examples/cora/`: a demo recording made from a real USBPcap capture of the vendor software (initialise, arm,
preview, scan), keeping only the instrument's own USB devices, and its protocol description. Open it with
`LibreTap --demo cora`, or from the terminal:

```
REC=app/examples/cora/demo
LibreTapCLI stats $REC
LibreTapCLI exchanges $REC | head          # the MCP2200 status poll (1218×), the I/O board's VOL 03 (114×), …
LibreTapCLI diff $REC --mark 2 --mark 3    # arm → preview: exposure 0xAE changed, I/O line 0b → 09 (laser)
LibreTapCLI decode $REC --protocol app/examples/cora/cora.protocol.json | tail   # 95 % of the bytes explained
```

What it is (from LibreRaman's PROTOCOL.md, worked out exactly this way): behind the instrument's USB hub,
two BaySpec spectrometers (`ba8c:1688`, vendor control requests; `0xAE` sets the exposure in 16 µs units − 1;
`0xAA` starts an exposure; a frame is 4608 bytes of big-endian pixels on bulk endpoint 2), a Prolific I/O board
(`067b:2303`, 115200 8N1, LF-terminated ASCII: `IO L 09` = **532 nm laser emission on**, answered `OK`), a
Silicon Labs laser driver (`10c4:ea60`, 19200 8N1, CR: `laseron`, `current 0.39` = **1064 nm laser**), and an MCP2200
(`04d8:00df`, HID: status poll, byte 10 bit 0 = armed). Arming needs a person at the instrument; the software
cannot do it. That is the kind of conclusion the analysis should lead to, verified with the operator.
