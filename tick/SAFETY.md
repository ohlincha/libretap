# Is it safe to run Tick on my instrument PC?

Short answer: **recording the screen is very low risk**; **listening to the vendor program** ("Start a program...")
puts a small piece of Tick inside that program, which is a real (small) risk. This page says exactly what Tick
does, what can go wrong and what you do then. It describes Tick 0.2.0. Everything marked *tested* was tested
under Wine (a Windows compatibility layer on Linux) set to behave like Windows 2000/XP/7, with a test program and a
pretend instrument; **Tick has not yet run on a real Windows PC or next to a real instrument.**

## Which way to use it

- **Tick.exe is the normal choice.** If IT rules or the antivirus do not accept a program that loads a DLL into
  another program, use **TickScreen.exe** instead: the same program without the listener (no DLL inside, no
  "Start a program...", no attaching). It only takes screenshots and notes marks and syncs, which is very low risk.
- **To record the serial port, use "Start a program...".** Tick then starts the vendor program itself and records
  the port being opened and all its settings from the start. Nothing of Tick is ever placed inside a program that
  was already running.
- *Advanced... > Listen to a running program* is only for a program that is in the middle of a measurement and
  cannot be restarted. It places the listener inside a program that is already working, and it misses the
  settings made before attaching (Tick reads the current ones back once). Not tested yet.

## What Tick changes on the PC

- **Nothing is installed.** No setup, no driver, no service, nothing starts with Windows, no registry entries
  written by Tick.
- Tick writes only into **its session folder**: `TickSessions\` next to `Tick.exe` (if that is not writable:
  `Documents\TickSessions`, and as a last resort the TEMP folder; the window always shows which). Plus one file,
  `Tick.ini` next to `Tick.exe`, and only when you press OK in *Advanced...*.
- With *Start a program...*, Tick also writes `TickHook.dll` (its listener) into the session folder's `tick\`
  folder and loads it into the vendor program **while that program runs**. For a 64-bit vendor program (64-bit
  Windows only) it writes `TickHook64.dll` and a small `TickHelper64.exe` there instead; the helper runs for a
  moment to load the DLL and then exits. Nothing in the vendor program's folder is changed. Tick
  reads (copies) the program's small settings files (`*.ini`, `*.cfg`, ...) into the session's `files\` folder.
- Windows itself may remember that Tick ran, as it does for every program (for example the list of recently opened
  files in the *Open* dialog).

**To remove Tick:** delete `Tick.exe` (or `TickScreen.exe`), `Tick.ini` (if any) and the `TickSessions` folder.
That is all.

**What is in the pictures:** by default the whole screen (except Tick's own window, painted grey). If other things
on the screen must not be recorded, *Advanced... > Screenshots show only the vendor program's windows* paints
everything except that program's windows black (anything lying on top of the program's window is still seen).
Window titles of the active window are recorded too (switch off in Advanced). Typed text is never recorded as
keystrokes, but whatever is visible on the screen is in the pictures.

## What can go wrong, and what to do

**Tick hangs, crashes, or is closed with Task Manager.**
The vendor program keeps running. If Tick was listening, the listener inside the vendor program keeps passing every
call through unchanged; it never waits for Tick (when its 1 MB buffer is full it simply stops keeping records).
The session is readable up to the moment Tick stopped. *What you do:* nothing for the instrument; start Tick again
if you want to go on recording (a new session folder is made). The vendor program is no longer listened to until
it is restarted from Tick. *Tested:* Tick killed while listening; the test program finished all its 20 exchanges
with the pretend instrument; the session was readable.

**Tick is closed normally while it listens.** Tick asks first. The program keeps running; the listener goes passive
(calls pass through). *Tested.*

**The vendor program hangs or crashes while Tick listens.**
Close it with Task Manager if needed, then start it again **the normal way, from its own icon**: Tick leaves
nothing behind in the program or on the disk, so a normal start is exactly as before Tick. Tick notes in the
session that the program ended (and its exit code) and keeps recording the screen. If the crash happens again only
when started from Tick, do not use the listener with this program (record the screen only) and please report it.
Note: the listener runs inside the vendor program, so a mistake in the listener *could* make the program crash;
this has only been tried with a test program so far. *Tested:* test program killed while listened to; Tick
recorded "ended (exit code 1)" and went on recording.

**The instrument is in the middle of an operation when the vendor program stops.**
Treat it exactly like any crash of the vendor software: bring the instrument to a safe state with its own controls
(front panel, stop/emergency button, interlocks) as the instrument's manual says, then restart the vendor program
from its icon. Tick never sends anything to the instrument; it only records what the vendor program does.

**The USB stick is pulled (or the session folder disappears) during a session.**
Everything written up to that moment stays readable: `events.jsonl` is written line by line and saved to the stick
at least once a second (marks, syncs and errors at once). Tick keeps running and shows the problem in red ("Cannot
write to the session folder" / "Could not write a screenshot"); it does not show "insert disk" boxes. The vendor
program is not affected. *What you do:* press Stop, put the stick back, press Start (a new session folder). Windows
may say the stick should be checked; let it. *Tested:* session folder deleted while recording and listening; Tick
kept running and reported it; the test program finished normally. *Not tested:* a real stick pulled on real
Windows.

**The disk or stick is full.** Below 30 MB free Tick pauses the screenshots (and says so); the much smaller event
file continues; screenshots resume above 50 MB free. *Not tested.*

**Antivirus warnings.** To listen, Tick loads its DLL into another program; some antivirus products treat that as
suspicious and may block or delete `Tick.exe`, even if you only want screen recording. Do not switch the antivirus
off on a PC that is on a network. Ask whoever looks after the PC to allow `Tick.exe`, or use `TickScreen.exe`
(no DLL, no loading into other programs), or film the screen with LibreTap's camera. *Not tested* with any
antivirus product.

**The PC is very old and slow.** Screenshots run at low priority so the vendor program comes first. If the PC feels
slower anyway, open *Advanced...* and take a screenshot every 1000 or 2000 ms instead of 500; or press Stop. The
listener adds only a memory copy to each serial call (not measured on an old PC). *Not tested* on a really old PC (Pentium II class).

## Before your first session

1. **Back up the vendor program's settings.** Copy its program folder (or at least its `.ini`/`.cfg` files) and
   any data folder to another place. Tick copies the small settings files from the program's own folder into the
   session automatically when you use *Start a program...*, but not settings kept elsewhere (other folders, the
   registry) - so make your own backup too.
2. **Look at the vendor program's icon first** (right-click > Properties): if it starts the program with extra
   arguments or in a different "Start in" folder, Tick's *Start a program...* does not know that. Use a shortcut to
   `Tick.exe /run "C:\path\VENDOR.EXE" <the same arguments>` instead, or record the screen only.
3. **Try it first on a measurement that does not matter**, ideally with the instrument idle or in a safe standby
   state: start with screen recording only, then once with *Start a program...*.
4. **Keep the instrument's own stop and emergency controls in reach**, as always when running its software.
5. Check there is free space on the stick (an hour of screen recording usually takes a few MB; much more if the
   screen changes all the time).

## Tested and not tested (Tick 0.2.0, 2026-11-26)

| Case | Tested |
|---|---|
| Tick killed while listening: program keeps working, session readable | yes, Wine (Windows 2000 mode) |
| Tick closed normally while listening: program keeps working | yes, Wine |
| Listened program killed: Tick notes it, keeps recording | yes, Wine |
| Session folder removed while recording and listening | yes, Wine (folder deleted; not a real stick) |
| Session readable after Tick is killed mid-recording | yes, Wine |
| Disk full | no (code path written, not exercised) |
| Antivirus | no |
| 64-bit test program started from Tick (Windows 7 mode) | yes, Wine (normal run; worst cases only with 32-bit) |
| TickScreen.exe; "vendor program's windows only" pictures | yes, Wine |
| Listen to a running program (attach), Pick the program's window | no |
| Real Windows 2000/XP/7/10/11, real vendor program, real instrument | **no** |
| Windows 95/98 | no |
| Slow PC (Pentium II) | no |

The test scripts are `tests/wine-test.sh`, `tests/wine-safety.sh` and `tests/wine-test64.sh`. The worst cases
above were run with Tick 0.1.0 and again with 0.2.0 (same results), with a 32-bit test program.
