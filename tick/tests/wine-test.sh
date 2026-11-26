#!/bin/sh
# End-to-end test of Tick under Wine in a virtual X display (Xvfb).
#   tests/wine-test.sh WINVER [SCRATCH]       WINVER: win2k | winxp | win7 | win10
# Needs: build/Tick.exe build/tickctl.exe build/fakevendor.exe (make all tools-test),
# a Wine (WINE=/path/to/wine, default: wine on PATH), xvfb-run, xdotool, import, python3.
# Steps: screen recording with notepad typing, Mark (button and Enter), Sync,
# Stop/Start, then "Start a program" (/run) with fakevendor talking to a fake
# instrument on a pty mapped to COM1, then close.  Each session is checked
# with check_session.py; the serial events are printed.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-win2k}
scratch=${2:-/tmp/tick-wine-test}
WINE=${WINE:-wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX=${WINEPREFIX:-$scratch/pfx} WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$scratch"
[ -d "$WINEPREFIX/drive_c/windows/syswow64" ] || (unset DISPLAY; "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w)
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$scratch/com1-link" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/tick-$ver"
rm -rf "$t"; mkdir -p "$t"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/fakevendor.exe" "$t/"
out="$scratch/$ver"; rm -rf "$out"; mkdir -p "$out"

cat > "$scratch/run-$ver.sh" <<EOF
set -x
cd "$t"
ctl() { "$WINE" tickctl.exe "\$@" 2>/dev/null; }
"$WINE" Tick.exe /start &
sleep 8
import -window root "$out/tick-window.png"
"$WINE" notepad &
sleep 4
xdotool type --delay 40 "Hello from the instrument PC"
sleep 1
ctl mark "first mark"
ctl enter "note typed, Enter pressed"
sleep 1
ctl sync
sleep 0.3
import -window root "$out/sync-flash.png"
sleep 2
ctl stop
sleep 1
ctl start
sleep 2
ctl close
sleep 2
python3 -I "$here/tests/fake_instrument.py" "$scratch/com1-link" 60 &
sleep 1
"$WINE" Tick.exe /run "C:\\\\tick-$ver\\\\fakevendor.exe" COM1 4 300 &
sleep 12
import -window root "$out/listening.png"
ctl close
sleep 3
ctl close
sleep 2
"$WINESERVER" -k
EOF
timeout 300 xvfb-run -a -s "-screen 0 1024x768x24" sh "$scratch/run-$ver.sh" > "$out/run.log" 2>&1 || true
for d in "$t"/TickSessions/*/; do
    python3 -I "$here/tests/check_session.py" "$d" "$out/$(basename "$d").png"
    grep -E '"kind":"(open|close|config|data|call|mark|sync|error)"' "$d/events.jsonl" | cut -c1-200 || true
    grep -E 'TickHook|Started|ended|stopped' "$d/events.jsonl" | cut -c1-200 || true
done
echo "screenshots of the X display: $out"
