#!/bin/sh
# Wine tests of Tick 0.2 additions (run like wine-test.sh, normally with win7):
#  1  64-bit vendor program: Tick /run fakevendor64.exe -> api-serial events via TickHook64.dll
#  2  TickEyes.exe /start: screen-only build, recorder "TickEyes"
#  3  program-windows-only capture: Tick.ini program_windows_only=1, Tick /run notepad.exe
#     (a 64-bit program under this Wine) -> pictures show notepad, the rest black; Tick's own
#     window is not masked grey here, so it must come out black (checked below)
# Usage: tests/wine-test64.sh WINVER SCRATCH
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-win7}
scratch=${2:-/tmp/tick-wine-test}
WINE=${WINE:-wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX=${WINEPREFIX:-$scratch/pfx} WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$scratch/com1-link" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
base="$WINEPREFIX/drive_c"
for d in t64 tscreen tprog; do rm -rf "$base/$d"; mkdir -p "$base/$d"; done
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/fakevendor64.exe" "$base/t64/"
cp "$here/build/TickEyes.exe" "$here/build/tickctl.exe" "$base/tscreen/"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$base/tprog/"
printf '[tick]\nprogram_windows_only=1\nhide_tick_window=0\n' > "$base/tprog/Tick.ini"
out="$scratch/t64-$ver"; rm -rf "$out"; mkdir -p "$out"
export WINE WINESERVER here scratch out base

cat > "$scratch/t64-$ver.sh" <<'EOS'
python3 -I "$here/tests/fake_instrument.py" "$scratch/com1-link" 120 &
sleep 1
echo "== 1: 64-bit program"
cd "$base/t64"
"$WINE" Tick.exe /run 'C:\t64\fakevendor64.exe' COM1 4 300 &
sleep 14
"$WINE" tickctl.exe close
sleep 3
echo "== 2: TickEyes"
cd "$base/tscreen"
"$WINE" TickEyes.exe /start &
sleep 6
import -window root "$out/tickscreen.png"
"$WINE" tickctl.exe mark "screen only"
sleep 1
"$WINE" tickctl.exe close
sleep 3
echo "== 3: program windows only"
cd "$base/tprog"
"$WINE" Tick.exe /run 'C:\windows\notepad.exe' &
sleep 6
xdotool type --delay 40 "only this window is recorded"
sleep 3
import -window root "$out/progonly-display.png"
"$WINE" tickctl.exe close
sleep 3
"$WINESERVER" -k
EOS
timeout 300 xvfb-run -a -s "-screen 0 1024x768x24" sh "$scratch/t64-$ver.sh" 2>&1 \
    | grep -vE "^[0-9a-f]{4}:|libEGL" | tee "$out/run.log"
for d in "$base"/t64/TickSessions/*/ "$base"/tscreen/TickSessions/*/ "$base"/tprog/TickSessions/*/; do
    echo "-- $d"
    python3 -I "$here/tests/check_session.py" "$d" "$out/$(basename "$d").png" || echo "CHECK FAILED"
    grep -E '"kind":"(status|error|open|config|mark)"' "$d/events.jsonl" | cut -c1-170
    grep -E '"recorder"|"capture' "$d/session.json"
    ls "$d/tick" 2>/dev/null || true
done
# Tick's window (bottom right, not notepad's) must be black in the program-only recording
d=$(ls -d "$base"/tprog/TickSessions/*/ | tail -1)
python3 -I - "$out/$(basename "$d").png" <<'EOP'
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", data[16:24])
idat, pos = b"", 8
while pos < len(data):
    n, = struct.unpack(">I", data[pos:pos + 4]); t = data[pos + 4:pos + 8]
    if t == b"IDAT": idat += data[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat)          # written by check_session.py with filter 0
px = lambda x, y: raw[y * (w * 3 + 1) + 1 + x * 3: y * (w * 3 + 1) + 4 + x * 3]
tick_area = [px(x, y) for x in range(800, 1000, 7) for y in range(600, 740, 7)]
note_area = [px(x, y) for x in range(20, 600, 9) for y in range(100, 400, 9)]
black = sum(p == b"\0\0\0" for p in tick_area) / len(tick_area)
white = sum(p == b"\xff\xff\xff" for p in note_area) / len(note_area)
print("program-only check: Tick's window area %.0f%% black, notepad area %.0f%% white -> %s" % (
    100 * black, 100 * white, "ok" if black > 0.99 and white > 0.9 else "FAILED"))
EOP
