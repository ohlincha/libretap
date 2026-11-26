#!/bin/sh
# Wine test of Tick's network and USB/device listener (32- and 64-bit programs):
# fakedevices.exe / fakedevices64.exe talk to a fake network instrument (TCP + UDP on
# 127.0.0.1) and use a device handle (\\.\NUL) with WriteFile/ReadFile/DeviceIoControl,
# HidD_* and WinUsb_* (those fail without a real device; the calls are still recorded).
# Usage: tests/wine-test-netusb.sh WINVER SCRATCH   (like wine-test.sh)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-win7}
scratch=${2:-/tmp/tick-wine-test}
WINE=${WINE:-wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX=${WINEPREFIX:-$scratch/pfx} WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/netusb"
rm -rf "$t"; mkdir -p "$t"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/fakedevices.exe" "$here/build/fakedevices64.exe" "$t/"
export WINE WINESERVER here scratch t
export TICK_TEST_NO_WINUSB=1    # Wine's winusb.dll has no WinUsb_Initialize (aborts the program)
cat > "$scratch/netusb-$ver.sh" <<'EOS'
cd "$t"
python3 -I "$here/tests/fake_net_instrument.py" 5800 120 &
sleep 1
for prog in fakedevices.exe fakedevices64.exe; do
  for what in "net 5800" usb; do
    echo "== $prog $what"
    "$WINE" Tick.exe /run "C:\\netusb\\$prog" $what &
    sleep 7
    "$WINE" tickctl.exe close
    sleep 2
  done
done
"$WINESERVER" -k
EOS
timeout 300 xvfb-run -a -s "-screen 0 800x600x24" sh "$scratch/netusb-$ver.sh" 2>&1 | grep -vE "^[0-9a-f]{4}:|libEGL"
echo "fakedevices.log:"; cat "$t/fakedevices.log"
for d in "$t"/TickSessions/*/; do
    echo "-- $d"
    python3 -I "$here/tests/check_session.py" "$d" || echo "CHECK FAILED"
    grep -vE '"kind":"(frame|input)"' "$d/events.jsonl" | cut -c1-230
    grep -E '"api-(net|usb)"' "$d/session.json" | cut -c1-200
done
