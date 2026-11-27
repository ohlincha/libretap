TICK TEST KIT: DemoMeter and DemoInstrument
==========================================

A pretend instrument and its "vendor program", for trying Tick on a test PC or
virtual machine before using it next to a real instrument. Nothing to install.

  windows\DemoMeter.exe       the vendor program (Windows 98, 2000, XP and
                              later). Run it from Tick.
  windows\DemoInstrument.exe  the instrument, on the same Windows PC.
  linux/demo-instrument       the instrument on a Linux computer, e.g. the host
                              of a Windows XP virtual machine.

A. Everything on one Windows PC
-------------------------------
1. Start windows\DemoInstrument.exe. Leave its window open.
2. Start Tick.exe > Start a program... > DemoMeter.exe.
3. In DemoMeter: Connection "Network", address 127.0.0.1, Connect.
4. Change integration time and gain; press Mark in Tick before each change.
5. Stop in Tick. The session (TickSessions\...) goes into LibreTap:
   File > Add Tick session.

B. Instrument on the Linux host, Windows XP in a virtual machine
----------------------------------------------------------------
On the host, in a terminal:   ./demo-instrument
It prints what it listens on; Ctrl-C stops it.

Network: in DemoMeter (in the VM), Connection "Network", address = the host
as seen from the VM (VirtualBox with NAT networking: 10.0.2.2), Connect.

Serial (tests Tick's COM-port listener, as with OPUS): give the VM a COM1
connected to demo-instrument, then choose "COM1, 9600 baud" in DemoMeter.
  VirtualBox: Settings > Serial Ports > Port 1: Enable, Port Mode "TCP",
              "Connect to existing pipe/socket" ticked, Path/Address
              127.0.0.1:5026.  (Or Port Mode "Host Pipe", ticked,
              /tmp/demo-instrument-com.)
  QEMU:       -serial tcp:127.0.0.1:5026
The VM must be started after demo-instrument.

Expected in Tick: "DemoMeter.exe connected to ...:5025 (TCP)" or
"DemoMeter.exe opened COM1: 9600 baud, 8N1", and the byte counts rising.
The VirtualBox and QEMU serial settings have not been tested.

Commands (text lines ending in CR)
----------------------------------
  *IDN?   -> DEMO-METER,2.1
  INT n   -> OK            integration time, ms
  GAIN n  -> OK            1, 2 or 4
  MEAS?   -> M 0.5418 V
