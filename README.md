# LibreTap

**Your instrument outlived its computer.**

LibreTap liberates old lab instruments from the old PCs that run their vendor software. While an operator uses the
vendor program as usual, LibreTap listens on a laptop: passive sniffers record every byte between the PC and the
instrument, and a webcam keeps pictures of the PC's screen. Afterwards the timeline shows screen and traffic side
by side, so anyone (or an AI assistant) can work out what each setting sends, and write new software for the
instrument.

![Timeline](app/docs/timeline-with-tick-light.png)

- **Serial tap**: a passive two-lead DB-9 tap and two USB-serial adapters (one per direction). The adapters only
  listen; nothing is ever sent.
- **USB sniffer**: a Raspberry Pi Pico with usb-sniffer-lite spliced into the instrument's USB cable (low/full
  speed). It captures in short pieces: press Record, do one action, press Stop. Control requests are named
  (descriptors, CDC, FTDI/CH34x/PL2303/CP210x), data is shown per endpoint.
- **Ethernet**: a switch with port mirroring (e.g. TP-Link TL-SG105E) copies the instrument's and the PC's traffic to
  the laptop; Wireshark's dumpcap captures it, LibreTap shows the TCP/UDP conversations like serial traffic.
- **Camera**: a full-resolution still of the instrument PC's screen every half second (only when it changes).
- **Tick**: a small companion program for the old PC itself (Windows 2000 to 11, separate project) records the
  screen and the vendor program's port calls in the same format. *Add Tick session…* combines its folder with
  LibreTap's recording; the clocks are lined up automatically from the traffic both saw, or with the Sync button.
- **Marks**: press Mark (space bar) and say what you are about to change; marks appear on the timeline.
- **Agents**: everything works from the command line, also against the running window:
  `LibreTapCLI status | start | mark "note" | sync | stop | ports | sessions | show | match | import`, all with
  `--json`.
- **Demo**: no hardware? `LibreTap --demo` (or Help ▸ Try the demo) records a pretend instrument.

**Status: version 0.1, untested on real hardware.** Tested with pseudo-terminals, a simulated Pico, synthetic
network captures and a stand-in dumpcap, simulated cameras, and real Tick sessions recorded under Wine; Windows and
macOS builds and the installers' permission steps are set up in CI but not yet run.

## What the installers change on your system

Capturing network traffic needs permission, so the installers set it up (this changes system settings):

- **Linux (.deb)**: depends on `wireshark-common` (dumpcap) and `ffmpeg`. Its post-install script does what
  answering "Yes" to Wireshark's "allow non-superusers to capture packets?" does: a `wireshark` group, dumpcap
  runnable by that group with the network-capture capabilities (`cap_net_raw,cap_net_admin`), and the user who ran
  the installation (via sudo or pkexec) added to the group. Until that user logs in again, LibreTap starts dumpcap
  through `sg wireshark`. Undo: `sudo dpkg-reconfigure wireshark-common` (answer No) and
  `sudo gpasswd -d $USER wireshark`. If it could not be set up, the Connections area says so with the one command
  that fixes it.
- **Windows**: Npcap (the capture driver) may not be bundled under its licence. The installer offers, checked when
  Wireshark is missing, to download Wireshark's official installer from wireshark.org, which installs dumpcap and
  offers Npcap; you click through it (it asks for administrator rights). LibreTap itself installs per user.
- **macOS**: nothing is changed. For Ethernet capture install Wireshark including its "ChmodBPF" part (it gives your
  user access to the capture devices); the Connections area tells you when it is missing.

The serial tap and the USB sniffer need no special rights on Windows and macOS; on Linux your user must be in the
`dialout` group (most desktop distributions do this for the first user).

## Build

Qt 6.5+ (Core, Gui, Widgets, SerialPort, Network), CMake, a C++17 compiler; ffmpeg for cameras on Windows/macOS;
Wireshark's dumpcap for Ethernet capture (pcap/pcapng files are read by LibreTap itself).

```
cmake -S app -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Recordings go to `~/LibreTap` (change under Advanced…, or `$LIBRETAP_HOME`). The file format is plain JSON lines and
pictures: see [docs/FORMAT.md](docs/FORMAT.md).

MIT licence (app/LICENSE).
