TICK TEST KIT: DemoMeter and DemoInstrument
==========================================

A pretend instrument and its "vendor program", for trying Tick on a test PC or
virtual machine before using it next to a real instrument. Windows 98, 2000, XP
and later. Nothing to install.

  DemoInstrument.exe   the instrument. Answers on the network (this computer,
                       port 5025) and on COM2 if there is one.
  DemoMeter.exe        the vendor program. Integration time, gain, a reading
                       every 0.5 s.

Try it
------
1. Copy this folder and Tick.exe onto the test PC.
2. Start DemoInstrument.exe. Leave its window open. If Windows Firewall asks,
   choose Unblock (or Keep blocking: the test uses this computer only).
3. Start Tick.exe. Press "Start a program...", choose DemoMeter.exe.
4. In DemoMeter, press Connect. Change the integration time and the gain;
   press Mark in Tick before each change.
5. Stop in Tick. The session folder (TickSessions\...) holds the screenshots
   and every command and answer. Open it in LibreTap: File > Add Tick session.

Expected in Tick: "DemoMeter.exe connected to 127.0.0.1:5025 (TCP)" and the
byte counts rising.

Serial (optional)
-----------------
For Tick's serial listener, the test PC needs two COM ports wired to each
other: COM1 for DemoMeter, COM2 for DemoInstrument. On a real PC: a null-modem
cable between COM1 and COM2. In a virtual machine: connect the VM's COM1 and
COM2 to each other (e.g. VirtualBox: both ports as "Host Pipe" with the same
pipe path, the first with "Connect to existing pipe" off, the second on).
Then choose "COM1, 9600 baud" in DemoMeter before Connect.
The VM setup has not been tested.

Commands (text lines ending in CR)
----------------------------------
  *IDN?   -> DEMO-METER,2.1
  INT n   -> OK            integration time, ms
  GAIN n  -> OK            1, 2 or 4
  MEAS?   -> M 0.5418 V
